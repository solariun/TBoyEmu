// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Cpu.cpp
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Implements the ARM7TDMI interpreter declared in Cpu.h (see Cpu.h for the
//  interface and the register/CPSR layout).  The file is organized as:
//    - modes ........ register banking (switchMode, SPSR, User-bank access)
//    - execution .... branchTo, IRQ entry, step() (fetch + pipeline model),
//                     condition codes, barrel shifter, flag helpers
//    - ARM .......... executeArm() decoder + one method per instruction group
//    - Thumb ........ executeThumb(), formats 1-19 in GBATEK order
//  There is no pre-decoded lookup table: each opcode is matched against bit
//  masks in a fixed order every time.  The order matters because several
//  ARM encodings overlap (e.g. multiplies and halfword transfers live inside
//  the data-processing encoding space).
//
//  How it fits in
//  --------------
//  Gba::runFrame() calls step() repeatedly.  step() fetches through
//  Bus::fetch16/fetch32 (which charge instruction-fetch wait states), runs
//  the instruction (whose loads/stores go through Bus::read*/write*, also
//  charged), then returns Bus::takeCycles() + internal cycles.  SWI opcodes
//  are passed to the SwiHandler (gba::Bios) instead of jumping to 0x08.
//  The real IRQ path is emulated: enterIrq() jumps to 0x18, where Bus.cpp
//  has placed a tiny copy of the BIOS IRQ dispatcher (see kBiosCode there).
//
//  Hardware notes
//  --------------
//  - Pipeline-visible PC (GBATEK "ARM CPU Register Set"): while an ARM
//    instruction executes, reading r15 gives its address + 8; in Thumb, + 4.
//    step() stores exactly that value in r_[15] before executing, so the
//    instruction handlers can just read r_[15].  The address of the next
//    instruction is kept in pc_ (already advanced by 4 or 2).  Exceptions:
//      * ARM data processing with a register-specified shift reads r15 as
//        address + 12 (one extra pipeline step).
//      * STR/STRH/STM storing r15 store address + 12 (ARM) -- coded as
//        r_[15] + 4.
//  - Cycle counting (GBATEK "ARM CPU Instruction Cycle Times"): each
//    instruction costs its fetch (N = non-sequential or S = sequential)
//    plus its data accesses plus "I" internal cycles.  The bus counts the
//    accesses; internalCycles_ counts the I cycles.  Only the first fetch
//    after a branch is non-sequential in this model; data accesses between
//    fetches do not break sequentiality (an approximation).
//  - IRQ entry: see enterIrq().  The IRQ is taken between instructions when
//    IME=1, (IE & IF) != 0 and the CPSR I bit is clear.
//  - BIOS read protection: while executing ARM code inside the BIOS area
//    (pc < 0x4000), step() asks the bus to remember the BIOS word at
//    pc + 8, i.e. the opcode the real CPU would have prefetched.  Reads of
//    the BIOS made later from outside the BIOS return that latched value.
//
//  Contributing
//  ------------
//  - Keep the decode order in executeArm()/executeThumb(); moving a test
//    earlier or later can mis-decode another instruction class.
//  - Every write to r15 must use writeReg()/branchTo(), never r_[15] = ...;
//    r_[15] is only the pipeline value for the instruction being executed.
//  - Quirks covered by the jsmolka tests (keep them): misaligned LDR rotate,
//    misaligned LDRH rotate, misaligned LDRSH = LDRSB, empty register list
//    (transfers r15, base +/- 0x40), STM with the base in the list,
//    LDM with the base in the list (no writeback), shifter carry-out rules.
//  - Test: ./tboyemu --headless --frames 120 --screenshot out.png arm.gba
//    (and thumb.gba, memory.gba, bios.gba) must say "All tests passed".
// ============================================================================
#include "Cpu.h"

namespace gba {

namespace {
// CPSR bits (GBATEK "ARM CPU Flags & Condition Field").
const u32 FN = 0x80000000u;  // bit 31 N: result negative
const u32 FZ = 0x40000000u;  // bit 30 Z: result zero
const u32 FC = 0x20000000u;  // bit 29 C: carry / not borrow / shifter carry-out
const u32 FV = 0x10000000u;  // bit 28 V: signed overflow
const u32 FI = 0x00000080u;  // bit 7  I: 1 = IRQs disabled
const u32 FT = 0x00000020u;  // bit 5  T: 1 = Thumb state

// Rotate right by n bits (n taken modulo 32; ror(v, 0) == v).
inline u32 ror(u32 v, u32 n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

// Number of set bits in the low 16 bits (size of an LDM/STM register list).
inline int popcount16(u32 v) {
    int c = 0;
    for (v &= 0xFFFF; v; v &= v - 1) ++c;
    return c;
}
}  // namespace

Cpu::Cpu(Bus& bus, Interrupts& interrupts) : bus_(bus), interrupts_(interrupts), swiHandler_(0) { reset(); }

void Cpu::reset() {
    for (int i = 0; i < 16; ++i) r_[i] = 0;
    for (int i = 0; i < 6; ++i) spsrBank_[i] = bank13_[i] = bank14_[i] = 0;
    for (int i = 0; i < 5; ++i) fiqBank_[i] = userBank_[i] = 0;
    // Stack pointers set up by the BIOS before it jumps to the cartridge.
    // (GBATEK "BIOS RAM Usage": SP_svc = 0x03007FE0, SP_irq = 0x03007FA0,
    // SP_usr/sys = 0x03007F00.)  CPSR = System mode, ARM state, I = F = 0.
    // The System entry lives in slot 0 (shared with User); since System is
    // the current mode, the live value is r_[13].
    cpsr_ = System;
    bank13_[bankIndex(Supervisor)] = 0x03007FE0;
    bank13_[bankIndex(Irq)] = 0x03007FA0;
    bank13_[bankIndex(System)] = 0x03007F00;
    r_[13] = 0x03007F00;
    // Execution starts at the first byte of the cartridge ROM.
    pc_ = 0x08000000;
    instructionAddr_ = pc_;
    branched_ = false;
    sequential_ = false;
    halted_ = false;
    internalCycles_ = 0;
}

// --------------------------------------------------------------- modes ----

// Bank slot for each mode: 0 = User/System (they share registers),
// 1 = FIQ, 2 = IRQ, 3 = Supervisor, 4 = Abort, 5 = Undefined.
// Invalid mode values fall back to slot 0.
int Cpu::bankIndex(u32 mode) {
    switch (mode & 0x1F) {
        case Fiq: return 1;
        case Irq: return 2;
        case Supervisor: return 3;
        case Abort: return 4;
        case Undefined: return 5;
        default: return 0;  // User / System
    }
}

// Changes only the CPSR mode bits.  If the new mode uses a different bank,
// the live r13/r14 go back to the old mode's slot and the new mode's r13/r14
// are loaded into r_[].  FIQ additionally banks r8-r12: leaving FIQ saves
// FIQ's r8-r12 and restores everyone else's; entering FIQ does the reverse.
// The SPSR is not touched here (callers handle it).
void Cpu::switchMode(u32 mode) {
    mode &= 0x1F;
    int oldBank = bankIndex(cpsr_);
    int newBank = bankIndex(mode);
    if (oldBank != newBank) {
        bank13_[oldBank] = r_[13];
        bank14_[oldBank] = r_[14];
        if (oldBank == 1) {
            for (int i = 0; i < 5; ++i) {
                fiqBank_[i] = r_[8 + i];
                r_[8 + i] = userBank_[i];
            }
        }
        if (newBank == 1) {
            for (int i = 0; i < 5; ++i) {
                userBank_[i] = r_[8 + i];
                r_[8 + i] = fiqBank_[i];
            }
        }
        r_[13] = bank13_[newBank];
        r_[14] = bank14_[newBank];
    }
    cpsr_ = (cpsr_ & ~0x1Fu) | mode;
}

// Full CPSR write (flags, I/F/T bits and mode).  Banks are swapped first,
// using the mode bits of the new value, then the whole value is stored.
void Cpu::setCpsr(u32 value) {
    switchMode(value & 0x1F);
    cpsr_ = value;
}

// User and System have no SPSR; reading it there returns the CPSR (what
// this emulator chooses for that unpredictable case).
u32 Cpu::spsr() const {
    int b = bankIndex(cpsr_);
    return b == 0 ? cpsr_ : spsrBank_[b];
}

void Cpu::setSpsr(u32 value) {
    int b = bankIndex(cpsr_);
    if (b != 0) spsrBank_[b] = value;
}

// Reads User-mode register i while in another mode (for "STM ...^").
// r8-r12 differ only in FIQ mode; r13/r14 differ in every privileged mode
// except System.  Otherwise the live register is the User one.
u32 Cpu::userReg(int i) const {
    int b = bankIndex(cpsr_);
    if (i >= 8 && i <= 12 && b == 1) return userBank_[i - 8];
    if ((i == 13 || i == 14) && b != 0) return i == 13 ? bank13_[0] : bank14_[0];
    return r_[i];
}

// Writes User-mode register i while in another mode (for "LDM ...^" without
// r15 in the list).  Same banking rules as userReg().
void Cpu::setUserReg(int i, u32 v) {
    int b = bankIndex(cpsr_);
    if (i >= 8 && i <= 12 && b == 1) {
        userBank_[i - 8] = v;
    } else if ((i == 13 || i == 14) && b != 0) {
        (i == 13 ? bank13_[0] : bank14_[0]) = v;
    } else {
        writeReg(i, v);
    }
}

// ----------------------------------------------------------- execution ----

// Jump: the target is aligned for the CURRENT instruction set (bit 0
// cleared for Thumb, bits 0-1 for ARM).  BX sets/clears T before calling
// this, so it aligns for the new set.  Marking branched_ makes the next
// fetch non-sequential (a pipeline refill costs an N cycle on hardware).
void Cpu::branchTo(u32 addr) {
    pc_ = thumb() ? (addr & ~1u) : (addr & ~3u);
    branched_ = true;
}

// IRQ exception entry (GBATEK "ARM CPU Exceptions"):
//   R14_irq = address of the next instruction + 4
//   SPSR_irq = CPSR
//   CPSR: mode = IRQ, T = 0 (ARM state), I = 1 (further IRQs masked)
//   PC = 0x00000018 (IRQ vector)
// Called between instructions, so pc_ is the next instruction to execute;
// the handler's final "SUBS PC, LR, #4" therefore resumes exactly at pc_,
// in the original state (T bit restored from SPSR) -- whether the
// interrupted code was ARM or Thumb.
// At 0x18 the bus provides a branch to the BIOS dispatcher at 0x128, which
// saves r0-r3/r12/lr, calls the game's handler whose address the game stored
// at 0x03007FFC, then returns from the exception (see Bus.cpp kBiosCode).
void Cpu::enterIrq() {
    u32 returnAddr = pc_ + 4;  // SUBS PC, LR, #4 resumes at pc_
    u32 oldCpsr = cpsr_;
    switchMode(Irq);
    spsrBank_[bankIndex(Irq)] = oldCpsr;
    cpsr_ = (cpsr_ & ~FT) | FI;
    r_[14] = returnAddr;
    pc_ = 0x18;
    sequential_ = false;
}

// Executes one instruction and returns the cycles it took (see Cpu.h).
//
// Order of events:
//  1. HALT: if halted and no enabled interrupt is pending (IE & IF == 0,
//     IME is ignored for waking), return 0 so the caller can skip time.
//     Otherwise leave HALT.
//  2. IRQ: if IME = 1, (IE & IF) != 0 and CPSR.I = 0, take the exception
//     now; the instruction executed below is then the one at 0x18.
//  3. Fetch the opcode at pc_ (charged as sequential or not), set r_[15] to
//     the pipeline value (pc_ + 8 ARM / pc_ + 4 Thumb), advance pc_ to the
//     next instruction, then decode and execute.
//  4. Collect the cycles the bus charged plus the internal cycles.
int Cpu::step() {
    if (halted_) {
        if (!interrupts_.pending()) return 0;  // caller skips time ahead
        halted_ = false;
    }
    if (interrupts_.shouldInterrupt() && !(cpsr_ & FI)) enterIrq();

    instructionAddr_ = pc_;
    // The bus needs the PC to decide whether BIOS reads are allowed.
    bus_.setPc(pc_);
    branched_ = false;
    internalCycles_ = 0;
    if (thumb()) {
        u16 op = bus_.fetch16(pc_, sequential_);
        r_[15] = pc_ + 4;
        pc_ += 2;
        executeThumb(op);
    } else {
        u32 op = bus_.fetch32(pc_, sequential_);
        // Inside the BIOS: latch the word the hardware would have prefetched
        // (two ARM instructions ahead).  This is what a later read of the
        // protected BIOS returns, e.g. 0xE55EC002 at 0x144 after the IRQ
        // dispatcher's "SUBS PC, LR, #4" at 0x13C.  Only ARM code runs in the
        // emulated BIOS, so Thumb fetches do not update the latch.
        if (pc_ < 0x4000) bus_.latchBiosAt(pc_ + 8);
        r_[15] = pc_ + 8;
        pc_ += 4;
        executeArm(op);
    }
    // A branch refills the pipeline: the next fetch is non-sequential.
    sequential_ = !branched_;
    // A write to HALTCNT (0x04000301) during this instruction halts the CPU
    // from the next step on.
    if (bus_.consumeHaltRequest()) halted_ = true;
    int cycles = bus_.takeCycles() + internalCycles_;
    return cycles > 0 ? cycles : 1;
}

// ARM condition codes (top 4 bits of every ARM opcode, bits 8-11 of the
// Thumb conditional branch).  GBATEK "ARM CPU Flags & Condition Field".
bool Cpu::condition(u32 cond) const {
    const bool n = (cpsr_ & FN) != 0, z = (cpsr_ & FZ) != 0, c = (cpsr_ & FC) != 0, v = (cpsr_ & FV) != 0;
    switch (cond) {
        case 0x0: return z;                 // EQ  equal
        case 0x1: return !z;                // NE  not equal
        case 0x2: return c;                 // CS/HS  unsigned >=
        case 0x3: return !c;                // CC/LO  unsigned <
        case 0x4: return n;                 // MI  negative
        case 0x5: return !n;                // PL  positive or zero
        case 0x6: return v;                 // VS  overflow
        case 0x7: return !v;                // VC  no overflow
        case 0x8: return c && !z;           // HI  unsigned >
        case 0x9: return !c || z;           // LS  unsigned <=
        case 0xA: return n == v;            // GE  signed >=
        case 0xB: return n != v;            // LT  signed <
        case 0xC: return !z && n == v;      // GT  signed >
        case 0xD: return z || n != v;       // LE  signed <=
        case 0xE: return true;              // AL  always
        default: return false;              // NV  never (reserved on ARMv4)
    }
}

// Barrel shifter (GBATEK "ARM Opcodes: Data Processing", shift types).
// Shift amount encodings that do not mean what they look like:
//   immediate LSL #0  -> no shift, C unchanged
//   immediate LSR #0  -> LSR #32  (result 0, C = bit 31)
//   immediate ASR #0  -> ASR #32  (result 0 or all ones, C = bit 31)
//   immediate ROR #0  -> RRX: rotate right by 1 through the carry flag
// With a register amount (immediate == false, only the low byte is used by
// the callers), 0 means "no shift, C unchanged", and amounts >= 32 follow
// the rules below (LSL/LSR: result 0; ASR: sign fill; ROR: modulo 32).
u32 Cpu::shift(u32 value, u32 type, u32 amount, bool immediate, bool& carry) const {
    carry = flagC();
    switch (type) {
        case 0:  // LSL
            if (amount == 0) return value;
            if (amount < 32) {
                carry = (value >> (32 - amount)) & 1;
                return value << amount;
            }
            // LSL #32: C = bit 0; LSL by more than 32: C = 0.
            carry = amount == 32 ? (value & 1) : false;
            return 0;
        case 1:  // LSR
            if (amount == 0) {
                if (!immediate) return value;
                amount = 32;  // LSR #0 encodes LSR #32
            }
            if (amount < 32) {
                carry = (value >> (amount - 1)) & 1;
                return value >> amount;
            }
            // LSR #32: C = bit 31; LSR by more than 32: C = 0.
            carry = amount == 32 ? (value >> 31) != 0 : false;
            return 0;
        case 2:  // ASR
            if (amount == 0) {
                if (!immediate) return value;
                amount = 32;  // ASR #0 encodes ASR #32
            }
            if (amount < 32) {
                carry = (value >> (amount - 1)) & 1;
                return static_cast<u32>(static_cast<s32>(value) >> amount);
            }
            // ASR by 32 or more: every bit (and C) becomes the sign bit.
            carry = (value >> 31) != 0;
            return carry ? 0xFFFFFFFFu : 0;
        default:  // ROR / RRX
            if (amount == 0) {
                if (!immediate) return value;
                // RRX: result = C:value >> 1, new C = old bit 0.
                bool c = flagC();  // RRX
                carry = value & 1;
                return (value >> 1) | (c ? 0x80000000u : 0);
            }
            amount &= 31;
            if (amount == 0) {
                // Register amount 32, 64, ...: value unchanged, C = bit 31.
                carry = (value >> 31) != 0;
                return value;
            }
            carry = (value >> (amount - 1)) & 1;
            return ror(value, amount);
    }
}

// The single adder used by ADD/ADC/SUB/SBC/RSB/RSC/CMP/CMN and the Thumb
// arithmetic.  Subtraction x - y is computed as x + ~y + 1 (SBC/RSC use
// carryIn = C instead of 1), so the carry out is the ARM "not borrow".
// V is set when a and b have the same sign and the result's sign differs.
u32 Cpu::addWithFlags(u32 a, u32 b, u32 carryIn, bool setFlags) {
    u64 wide = static_cast<u64>(a) + b + carryIn;
    u32 result = static_cast<u32>(wide);
    if (setFlags) {
        setNZ(result);
        setC((wide >> 32) != 0);
        bool overflow = ((~(a ^ b) & (a ^ result)) >> 31) != 0;
        cpsr_ = overflow ? (cpsr_ | FV) : (cpsr_ & ~FV);
    }
    return result;
}

// Misaligned word load: the bus returns the aligned word, which the
// ARM7TDMI rotates so that the addressed byte ends up in bits 0-7.
u32 Cpu::loadWord(u32 addr) { return ror(bus_.read32(addr), (addr & 3) * 8); }

// Multiply early termination (GBATEK "ARM CPU Instruction Cycle Times"):
// m = 1 if bits 8-31 of the multiplier are all 0 (or all 1 when signed),
// 2 if bits 16-31 are, 3 if bits 24-31 are, otherwise 4.
// Inverting a negative signed value turns "all 1" into "all 0".
int Cpu::multiplyCycles(u32 rs, bool isSigned) const {
    if (isSigned && (rs & 0x80000000u)) rs = ~rs;
    if ((rs & 0xFFFFFF00u) == 0) return 1;
    if ((rs & 0xFFFF0000u) == 0) return 2;
    if ((rs & 0xFF000000u) == 0) return 3;
    return 4;
}

// ------------------------------------------------------------------ ARM ----

// ARM decoder.  Every ARM opcode is 32 bits: cond(31-28) then a class
// selected by bits 27-20 and 7-4.  The tests below go from the most specific
// pattern to the most general (GBATEK "ARM Binary Opcode Format"):
//
//   cond 0001 0010 1111 1111 1111 0001 Rn     BX               (exact match)
//   cond 101L offset24                       B / BL
//   cond 1111 comment24                      SWI
//   cond 0000 00AS Rd Rn Rs 1001 Rm           MUL / MLA
//   cond 0000 1UAS RdHi RdLo Rs 1001 Rm       UMULL/UMLAL/SMULL/SMLAL
//   cond 0001 0B00 Rn Rd 0000 1001 Rm         SWP / SWPB
//   cond 000P UIWL Rn Rd .... 1SH1 ....       LDRH/STRH/LDRSB/LDRSH (SH != 0)
//   cond 0001 0P00 1111 Rd 0000 0000 0000     MRS
//   cond 00I1 0P10 field 1111 operand12       MSR
//   cond 00I opcode4 S Rn Rd operand2         data processing
//   cond 01IP UBWL Rn Rd offset12             LDR / STR (I=1 with bit 4=1 is undefined)
//   cond 100P USWL Rn register-list           LDM / STM
//   cond 11.. ....                            coprocessor (ignored)
//
// The multiply, swap, halfword and PSR patterns are all inside the
// "00" data-processing space, so they must be tested before it.
void Cpu::executeArm(u32 op) {
    // Condition field false: the instruction is skipped (just its fetch).
    if (!condition(op >> 28)) return;

    if ((op & 0x0FFFFFF0u) == 0x012FFF10u) return armBranchExchange(op);
    if ((op & 0x0E000000u) == 0x0A000000u) return armBranch(op);
    if ((op & 0x0F000000u) == 0x0F000000u) {
        // SWI: the BIOS function number is in bits 16-23 (the BIOS reads it
        // from the comment field; bits 0-15 are ignored on the GBA).
        if (swiHandler_) swiHandler_->swi((op >> 16) & 0xFF);
        return;
    }
    if ((op & 0x0FC000F0u) == 0x00000090u) return armMultiply(op);
    if ((op & 0x0F8000F0u) == 0x00800090u) return armMultiplyLong(op);
    if ((op & 0x0FB00FF0u) == 0x01000090u) return armSwap(op);
    // Bits 7 and 4 set with SH (bits 6-5) != 0; SH == 0 is multiply/swap.
    if ((op & 0x0E000090u) == 0x00000090u && (op & 0x60u)) return armHalfwordTransfer(op);
    // First pattern: MRS.  Second: MSR (register or immediate form).
    if ((op & 0x0FBF0FFFu) == 0x010F0000u || (op & 0x0DB0F000u) == 0x0120F000u) return armPsrTransfer(op);
    if ((op & 0x0C000000u) == 0x00000000u) return armDataProcessing(op);
    if ((op & 0x0C000000u) == 0x04000000u) {
        if ((op & 0x02000010u) == 0x02000010u) return;  // undefined instruction
        return armSingleTransfer(op);
    }
    if ((op & 0x0E000000u) == 0x08000000u) return armBlockTransfer(op);
    // Coprocessor instructions: no coprocessor on the GBA.
}

// BX Rn: branch to Rn; bit 0 of Rn selects the new state (1 = Thumb).
void Cpu::armBranchExchange(u32 op) {
    u32 target = r_[op & 0xF];
    if (target & 1) {
        cpsr_ |= FT;
    } else {
        cpsr_ &= ~FT;
    }
    branchTo(target);
}

// B / BL: PC = (instruction + 8) + signed 24-bit offset * 4.
// BL (bit 24) also stores the return address (instruction + 4) in LR.
void Cpu::armBranch(u32 op) {
    s32 offset = static_cast<s32>(op << 8) >> 6;  // sign-extended 24 bits * 4
    if (op & 0x01000000u) r_[14] = pc_;           // BL: address of the next instruction
    branchTo(r_[15] + static_cast<u32>(offset));
}

// Data processing: Rd = Rn <op> Operand2 (GBATEK "ARM Opcodes: Data
// Processing (ALU)").
//   bits 24-21 opcode, bit 20 S (set flags), 19-16 Rn, 15-12 Rd,
//   bit 25 I: 1 = Operand2 is an 8-bit immediate rotated right by
//               2 * bits 11-8;
//             0 = Operand2 is Rm (bits 3-0) shifted by type (bits 6-5) by
//               either an immediate (bits 11-7, bit 4 = 0) or the low byte
//               of Rs (bits 11-8, bit 4 = 1).
// Logical ops (AND, EOR, TST, TEQ, ORR, MOV, BIC, MVN) set C from the
// shifter; arithmetic ops set C and V from the adder.
// Rd = r15 with S set is the "return from exception" form (e.g.
// "SUBS PC, LR, #4"): CPSR is restored from the SPSR.
void Cpu::armDataProcessing(u32 op) {
    const u32 opcode = (op >> 21) & 0xF;
    const bool setFlags = (op & 0x00100000u) != 0;
    const int rn = static_cast<int>((op >> 16) & 0xF);
    const int rd = static_cast<int>((op >> 12) & 0xF);

    u32 operand2;
    bool carry = flagC();
    u32 rnValue = r_[rn];
    if (op & 0x02000000u) {
        // Rotated immediate.  A non-zero rotation also sets the shifter
        // carry to bit 31 of the result.
        u32 rotate = ((op >> 8) & 0xF) * 2;
        operand2 = ror(op & 0xFF, rotate);
        if (rotate) carry = (operand2 >> 31) != 0;
    } else {
        const int rm = static_cast<int>(op & 0xF);
        const u32 type = (op >> 5) & 3;
        u32 rmValue = r_[rm];
        if (op & 0x10u) {
            // Register-specified shift: one extra cycle, PC reads 12 ahead.
            ++internalCycles_;
            if (rm == 15) rmValue += 4;
            if (rn == 15) rnValue += 4;
            u32 amount = r_[(op >> 8) & 0xF] & 0xFF;
            operand2 = shift(rmValue, type, amount, false, carry);
        } else {
            operand2 = shift(rmValue, type, (op >> 7) & 0x1F, true, carry);
        }
    }

    u32 result = 0;
    bool logical = false;      // flags from the shifter (C) instead of the adder
    bool writeResult = true;   // false for the compare/test ops
    switch (opcode) {
        case 0x0: result = rnValue & operand2; logical = true; break;                              // AND
        case 0x1: result = rnValue ^ operand2; logical = true; break;                              // EOR
        case 0x2: result = addWithFlags(rnValue, ~operand2, 1, setFlags); break;                   // SUB
        case 0x3: result = addWithFlags(operand2, ~rnValue, 1, setFlags); break;                   // RSB
        case 0x4: result = addWithFlags(rnValue, operand2, 0, setFlags); break;                    // ADD
        case 0x5: result = addWithFlags(rnValue, operand2, flagC() ? 1 : 0, setFlags); break;      // ADC
        case 0x6: result = addWithFlags(rnValue, ~operand2, flagC() ? 1 : 0, setFlags); break;     // SBC
        case 0x7: result = addWithFlags(operand2, ~rnValue, flagC() ? 1 : 0, setFlags); break;     // RSC
        case 0x8: result = rnValue & operand2; logical = true; writeResult = false; break;         // TST
        case 0x9: result = rnValue ^ operand2; logical = true; writeResult = false; break;         // TEQ
        case 0xA: result = addWithFlags(rnValue, ~operand2, 1, true); writeResult = false; break;  // CMP
        case 0xB: result = addWithFlags(rnValue, operand2, 0, true); writeResult = false; break;   // CMN
        case 0xC: result = rnValue | operand2; logical = true; break;                              // ORR
        case 0xD: result = operand2; logical = true; break;                                        // MOV
        case 0xE: result = rnValue & ~operand2; logical = true; break;                             // BIC
        default: result = ~operand2; logical = true; break;                                        // MVN
    }

    if (setFlags && logical) {
        setNZ(result);
        setC(carry);
    }
    if (writeResult) {
        if (rd == 15) {
            // Restore CPSR first so that branchTo() aligns the target for
            // the restored instruction set (ARM or Thumb).
            if (setFlags) setCpsr(spsr());  // return from exception
            branchTo(result);
        } else {
            r_[rd] = result;
        }
    } else if (rd == 15 && setFlags) {
        setCpsr(spsr());  // e.g. TEQP-style: restore CPSR only
    }
}

// MRS Rd, CPSR/SPSR  and  MSR CPSR/SPSR_<fields>, Rm/#imm
// (GBATEK "ARM Opcodes: PSR Transfer (MRS, MSR)").
// bit 22 P: 0 = CPSR, 1 = SPSR of the current mode.
// MSR field mask, bits 19-16: f = flags (31-24), s = status (23-16),
// x = extension (15-8), c = control (7-0: I, F, T, mode).
void Cpu::armPsrTransfer(u32 op) {
    const bool useSpsr = (op & 0x00400000u) != 0;
    if ((op & 0x0FBF0FFFu) == 0x010F0000u) {  // MRS
        r_[(op >> 12) & 0xF] = useSpsr ? spsr() : cpsr_;
        return;
    }
    // MSR
    u32 value = (op & 0x02000000u) ? ror(op & 0xFF, ((op >> 8) & 0xF) * 2) : r_[op & 0xF];
    u32 mask = 0;
    if (op & 0x00080000u) mask |= 0xFF000000u;
    if (op & 0x00040000u) mask |= 0x00FF0000u;
    if (op & 0x00020000u) mask |= 0x0000FF00u;
    if (op & 0x00010000u) mask |= 0x000000FFu;
    if (useSpsr) {
        setSpsr((spsr() & ~mask) | (value & mask));
        return;
    }
    if ((cpsr_ & 0x1F) == User) mask &= 0xFF000000u;  // user mode: flags only
    u32 next = (cpsr_ & ~mask) | (value & mask);
    next = (next & ~FT) | (cpsr_ & FT);  // MSR cannot change the instruction set
    if (mask & 0xFF) {
        // The control byte changed: may be a mode switch, so bank registers.
        // Bit 4 of the mode is forced to 1 (all valid ARMv4T modes have it).
        setCpsr(next | 0x10);
    } else {
        cpsr_ = next;
    }
}

// MUL Rd, Rm, Rs  (Rd = Rm * Rs)  /  MLA Rd, Rm, Rs, Rn  (Rd = Rm * Rs + Rn)
// Fields: 19-16 Rd, 15-12 Rn (accumulate), 11-8 Rs, 3-0 Rm, bit 21 A, 20 S.
// S sets N and Z only (C is left unchanged here; on ARMv4 it is
// "meaningless").  Cycles: m (from Rs) + 1 more for MLA.
void Cpu::armMultiply(u32 op) {
    const int rd = static_cast<int>((op >> 16) & 0xF);
    const u32 rs = r_[(op >> 8) & 0xF];
    u32 result = r_[op & 0xF] * rs;
    internalCycles_ += multiplyCycles(rs, true);
    if (op & 0x00200000u) {  // MLA
        result += r_[(op >> 12) & 0xF];
        ++internalCycles_;
    }
    r_[rd] = result;
    if (op & 0x00100000u) setNZ(result);
}

// 64-bit multiplies: RdHi:RdLo = Rm * Rs (+ RdHi:RdLo when accumulating).
// Fields: 19-16 RdHi, 15-12 RdLo, 11-8 Rs, 3-0 Rm, bit 22 U (1 = signed:
// SMULL/SMLAL), bit 21 A, bit 20 S (N = bit 63, Z = whole 64-bit result 0).
// Cycles: m + 1, plus 1 more when accumulating.
void Cpu::armMultiplyLong(u32 op) {
    const int hi = static_cast<int>((op >> 16) & 0xF);
    const int lo = static_cast<int>((op >> 12) & 0xF);
    const bool isSigned = (op & 0x00400000u) != 0;
    const u32 rs = r_[(op >> 8) & 0xF];
    const u32 rm = r_[op & 0xF];
    u64 result = isSigned ? static_cast<u64>(static_cast<s64>(static_cast<s32>(rm)) * static_cast<s32>(rs))
                          : static_cast<u64>(rm) * rs;
    internalCycles_ += multiplyCycles(rs, isSigned) + 1;
    if (op & 0x00200000u) {  // accumulate
        result += (static_cast<u64>(r_[hi]) << 32) | r_[lo];
        ++internalCycles_;
    }
    r_[lo] = static_cast<u32>(result);
    r_[hi] = static_cast<u32>(result >> 32);
    if (op & 0x00100000u) {
        cpsr_ = (cpsr_ & 0x3FFFFFFF) | (static_cast<u32>(result >> 32) & FN) | (result == 0 ? FZ : 0);
    }
}

// SWP Rd, Rm, [Rn]: atomic "Rd = [Rn]; [Rn] = Rm" (word, with the usual
// misaligned-load rotation) or SWPB (byte, bit 22).  One internal cycle.
void Cpu::armSwap(u32 op) {
    const u32 addr = r_[(op >> 16) & 0xF];
    const u32 source = r_[op & 0xF];
    const int rd = static_cast<int>((op >> 12) & 0xF);
    if (op & 0x00400000u) {  // SWPB
        u8 old = bus_.read8(addr);
        bus_.write8(addr, static_cast<u8>(source));
        r_[rd] = old;
    } else {
        u32 old = loadWord(addr);
        bus_.write32(addr, source);
        r_[rd] = old;
    }
    ++internalCycles_;
}

// Halfword and signed transfers (GBATEK "ARM Opcodes: Memory: Halfword,
// Doubleword, and Signed Data Transfer").
//   bit 24 P (1 = pre-index: add offset before the access),
//   bit 23 U (1 = add offset, 0 = subtract),
//   bit 22 I (1 = 8-bit immediate offset split in bits 11-8 and 3-0,
//             0 = offset register Rm in bits 3-0),
//   bit 21 W (write back the address; post-index always writes back),
//   bit 20 L (1 = load), bits 6-5 SH: 1 = unsigned halfword (LDRH/STRH),
//   2 = signed byte (LDRSB), 3 = signed halfword (LDRSH).
// Loads take one extra internal cycle.
void Cpu::armHalfwordTransfer(u32 op) {
    const bool pre = (op & 0x01000000u) != 0;
    const bool up = (op & 0x00800000u) != 0;
    const bool writeback = (op & 0x00200000u) != 0;
    const bool load = (op & 0x00100000u) != 0;
    const int rn = static_cast<int>((op >> 16) & 0xF);
    const int rd = static_cast<int>((op >> 12) & 0xF);
    const u32 sh = (op >> 5) & 3;
    const u32 offset = (op & 0x00400000u) ? (((op >> 4) & 0xF0) | (op & 0xF)) : r_[op & 0xF];

    u32 addr = r_[rn];
    const u32 stepped = up ? addr + offset : addr - offset;
    if (pre) addr = stepped;

    u32 value = 0;
    if (load) {
        if (sh == 1) {  // LDRH (misaligned: rotated)
            value = ror(bus_.read16(addr), (addr & 1) * 8);
        } else if (sh == 2) {  // LDRSB
            value = static_cast<u32>(static_cast<s32>(static_cast<s8>(bus_.read8(addr))));
        } else {  // LDRSH (misaligned: behaves like LDRSB)
            value = (addr & 1) ? static_cast<u32>(static_cast<s32>(static_cast<s8>(bus_.read8(addr))))
                               : static_cast<u32>(static_cast<s32>(static_cast<s16>(bus_.read16(addr))));
        }
        ++internalCycles_;
    } else {
        // STRH; storing r15 stores instruction + 12.
        u32 data = rd == 15 ? r_[15] + 4 : r_[rd];
        bus_.write16(addr, static_cast<u16>(data));
    }

    if (!pre) {
        r_[rn] = stepped;
    } else if (writeback) {
        r_[rn] = addr;
    }
    if (load) writeReg(rd, value);  // a loaded base register wins over writeback
}

// LDR / STR / LDRB / STRB (GBATEK "ARM Opcodes: Memory: Single Data
// Transfer").
//   bit 25 I (1 = offset is Rm shifted by an immediate, 0 = 12-bit
//   immediate), bit 24 P (pre-index), 23 U (add), 22 B (byte),
//   21 W (writeback; in post-index it would select the "T" user-mode
//   variant, which makes no difference here), 20 L (load).
// LDR of a misaligned word is rotated (loadWord).  Loading r15 jumps
// (staying in the current state, ARMv4 behavior).  Loads take one extra
// internal cycle.
void Cpu::armSingleTransfer(u32 op) {
    const bool pre = (op & 0x01000000u) != 0;
    const bool up = (op & 0x00800000u) != 0;
    const bool byte = (op & 0x00400000u) != 0;
    const bool writeback = (op & 0x00200000u) != 0;
    const bool load = (op & 0x00100000u) != 0;
    const int rn = static_cast<int>((op >> 16) & 0xF);
    const int rd = static_cast<int>((op >> 12) & 0xF);

    u32 offset;
    if (op & 0x02000000u) {
        // Shifted register offset; the shifter carry is discarded.
        bool carry;
        offset = shift(r_[op & 0xF], (op >> 5) & 3, (op >> 7) & 0x1F, true, carry);
    } else {
        offset = op & 0xFFF;
    }

    u32 addr = r_[rn];
    const u32 stepped = up ? addr + offset : addr - offset;
    if (pre) addr = stepped;

    u32 value = 0;
    if (load) {
        value = byte ? bus_.read8(addr) : loadWord(addr);
        ++internalCycles_;
    } else {
        // Storing r15 stores instruction + 12.
        u32 data = rd == 15 ? r_[15] + 4 : r_[rd];
        if (byte) {
            bus_.write8(addr, static_cast<u8>(data));
        } else {
            bus_.write32(addr, data);
        }
    }

    if (!pre) {
        r_[rn] = stepped;
    } else if (writeback) {
        r_[rn] = addr;
    }
    // Written last: when Rd == Rn the loaded value wins over writeback.
    if (load) writeReg(rd, value);
}

// LDM / STM (GBATEK "ARM Opcodes: Memory: Block Data Transfer").
//   bit 24 P (pre: step before each access), 23 U (up: increasing addresses),
//   22 S, 21 W (writeback), 20 L (load), 19-16 Rn, 15-0 register list.
// Registers are always transferred lowest-numbered first at the lowest
// address, so for "down" modes the start address is computed first and the
// loop always walks upwards:
//   IA: base ... base+4n-4      IB: base+4 ... base+4n
//   DA: base-4n+4 ... base      DB: base-4n ... base-4
// S bit: with LDM and r15 in the list, CPSR = SPSR after the transfer
// (exception return, e.g. "LDMFD SP!, {..., PC}^").  Otherwise it means
// "transfer the User-mode registers" from a privileged mode.
void Cpu::armBlockTransfer(u32 op) {
    const bool pre = (op & 0x01000000u) != 0;
    const bool up = (op & 0x00800000u) != 0;
    const bool psrOrUser = (op & 0x00400000u) != 0;
    const bool writeback = (op & 0x00200000u) != 0;
    const bool load = (op & 0x00100000u) != 0;
    const int rn = static_cast<int>((op >> 16) & 0xF);
    u32 list = op & 0xFFFF;

    const u32 base = r_[rn];
    int count = popcount16(list);
    u32 bytes = static_cast<u32>(count) * 4;
    if (list == 0) {
        // ARMv4 quirk: an empty list transfers R15 and moves the base by 0x40.
        list = 0x8000;
        bytes = 0x40;
    }
    u32 start;
    if (up) {
        start = pre ? base + 4 : base;
    } else {
        start = pre ? base - bytes : base - bytes + 4;
    }
    const u32 newBase = up ? base + bytes : base - bytes;
    const bool loadsPc = load && (list & 0x8000);
    const bool userBank = psrOrUser && !loadsPc;

    u32 addr = start;
    bool first = true;
    u32 loadedPc = 0;
    for (int i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        if (load) {
            u32 v = bus_.read32(addr);
            if (i == 15) loadedPc = v;
            if (userBank) {
                setUserReg(i, v);
            } else {
                writeReg(i, v);
            }
        } else {
            u32 v = userBank ? userReg(i) : r_[i];
            // Storing r15 stores instruction + 12.
            if (i == 15) v = r_[15] + 4;
            bus_.write32(addr, v);
            // STM with the base in the list stores the old base only if it
            // is the first register; the writeback happens after the first.
            if (first && writeback) r_[rn] = newBase;
        }
        first = false;
        addr += 4;
    }
    if (load) ++internalCycles_;

    // LDM with the base in the list: the loaded value wins, no writeback.
    if (writeback && (!load || !(list & (1u << rn)))) r_[rn] = newBase;
    if (loadsPc && psrOrUser) {
        setCpsr(spsr());  // LDM ..., {.., pc}^ : return from exception
        branchTo(loadedPc);  // aligned for the restored instruction set
    }
}

// ---------------------------------------------------------------- Thumb ----

// Thumb decoder.  Thumb opcodes are 16 bits and only r0-r7 are reachable
// by most formats.  The formats are numbered as in GBATEK "THUMB Binary
// Opcode Format" (also the ARM7TDMI data sheet):
//
//   000xx  1  move shifted register (LSL/LSR/ASR #imm)   xx != 11
//   00011  2  add / subtract (register or 3-bit immediate)
//   001xx  3  MOV/CMP/ADD/SUB Rd, #imm8
//   010000 4  ALU operations (16 ops on Rd, Rs)
//   010001 5  hi-register ADD/CMP/MOV, BX
//   01001  6  LDR Rd, [PC, #imm]
//   0101.. 7  LDR/STR(B) with register offset          (bit 9 = 0)
//   0101.. 8  LDRH/STRH/LDSB/LDSH with register offset (bit 9 = 1)
//   011..  9  LDR/STR(B) with 5-bit immediate offset
//   1000.  10 LDRH/STRH with immediate offset
//   1001.  11 LDR/STR Rd, [SP, #imm]
//   1010.  12 ADD Rd, PC/SP, #imm (load address)
//   10110000 13 ADD SP, #+/-imm
//   1011x10x 14 PUSH / POP
//   1100.  15 LDMIA / STMIA
//   1101 cond 16 conditional branch   (cond 1111 = format 17 SWI)
//   11100  18 unconditional branch
//   1111H  19 BL, as a pair of instructions (H = 0 high part, 1 low part)
//
// r_[15] reads as instruction + 4.  Where noted, the value is word-aligned
// with "& ~2" because PC-relative accesses use a word-aligned PC.
void Cpu::executeThumb(u16 op) {
    const u32 top5 = op >> 11;

    if (top5 < 3) {  // format 1: shift by immediate
        // top5 is directly the shift type: 0 LSL, 1 LSR, 2 ASR.  Always sets
        // N, Z and C (LSL #0 keeps C, LSR/ASR #0 mean #32).
        const int rd = op & 7;
        bool carry;
        u32 result = shift(r_[(op >> 3) & 7], top5, (op >> 6) & 0x1F, true, carry);
        r_[rd] = result;
        setNZ(result);
        setC(carry);
        return;
    }
    if (top5 == 3) {  // format 2: add / subtract
        // bit 10: operand is a 3-bit immediate (1) or register (0);
        // bit 9: subtract (1) or add (0).  Rd = Rs +/- operand, flags set.
        const int rd = op & 7;
        const u32 a = r_[(op >> 3) & 7];
        const u32 b = (op & 0x400) ? ((op >> 6) & 7) : r_[(op >> 6) & 7];
        r_[rd] = (op & 0x200) ? addWithFlags(a, ~b, 1, true) : addWithFlags(a, b, 0, true);
        return;
    }
    if ((op & 0xE000) == 0x2000) {  // format 3: immediate operations
        // bits 12-11: 0 MOV, 1 CMP, 2 ADD, 3 SUB; Rd in bits 10-8, imm8.
        const int rd = (op >> 8) & 7;
        const u32 imm = op & 0xFF;
        switch ((op >> 11) & 3) {
            case 0: r_[rd] = imm; setNZ(imm); break;
            case 1: addWithFlags(r_[rd], ~imm, 1, true); break;
            case 2: r_[rd] = addWithFlags(r_[rd], imm, 0, true); break;
            default: r_[rd] = addWithFlags(r_[rd], ~imm, 1, true); break;
        }
        return;
    }
    if ((op & 0xFC00) == 0x4000) {  // format 4: ALU operations
        // Rd (bits 2-0) = Rd <op> Rs (bits 5-3), op in bits 9-6.  All set
        // flags.  Register shifts use the low byte of Rs and cost 1 I cycle.
        const int rd = op & 7;
        const u32 rs = r_[(op >> 3) & 7];
        u32& d = r_[rd];
        bool carry;
        // Opcodes: 0 AND, 1 EOR, 2 LSL, 3 LSR, 4 ASR, 5 ADC, 6 SBC, 7 ROR,
        // 8 TST, 9 NEG, A CMP, B CMN, C ORR, D MUL, E BIC, F MVN.
        switch ((op >> 6) & 0xF) {
            case 0x0: d &= rs; setNZ(d); break;  // AND
            case 0x1: d ^= rs; setNZ(d); break;  // EOR
            case 0x2: d = shift(d, 0, rs & 0xFF, false, carry); setNZ(d); setC(carry); ++internalCycles_; break;
            case 0x3: d = shift(d, 1, rs & 0xFF, false, carry); setNZ(d); setC(carry); ++internalCycles_; break;
            case 0x4: d = shift(d, 2, rs & 0xFF, false, carry); setNZ(d); setC(carry); ++internalCycles_; break;
            case 0x5: d = addWithFlags(d, rs, flagC() ? 1 : 0, true); break;   // ADC
            case 0x6: d = addWithFlags(d, ~rs, flagC() ? 1 : 0, true); break;  // SBC
            case 0x7: d = shift(d, 3, rs & 0xFF, false, carry); setNZ(d); setC(carry); ++internalCycles_; break;
            case 0x8: setNZ(d & rs); break;  // TST
            case 0x9: d = addWithFlags(0, ~rs, 1, true); break;  // NEG
            case 0xA: addWithFlags(d, ~rs, 1, true); break;      // CMP
            case 0xB: addWithFlags(d, rs, 0, true); break;       // CMN
            case 0xC: d |= rs; setNZ(d); break;  // ORR
            // MUL: Rd = Rd * Rs; timing taken from the old Rd value.
            case 0xD: internalCycles_ += multiplyCycles(d, true); d *= rs; setNZ(d); break;
            case 0xE: d &= ~rs; setNZ(d); break;  // BIC
            default: d = ~rs; setNZ(d); break;    // MVN
        }
        return;
    }
    if ((op & 0xFC00) == 0x4400) {  // format 5: high registers / BX
        // Can use r8-r15: bit 7 (H1) extends Rd, bit 6 (H2) extends Rs.
        // bits 9-8: 0 ADD (no flags), 1 CMP (flags), 2 MOV (no flags), 3 BX.
        // ADD/MOV to r15 are jumps; BX switches state from bit 0.
        const int rd = (op & 7) | ((op >> 4) & 8);
        const int rs = (op >> 3) & 0xF;
        const u32 value = r_[rs];
        switch ((op >> 8) & 3) {
            case 0: writeReg(rd, r_[rd] + value); break;
            case 1: addWithFlags(r_[rd], ~value, 1, true); break;
            case 2: writeReg(rd, value); break;
            default:
                if (value & 1) {
                    cpsr_ |= FT;
                } else {
                    cpsr_ &= ~FT;
                }
                branchTo(value);
                break;
        }
        return;
    }
    if ((op & 0xF800) == 0x4800) {  // format 6: PC-relative load
        // LDR Rd, [PC, #imm8 * 4], PC word-aligned (literal pools).
        r_[(op >> 8) & 7] = bus_.read32((r_[15] & ~2u) + (op & 0xFFu) * 4);
        ++internalCycles_;
        return;
    }
    if ((op & 0xF000) == 0x5000) {  // formats 7 / 8: register offset
        // Address = Rb (bits 5-3) + Ro (bits 8-6); Rd in bits 2-0.
        const int rd = op & 7;
        const u32 addr = r_[(op >> 3) & 7] + r_[(op >> 6) & 7];
        if (!(op & 0x200)) {
            // Format 7, bits 11-10 = L,B: 0 STR, 1 STRB, 2 LDR, 3 LDRB.
            switch ((op >> 10) & 3) {
                case 0: bus_.write32(addr, r_[rd]); break;
                case 1: bus_.write8(addr, static_cast<u8>(r_[rd])); break;
                case 2: r_[rd] = loadWord(addr); ++internalCycles_; break;
                default: r_[rd] = bus_.read8(addr); ++internalCycles_; break;
            }
        } else {
            // Format 8, bits 11-10 = H,S: 0 STRH, 1 LDSB, 2 LDRH, 3 LDSH
            // (same misaligned-access quirks as the ARM versions).
            switch ((op >> 10) & 3) {
                case 0: bus_.write16(addr, static_cast<u16>(r_[rd])); break;
                case 1: r_[rd] = static_cast<u32>(static_cast<s32>(static_cast<s8>(bus_.read8(addr)))); ++internalCycles_; break;
                case 2: r_[rd] = ror(bus_.read16(addr), (addr & 1) * 8); ++internalCycles_; break;
                default:
                    r_[rd] = (addr & 1) ? static_cast<u32>(static_cast<s32>(static_cast<s8>(bus_.read8(addr))))
                                        : static_cast<u32>(static_cast<s32>(static_cast<s16>(bus_.read16(addr))));
                    ++internalCycles_;
                    break;
            }
        }
        return;
    }
    if ((op & 0xE000) == 0x6000) {  // format 9: immediate offset
        // bit 12 B (byte), bit 11 L (load); offset imm5, scaled by 4 for
        // words, unscaled for bytes.
        const int rd = op & 7;
        const u32 base = r_[(op >> 3) & 7];
        const u32 imm = (op >> 6) & 0x1F;
        const bool byte = (op & 0x1000) != 0;
        const u32 addr = base + (byte ? imm : imm * 4);
        if (op & 0x800) {
            r_[rd] = byte ? bus_.read8(addr) : loadWord(addr);
            ++internalCycles_;
        } else if (byte) {
            bus_.write8(addr, static_cast<u8>(r_[rd]));
        } else {
            bus_.write32(addr, r_[rd]);
        }
        return;
    }
    if ((op & 0xF000) == 0x8000) {  // format 10: halfword immediate offset
        // LDRH/STRH Rd, [Rb, #imm5 * 2]; bit 11 L.
        const int rd = op & 7;
        const u32 addr = r_[(op >> 3) & 7] + ((op >> 6) & 0x1F) * 2;
        if (op & 0x800) {
            r_[rd] = ror(bus_.read16(addr), (addr & 1) * 8);
            ++internalCycles_;
        } else {
            bus_.write16(addr, static_cast<u16>(r_[rd]));
        }
        return;
    }
    if ((op & 0xF000) == 0x9000) {  // format 11: SP-relative
        // LDR/STR Rd, [SP, #imm8 * 4]; bit 11 L.
        const int rd = (op >> 8) & 7;
        const u32 addr = r_[13] + (op & 0xFFu) * 4;
        if (op & 0x800) {
            r_[rd] = loadWord(addr);
            ++internalCycles_;
        } else {
            bus_.write32(addr, r_[rd]);
        }
        return;
    }
    if ((op & 0xF000) == 0xA000) {  // format 12: load address
        // ADD Rd, SP/PC, #imm8 * 4; bit 11: 1 = SP, 0 = PC (word-aligned).
        const int rd = (op >> 8) & 7;
        r_[rd] = ((op & 0x800) ? r_[13] : (r_[15] & ~2u)) + (op & 0xFFu) * 4;
        return;
    }
    if ((op & 0xFF00) == 0xB000) {  // format 13: adjust SP
        // ADD SP, #+/-imm7 * 4; bit 7 = subtract.
        const u32 imm = (op & 0x7Fu) * 4;
        r_[13] = (op & 0x80) ? r_[13] - imm : r_[13] + imm;
        return;
    }
    if ((op & 0xF600) == 0xB400) {  // format 14: PUSH / POP
        // bit 11 L: 1 = POP {list[, PC]} (LDMIA SP!), 0 = PUSH {list[, LR]}
        // (STMDB SP!).  bit 8 R adds PC (pop) or LR (push).
        const u32 list = op & 0xFF;
        const bool extra = (op & 0x100) != 0;  // LR for push, PC for pop
        if (op & 0x800) {
            u32 addr = r_[13];
            for (int i = 0; i < 8; ++i) {
                if (list & (1u << i)) {
                    r_[i] = bus_.read32(addr);
                    addr += 4;
                }
            }
            if (extra) {
                branchTo(bus_.read32(addr));  // ARMv4: no switch to ARM state
                addr += 4;
            }
            r_[13] = addr;
            ++internalCycles_;
        } else {
            // Push: lowest register goes to the lowest address, so compute
            // the final SP first and store upwards.
            u32 count = static_cast<u32>(popcount16(list) + (extra ? 1 : 0));
            u32 addr = r_[13] - count * 4;
            r_[13] = addr;
            for (int i = 0; i < 8; ++i) {
                if (list & (1u << i)) {
                    bus_.write32(addr, r_[i]);
                    addr += 4;
                }
            }
            if (extra) bus_.write32(addr, r_[14]);
        }
        return;
    }
    if ((op & 0xF000) == 0xC000) {  // format 15: LDMIA / STMIA
        // LDMIA/STMIA Rb!, {list}; bit 11 L; Rb in bits 10-8.  Always writes
        // back, except LDMIA with Rb in the list (the loaded value wins).
        const int rb = (op >> 8) & 7;
        u32 list = op & 0xFF;
        u32 addr = r_[rb];
        if (list == 0) {  // empty list quirk
            // Like ARM: transfers r15 (a load jumps; a store writes
            // instruction + 6) and the base moves by 0x40.
            if (op & 0x800) {
                branchTo(bus_.read32(addr));
            } else {
                bus_.write32(addr, r_[15] + 2);
            }
            r_[rb] = addr + 0x40;
            return;
        }
        const u32 newBase = addr + static_cast<u32>(popcount16(list)) * 4;
        if (op & 0x800) {
            for (int i = 0; i < 8; ++i) {
                if (list & (1u << i)) {
                    r_[i] = bus_.read32(addr);
                    addr += 4;
                }
            }
            if (!(list & (1u << rb))) r_[rb] = newBase;
            ++internalCycles_;
        } else {
            // STMIA with Rb in the list: stores the old base if Rb is the
            // first (lowest) register, otherwise the already-updated base.
            bool first = true;
            for (int i = 0; i < 8; ++i) {
                if (list & (1u << i)) {
                    bus_.write32(addr, r_[i]);
                    if (first) r_[rb] = newBase;
                    first = false;
                    addr += 4;
                }
            }
        }
        return;
    }
    // Must be tested before format 16: SWI uses the "condition 1111" slot.
    if ((op & 0xFF00) == 0xDF00) {  // format 17: SWI
        // The BIOS function number is the low byte (bits 7-0).
        if (swiHandler_) swiHandler_->swi(op & 0xFF);
        return;
    }
    if ((op & 0xF000) == 0xD000) {  // format 16: conditional branch
        // B<cond> to PC + signed imm8 * 2, cond in bits 11-8.  (Condition
        // 1110 is undefined on hardware; here it behaves as "always".)
        if (condition((op >> 8) & 0xF)) {
            s32 offset = static_cast<s32>(static_cast<s8>(op & 0xFF)) * 2;
            branchTo(r_[15] + static_cast<u32>(offset));
        }
        return;
    }
    if ((op & 0xF800) == 0xE000) {  // format 18: unconditional branch
        // B to PC + signed imm11 * 2 (shift left 21, arithmetic right 20).
        s32 offset = static_cast<s32>(static_cast<u32>(op & 0x7FF) << 21) >> 20;
        branchTo(r_[15] + static_cast<u32>(offset));
        return;
    }
    if ((op & 0xF000) == 0xF000) {  // format 19: long branch with link
        // BL is two 16-bit instructions:
        //   H = 0: LR = PC + (signed imm11 << 12)       (upper half of offset)
        //   H = 1: PC = LR + (imm11 << 1); LR = next instruction | 1
        // Bit 0 of the new LR marks a Thumb return address for BX LR.
        if (!(op & 0x800)) {
            s32 offset = static_cast<s32>(static_cast<u32>(op & 0x7FF) << 21) >> 9;
            r_[14] = r_[15] + static_cast<u32>(offset);
        } else {
            u32 next = pc_;
            branchTo(r_[14] + ((op & 0x7FFu) << 1));
            r_[14] = next | 1;
        }
        return;
    }
    // Undefined Thumb instruction: ignore.
}

}  // namespace gba
