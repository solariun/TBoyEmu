#include "Cpu.h"

namespace gba {

namespace {
const u32 FN = 0x80000000u;
const u32 FZ = 0x40000000u;
const u32 FC = 0x20000000u;
const u32 FV = 0x10000000u;
const u32 FI = 0x00000080u;
const u32 FT = 0x00000020u;

inline u32 ror(u32 v, u32 n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

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
    cpsr_ = System;
    bank13_[bankIndex(Supervisor)] = 0x03007FE0;
    bank13_[bankIndex(Irq)] = 0x03007FA0;
    bank13_[bankIndex(System)] = 0x03007F00;
    r_[13] = 0x03007F00;
    pc_ = 0x08000000;
    instructionAddr_ = pc_;
    branched_ = false;
    sequential_ = false;
    halted_ = false;
    internalCycles_ = 0;
}

// --------------------------------------------------------------- modes ----

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

void Cpu::setCpsr(u32 value) {
    switchMode(value & 0x1F);
    cpsr_ = value;
}

u32 Cpu::spsr() const {
    int b = bankIndex(cpsr_);
    return b == 0 ? cpsr_ : spsrBank_[b];
}

void Cpu::setSpsr(u32 value) {
    int b = bankIndex(cpsr_);
    if (b != 0) spsrBank_[b] = value;
}

u32 Cpu::userReg(int i) const {
    int b = bankIndex(cpsr_);
    if (i >= 8 && i <= 12 && b == 1) return userBank_[i - 8];
    if ((i == 13 || i == 14) && b != 0) return i == 13 ? bank13_[0] : bank14_[0];
    return r_[i];
}

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

void Cpu::branchTo(u32 addr) {
    pc_ = thumb() ? (addr & ~1u) : (addr & ~3u);
    branched_ = true;
}

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

int Cpu::step() {
    if (halted_) {
        if (!interrupts_.pending()) return 0;  // caller skips time ahead
        halted_ = false;
    }
    if (interrupts_.shouldInterrupt() && !(cpsr_ & FI)) enterIrq();

    instructionAddr_ = pc_;
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
        if (pc_ < 0x4000) bus_.latchBiosAt(pc_ + 8);
        r_[15] = pc_ + 8;
        pc_ += 4;
        executeArm(op);
    }
    sequential_ = !branched_;
    if (bus_.consumeHaltRequest()) halted_ = true;
    int cycles = bus_.takeCycles() + internalCycles_;
    return cycles > 0 ? cycles : 1;
}

bool Cpu::condition(u32 cond) const {
    const bool n = (cpsr_ & FN) != 0, z = (cpsr_ & FZ) != 0, c = (cpsr_ & FC) != 0, v = (cpsr_ & FV) != 0;
    switch (cond) {
        case 0x0: return z;
        case 0x1: return !z;
        case 0x2: return c;
        case 0x3: return !c;
        case 0x4: return n;
        case 0x5: return !n;
        case 0x6: return v;
        case 0x7: return !v;
        case 0x8: return c && !z;
        case 0x9: return !c || z;
        case 0xA: return n == v;
        case 0xB: return n != v;
        case 0xC: return !z && n == v;
        case 0xD: return z || n != v;
        case 0xE: return true;
        default: return false;
    }
}

u32 Cpu::shift(u32 value, u32 type, u32 amount, bool immediate, bool& carry) const {
    carry = flagC();
    switch (type) {
        case 0:  // LSL
            if (amount == 0) return value;
            if (amount < 32) {
                carry = (value >> (32 - amount)) & 1;
                return value << amount;
            }
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
            carry = amount == 32 ? (value >> 31) != 0 : false;
            return 0;
        case 2:  // ASR
            if (amount == 0) {
                if (!immediate) return value;
                amount = 32;
            }
            if (amount < 32) {
                carry = (value >> (amount - 1)) & 1;
                return static_cast<u32>(static_cast<s32>(value) >> amount);
            }
            carry = (value >> 31) != 0;
            return carry ? 0xFFFFFFFFu : 0;
        default:  // ROR / RRX
            if (amount == 0) {
                if (!immediate) return value;
                bool c = flagC();  // RRX
                carry = value & 1;
                return (value >> 1) | (c ? 0x80000000u : 0);
            }
            amount &= 31;
            if (amount == 0) {
                carry = (value >> 31) != 0;
                return value;
            }
            carry = (value >> (amount - 1)) & 1;
            return ror(value, amount);
    }
}

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

u32 Cpu::loadWord(u32 addr) { return ror(bus_.read32(addr), (addr & 3) * 8); }

int Cpu::multiplyCycles(u32 rs, bool isSigned) const {
    if (isSigned && (rs & 0x80000000u)) rs = ~rs;
    if ((rs & 0xFFFFFF00u) == 0) return 1;
    if ((rs & 0xFFFF0000u) == 0) return 2;
    if ((rs & 0xFF000000u) == 0) return 3;
    return 4;
}

// ------------------------------------------------------------------ ARM ----

void Cpu::executeArm(u32 op) {
    if (!condition(op >> 28)) return;

    if ((op & 0x0FFFFFF0u) == 0x012FFF10u) return armBranchExchange(op);
    if ((op & 0x0E000000u) == 0x0A000000u) return armBranch(op);
    if ((op & 0x0F000000u) == 0x0F000000u) {
        if (swiHandler_) swiHandler_->swi((op >> 16) & 0xFF);
        return;
    }
    if ((op & 0x0FC000F0u) == 0x00000090u) return armMultiply(op);
    if ((op & 0x0F8000F0u) == 0x00800090u) return armMultiplyLong(op);
    if ((op & 0x0FB00FF0u) == 0x01000090u) return armSwap(op);
    if ((op & 0x0E000090u) == 0x00000090u && (op & 0x60u)) return armHalfwordTransfer(op);
    if ((op & 0x0FBF0FFFu) == 0x010F0000u || (op & 0x0DB0F000u) == 0x0120F000u) return armPsrTransfer(op);
    if ((op & 0x0C000000u) == 0x00000000u) return armDataProcessing(op);
    if ((op & 0x0C000000u) == 0x04000000u) {
        if ((op & 0x02000010u) == 0x02000010u) return;  // undefined instruction
        return armSingleTransfer(op);
    }
    if ((op & 0x0E000000u) == 0x08000000u) return armBlockTransfer(op);
    // Coprocessor instructions: no coprocessor on the GBA.
}

void Cpu::armBranchExchange(u32 op) {
    u32 target = r_[op & 0xF];
    if (target & 1) {
        cpsr_ |= FT;
    } else {
        cpsr_ &= ~FT;
    }
    branchTo(target);
}

void Cpu::armBranch(u32 op) {
    s32 offset = static_cast<s32>(op << 8) >> 6;  // sign-extended 24 bits * 4
    if (op & 0x01000000u) r_[14] = pc_;           // BL: address of the next instruction
    branchTo(r_[15] + static_cast<u32>(offset));
}

void Cpu::armDataProcessing(u32 op) {
    const u32 opcode = (op >> 21) & 0xF;
    const bool setFlags = (op & 0x00100000u) != 0;
    const int rn = static_cast<int>((op >> 16) & 0xF);
    const int rd = static_cast<int>((op >> 12) & 0xF);

    u32 operand2;
    bool carry = flagC();
    u32 rnValue = r_[rn];
    if (op & 0x02000000u) {
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
    bool logical = false;
    bool writeResult = true;
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
            if (setFlags) setCpsr(spsr());  // return from exception
            branchTo(result);
        } else {
            r_[rd] = result;
        }
    } else if (rd == 15 && setFlags) {
        setCpsr(spsr());  // e.g. TEQP-style: restore CPSR only
    }
}

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
        setCpsr(next | 0x10);
    } else {
        cpsr_ = next;
    }
}

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
    if (load) writeReg(rd, value);
}

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

    if (writeback && (!load || !(list & (1u << rn)))) r_[rn] = newBase;
    if (loadsPc && psrOrUser) {
        setCpsr(spsr());  // LDM ..., {.., pc}^ : return from exception
        branchTo(loadedPc);  // aligned for the restored instruction set
    }
}

