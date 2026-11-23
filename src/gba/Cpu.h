// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Cpu.h
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Declares gba::Cpu, an interpreter for the ARM7TDMI processor of the Game Boy
//  Advance (architecture ARMv4T), and gba::SwiHandler, the small interface the
//  CPU calls when a program executes a software interrupt (SWI) instruction.
//  The CPU runs both instruction sets of the chip: 32-bit ARM and 16-bit
//  Thumb.  It keeps the banked registers of every processor mode, takes IRQ
//  exceptions, and reports how many cycles each instruction took.
//  See Cpu.cpp for the implementation and the instruction decoders.
//
//  How it fits in
//  --------------
//  gba::Gba owns one Cpu, one Bus and one Bios.  Gba::runFrame() calls
//  Cpu::step() in a loop; each call executes ONE instruction and returns the
//  number of CPU cycles (16.78 MHz clocks) it took, which Gba then feeds to
//  the timers and the PPU.  Every memory access goes through gba::Bus, which
//  adds the access cost (wait states) to a cycle counter that step() collects.
//  Interrupt state (IE/IF/IME) lives in gba::Interrupts (Common.h); the CPU
//  only reads it to decide whether to take an IRQ or to leave HALT.
//  SWI instructions do not enter the real BIOS: they are forwarded to the
//  SwiHandler (gba::Bios, see Bios.h), which emulates the BIOS call in C++.
//
//  Hardware notes
//  --------------
//  (GBATEK: "ARM CPU Overview", "ARM CPU Register Set", "ARM CPU Flags &
//  Condition Field", "ARM CPU Exceptions", "ARM Instruction Summary",
//  "THUMB Instruction Summary".)
//  - 16 visible registers r0-r15; r13 = SP, r14 = LR (link register), r15 = PC.
//  - CPSR (current program status register):
//      bit 31 N (negative), 30 Z (zero), 29 C (carry), 28 V (overflow),
//      bit 7 I (1 = IRQs disabled), bit 6 F (1 = FIQs disabled),
//      bit 5 T (1 = Thumb state, 0 = ARM state), bits 0-4 M (mode, see Mode).
//  - Banked registers: some registers are swapped out when the mode changes.
//    FIQ has its own r8-r14; IRQ, Supervisor, Abort and Undefined each have
//    their own r13 and r14 and an SPSR (saved program status register, a copy
//    of the CPSR at exception entry).  User and System share one bank.
//  - Pipeline: the ARM7TDMI has a 3-stage pipeline (fetch, decode, execute),
//    so an instruction that reads r15 sees the address of the instruction
//    being executed + 8 in ARM state, + 4 in Thumb state.  This class models
//    that by storing the "pipeline-visible" value in r_[15] before executing,
//    and the real next-instruction address separately in pc_.
//  - IRQ exception: save CPSR into SPSR_irq, switch to IRQ mode, force ARM
//    state, set I, set LR_irq = (address of the next instruction) + 4, and
//    jump to the vector at 0x00000018.  The handler returns with
//    "SUBS PC, LR, #4", which also restores CPSR from SPSR.
//
//  Contributing
//  ------------
//  - All registers r0-r14 are always stored in r_[] for the CURRENT mode;
//    the banks hold the values of the modes that are not active.  Never write
//    the banks directly except through switchMode()/setUserReg().
//  - Any write to r15 must go through writeReg()/branchTo() so that pc_ is
//    updated, the address is aligned, and the next fetch is non-sequential.
//  - Test changes with the jsmolka gba-tests (arm, thumb, memory, bios):
//      make && ./tboyemu --headless --frames 120 --screenshot out.png arm.gba
//    and check that the picture says "All tests passed".
// ============================================================================
#ifndef TBOYEMU_GBA_CPU_H
#define TBOYEMU_GBA_CPU_H

#include "Bus.h"
#include "Common.h"

namespace gba {

// Receives SWI (software interrupt) calls: the BIOS is emulated in C++.
//
// On hardware, "SWI n" (ARM: SWI with the number in bits 16-23 of the opcode;
// Thumb: SWI with the number in bits 0-7) enters Supervisor mode and jumps to
// the BIOS vector at 0x08, which dispatches to a BIOS routine.  TBoyEmu has no
// BIOS image, so the CPU instead calls swi(number) synchronously, in the
// middle of executing the SWI instruction, and the handler reads/writes the
// CPU registers directly (via Cpu::reg()/setReg()).  The CPU mode is NOT
// changed; to the program it looks like the call returned immediately.
// gba::Bios is the only implementation.  Not owned by the Cpu.
class SwiHandler {
public:
    virtual ~SwiHandler() {}
    // number: the 8-bit BIOS function number (0x00 - 0xFF).
    virtual void swi(u32 number) = 0;
};

// ARM7TDMI (ARMv4T) interpreter: ARM and Thumb instruction sets, banked
// registers for every processor mode, IRQ exceptions.  step() executes one
// instruction and returns the cycles it took (from the bus wait states plus
// internal cycles).
//
// Ownership/lifetime: owned by gba::Gba; holds references to the Bus and the
// Interrupts, which Gba also owns and which outlive it.  Single-threaded, like
// the rest of the emulator.  Not copyable.
// Not emulated: FIQ (nothing on the GBA raises it), data/prefetch aborts,
// the undefined-instruction exception (undefined opcodes are ignored), and
// the coprocessor instructions (the GBA has no coprocessor).
class Cpu : private NonCopyable {
public:
    // Values of the CPSR mode field (bits 0-4).  System is a privileged mode
    // that uses the User registers.
    enum Mode : u32 { User = 0x10, Fiq = 0x11, Irq = 0x12, Supervisor = 0x13, Abort = 0x17, Undefined = 0x1B, System = 0x1F };