// ---------------------------------------------------------------- Thumb ----

void Cpu::executeThumb(u16 op) {
    const u32 top5 = op >> 11;

    if (top5 < 3) {  // format 1: shift by immediate
        const int rd = op & 7;
        bool carry;
        u32 result = shift(r_[(op >> 3) & 7], top5, (op >> 6) & 0x1F, true, carry);
        r_[rd] = result;
        setNZ(result);
        setC(carry);
        return;
    }
    if (top5 == 3) {  // format 2: add / subtract
        const int rd = op & 7;
        const u32 a = r_[(op >> 3) & 7];
        const u32 b = (op & 0x400) ? ((op >> 6) & 7) : r_[(op >> 6) & 7];
        r_[rd] = (op & 0x200) ? addWithFlags(a, ~b, 1, true) : addWithFlags(a, b, 0, true);
        return;
    }
    if ((op & 0xE000) == 0x2000) {  // format 3: immediate operations
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
        const int rd = op & 7;
        const u32 rs = r_[(op >> 3) & 7];
        u32& d = r_[rd];
        bool carry;
        switch ((op >> 6) & 0xF) {
            case 0x0: d &= rs; setNZ(d); break;
            case 0x1: d ^= rs; setNZ(d); break;
            case 0x2: d = shift(d, 0, rs & 0xFF, false, carry); setNZ(d); setC(carry); ++internalCycles_; break;
            case 0x3: d = shift(d, 1, rs & 0xFF, false, carry); setNZ(d); setC(carry); ++internalCycles_; break;
            case 0x4: d = shift(d, 2, rs & 0xFF, false, carry); setNZ(d); setC(carry); ++internalCycles_; break;
            case 0x5: d = addWithFlags(d, rs, flagC() ? 1 : 0, true); break;
            case 0x6: d = addWithFlags(d, ~rs, flagC() ? 1 : 0, true); break;
            case 0x7: d = shift(d, 3, rs & 0xFF, false, carry); setNZ(d); setC(carry); ++internalCycles_; break;
            case 0x8: setNZ(d & rs); break;
            case 0x9: d = addWithFlags(0, ~rs, 1, true); break;  // NEG
            case 0xA: addWithFlags(d, ~rs, 1, true); break;      // CMP
            case 0xB: addWithFlags(d, rs, 0, true); break;       // CMN
            case 0xC: d |= rs; setNZ(d); break;
            case 0xD: internalCycles_ += multiplyCycles(d, true); d *= rs; setNZ(d); break;
            case 0xE: d &= ~rs; setNZ(d); break;
            default: d = ~rs; setNZ(d); break;
        }
        return;
    }
    if ((op & 0xFC00) == 0x4400) {  // format 5: high registers / BX
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
        r_[(op >> 8) & 7] = bus_.read32((r_[15] & ~2u) + (op & 0xFFu) * 4);
        ++internalCycles_;
        return;
    }
    if ((op & 0xF000) == 0x5000) {  // formats 7 / 8: register offset
        const int rd = op & 7;
        const u32 addr = r_[(op >> 3) & 7] + r_[(op >> 6) & 7];
        if (!(op & 0x200)) {
            switch ((op >> 10) & 3) {
                case 0: bus_.write32(addr, r_[rd]); break;
                case 1: bus_.write8(addr, static_cast<u8>(r_[rd])); break;
                case 2: r_[rd] = loadWord(addr); ++internalCycles_; break;
                default: r_[rd] = bus_.read8(addr); ++internalCycles_; break;
            }
        } else {
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
        const int rd = (op >> 8) & 7;
        r_[rd] = ((op & 0x800) ? r_[13] : (r_[15] & ~2u)) + (op & 0xFFu) * 4;
        return;
    }
    if ((op & 0xFF00) == 0xB000) {  // format 13: adjust SP
        const u32 imm = (op & 0x7Fu) * 4;
        r_[13] = (op & 0x80) ? r_[13] - imm : r_[13] + imm;
        return;
    }
    if ((op & 0xF600) == 0xB400) {  // format 14: PUSH / POP
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
        const int rb = (op >> 8) & 7;
        u32 list = op & 0xFF;
        u32 addr = r_[rb];
        if (list == 0) {  // empty list quirk
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
    if ((op & 0xFF00) == 0xDF00) {  // format 17: SWI
        if (swiHandler_) swiHandler_->swi(op & 0xFF);
        return;
    }
    if ((op & 0xF000) == 0xD000) {  // format 16: conditional branch
        if (condition((op >> 8) & 0xF)) {
            s32 offset = static_cast<s32>(static_cast<s8>(op & 0xFF)) * 2;
            branchTo(r_[15] + static_cast<u32>(offset));
        }
        return;
    }
    if ((op & 0xF800) == 0xE000) {  // format 18: unconditional branch
        s32 offset = static_cast<s32>(static_cast<u32>(op & 0x7FF) << 21) >> 20;
        branchTo(r_[15] + static_cast<u32>(offset));
        return;
    }
    if ((op & 0xF000) == 0xF000) {  // format 19: long branch with link
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