    // bus: memory map used for every fetch / load / store.
    // interrupts: IE/IF/IME, polled before every instruction.
    Cpu(Bus& bus, Interrupts& interrupts);

    // Puts the CPU in the state the real BIOS leaves it in right before it
    // jumps to the cartridge (we skip the boot sequence): System mode, ARM
    // state, IRQs enabled in CPSR, the stacks of System/IRQ/Supervisor set to
    // their standard addresses in IWRAM, all other registers 0.
    void reset();  // state left by the BIOS boot sequence, PC = 0x08000000
    // Executes one instruction (or takes a pending IRQ first, then executes
    // the first handler instruction).  Returns the number of CPU cycles used,
    // always >= 1 -- except while halted with no interrupt pending, where it
    // returns 0 and the caller (Gba::runFrame) skips time forward to the next
    // event that could raise an interrupt.
    int step();

    // HALT state (entered via the Halt/IntrWait BIOS calls or by writing
    // HALTCNT at 0x04000301).  The CPU leaves it as soon as (IE & IF) != 0,
    // even when IME is 0 (GBATEK "BIOS Halt Functions").
    bool halted() const { return halted_; }
    void halt() { halted_ = true; }

    // Register access for the BIOS emulation.
    // reg(15) returns the address of the NEXT instruction (pc_), not the
    // pipeline-visible r15, because a BIOS call is "returning" to it.
    u32 reg(int i) const { return i == 15 ? pc_ : r_[i]; }
    // Writing r15 is a jump (see branchTo()).
    void setReg(int i, u32 v) {
        if (i == 15) {
            branchTo(v);
        } else {
            r_[i] = v;
        }
    }
    u32 cpsr() const { return cpsr_; }
    // True when the CPSR T bit (bit 5) is set, i.e. executing Thumb code.
    bool thumb() const { return (cpsr_ & 0x20) != 0; }
    // Address of the next instruction to fetch.
    u32 currentPc() const { return pc_; }
    // Jumps to addr: aligns it to 2 (Thumb) or 4 (ARM) bytes, makes it the
    // next instruction and marks the next fetch as non-sequential.  To switch
    // the instruction set, set/clear the T bit first (as BX does).
    void branchTo(u32 addr);  // keeps the current instruction set
    // Installs the BIOS emulation; may be null (SWIs are then ignored).
    void setSwiHandler(SwiHandler* handler) { swiHandler_ = handler; }
    // Make the current instruction run again (used for IntrWait).
    // Called from inside swi(): it sets pc_ back to the SWI instruction itself
    // so that, after the CPU is woken by an interrupt and the IRQ handler
    // returns, the SWI executes again and re-checks its condition.
    void repeatInstruction() { branchTo(instructionAddr_); }

private:
    // ---- execution ----
    // Decode and run one 32-bit ARM opcode (checks the condition field first).
    void executeArm(u32 op);
    // Decode and run one 16-bit Thumb opcode (formats 1-19 in GBATEK order).
    void executeThumb(u16 op);
    // Take an IRQ exception: bank registers, SPSR_irq = CPSR, LR, PC = 0x18.
    void enterIrq();

    // ARM instruction groups (one method per GBATEK "ARM Opcodes" chapter).
    void armBranchExchange(u32 op);    // BX Rn
    void armBranch(u32 op);            // B / BL
    void armDataProcessing(u32 op);    // AND, EOR, SUB, ..., MOV, BIC, MVN
    void armPsrTransfer(u32 op);       // MRS / MSR
    void armMultiply(u32 op);          // MUL / MLA
    void armMultiplyLong(u32 op);      // UMULL / UMLAL / SMULL / SMLAL
    void armSwap(u32 op);              // SWP / SWPB
    void armHalfwordTransfer(u32 op);  // LDRH / STRH / LDRSB / LDRSH
    void armSingleTransfer(u32 op);    // LDR / STR / LDRB / STRB
    void armBlockTransfer(u32 op);     // LDM / STM

    // ---- helpers ----
    // Evaluates a 4-bit condition code (EQ, NE, CS, ..., AL) against N/Z/C/V.
    bool condition(u32 cond) const;
    // Barrel shifter.  type: 0 LSL, 1 LSR, 2 ASR, 3 ROR.  immediate: true when
    // the amount comes from the opcode (then amount 0 means LSR/ASR #32 or
    // RRX), false when it comes from a register (then 0 means "no shift").
    // carry: receives the shifter carry-out (C flag unchanged when no shift).
    u32 shift(u32 value, u32 type, u32 amount, bool immediate, bool& carry) const;
    // Returns a + b + carryIn; when setFlags, sets N, Z, C (unsigned carry out
    // of bit 31) and V (signed overflow).  Subtraction a - b is done as
    // a + ~b + 1, so C ends up as "no borrow", matching ARM semantics.
    u32 addWithFlags(u32 a, u32 b, u32 carryIn, bool setFlags);
    // Sets N (bit 31 of v) and Z (v == 0); leaves C and V alone.
    void setNZ(u32 v) {
        cpsr_ = (cpsr_ & 0x3FFFFFFF) | (v & 0x80000000u) | (v == 0 ? 0x40000000u : 0);
    }
    void setC(bool c) { cpsr_ = c ? (cpsr_ | 0x20000000u) : (cpsr_ & ~0x20000000u); }
    bool flagC() const { return (cpsr_ & 0x20000000u) != 0; }
    // 32-bit load.  The bus reads the aligned word; for addr & 3 != 0 the
    // ARM7TDMI rotates it right by 8 * (addr & 3) bits (GBATEK "ARM Opcodes:
    // Memory: Single Data Transfer (LDR, STR, PLD)").
    u32 loadWord(u32 addr);  // with the ARM rotation for misaligned addresses
    u32 operandReg(int i) const { return r_[i]; }
    // Writes a register; r15 is a jump through branchTo().
    void writeReg(int i, u32 v) {
        if (i == 15) {
            branchTo(v);
        } else {
            r_[i] = v;
        }
    }
    // Internal cycles (the "m" in GBATEK's timing tables) of a multiply,
    // 1-4 depending on how many top bytes of the multiplier rs are all 0
    // (or all 1 for signed multiplies): the hardware stops early.
    int multiplyCycles(u32 rs, bool isSigned) const;

    // ---- modes ----
    // Maps a mode to its bank slot: 0 User/System, 1 FIQ, 2 IRQ,
    // 3 Supervisor, 4 Abort, 5 Undefined.
    static int bankIndex(u32 mode);
    // Changes the CPSR mode bits, saving the current r13/r14 (and r8-r12 for
    // FIQ) into the old bank and loading the ones of the new mode.
    void switchMode(u32 mode);
    // Writes the whole CPSR, swapping register banks if the mode changes.
    void setCpsr(u32 value);
    // SPSR of the current mode.  User/System have no SPSR: returns CPSR.
    u32 spsr() const;
    // Writes the SPSR of the current mode (ignored in User/System).
    void setSpsr(u32 value);
    // Access to the User-mode register i from a privileged mode, as done by
    // "LDM/STM ...^" (S bit without PC in the list).
    u32 userReg(int i) const;
    void setUserReg(int i, u32 v);

    Bus& bus_;
    Interrupts& interrupts_;
    SwiHandler* swiHandler_;  // not owned; may be null

    u32 r_[16];  // r_[15] = address of the current instruction + 8 (ARM) / + 4 (Thumb)
    u32 cpsr_;   // N Z C V ... I F T M[4:0]; see the file header
    // Banked state, indexed by bankIndex(): slot 0 = User/System,
    // 1 = FIQ, 2 = IRQ, 3 = Supervisor, 4 = Abort, 5 = Undefined.
    // spsrBank_[0] is unused (User/System have no SPSR).  The entry of the
    // CURRENT mode is stale: the live values are in r_[13] / r_[14].
    u32 spsrBank_[6];
    u32 bank13_[6], bank14_[6];
    // r8-r12: fiqBank_ holds FIQ's copies while not in FIQ mode, userBank_
    // holds everyone else's copies while in FIQ mode.
    u32 fiqBank_[5], userBank_[5];  // r8-r12

    u32 pc_;               // next instruction to execute
    u32 instructionAddr_;  // instruction being executed
    // Set by branchTo() when the executing instruction changed the flow; the
    // next fetch is then non-sequential (N cycle) instead of sequential (S).
    bool branched_;
    // Whether the next instruction fetch is sequential (affects ROM timing).
    bool sequential_;
    // In HALT: step() does nothing until an enabled interrupt is pending.
    bool halted_;
    // Internal ("I") cycles of the current instruction, added to the bus
    // cycles by step(): multiplies, register-specified shifts, loads, etc.
    int internalCycles_;
};

}  // namespace gba

#endif
