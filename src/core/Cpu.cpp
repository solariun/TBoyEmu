#include "Cpu.h"

namespace gb {

namespace {
const u8 FZ = 0x80;  // zero
const u8 FN = 0x40;  // subtract
const u8 FH = 0x20;  // half carry
const u8 FC = 0x10;  // carry

const u16 kInterruptVectors[5] = {0x40, 0x48, 0x50, 0x58, 0x60};
}  // namespace

Cpu::Cpu(Bus& bus, InterruptController& ic) : bus_(bus), ic_(ic) { reset(true); }

void Cpu::reset(bool cgbMode) {
    // Register state left by the boot ROM (the boot ROM itself is skipped).
    if (cgbMode) {
        r_.a = 0x11; r_.f = 0x80;
        r_.b = 0x00; r_.c = 0x00;
        r_.d = 0xFF; r_.e = 0x56;
        r_.h = 0x00; r_.l = 0x0D;
    } else {
        r_.a = 0x01; r_.f = 0xB0;
        r_.b = 0x00; r_.c = 0x13;
        r_.d = 0x00; r_.e = 0xD8;
        r_.h = 0x01; r_.l = 0x4D;
    }
    r_.sp = 0xFFFE;
    r_.pc = 0x0100;
    ime_ = false;
    eiDelay_ = 0;
    halted_ = false;
    haltBug_ = false;
}

// ------------------------------------------------------------ helpers ----

u8 Cpu::fetch8() {
    u8 v = read8(r_.pc);
    if (haltBug_) {
        haltBug_ = false;  // PC fails to increment once
    } else {
        r_.pc = static_cast<u16>(r_.pc + 1);
    }
    return v;
}

u16 Cpu::fetch16() {
    u8 lo = fetch8();
    u8 hi = fetch8();
    return static_cast<u16>(lo | (hi << 8));
}

void Cpu::push16(u16 value) {
    idle();
    r_.sp = static_cast<u16>(r_.sp - 1);
    write8(r_.sp, static_cast<u8>(value >> 8));
    r_.sp = static_cast<u16>(r_.sp - 1);
    write8(r_.sp, static_cast<u8>(value));
}

u16 Cpu::pop16() {
    u8 lo = read8(r_.sp);
    r_.sp = static_cast<u16>(r_.sp + 1);
    u8 hi = read8(r_.sp);
    r_.sp = static_cast<u16>(r_.sp + 1);
    return static_cast<u16>(lo | (hi << 8));
}

u16 Cpu::pair(int index) const {
    switch (index) {
        case 0: return bc();
        case 1: return de();
        case 2: return hl();
        default: return r_.sp;
    }
}

void Cpu::setPair(int index, u16 v) {
    switch (index) {
        case 0: setBc(v); break;
        case 1: setDe(v); break;
        case 2: setHl(v); break;
        default: r_.sp = v; break;
    }
}

u8 Cpu::getReg(int index) {
    switch (index) {
        case 0: return r_.b;
        case 1: return r_.c;
        case 2: return r_.d;
        case 3: return r_.e;
        case 4: return r_.h;
        case 5: return r_.l;
        case 6: return read8(hl());
        default: return r_.a;
    }
}

void Cpu::setReg(int index, u8 value) {
    switch (index) {
        case 0: r_.b = value; break;
        case 1: r_.c = value; break;
        case 2: r_.d = value; break;
        case 3: r_.e = value; break;
        case 4: r_.h = value; break;
        case 5: r_.l = value; break;
        case 6: write8(hl(), value); break;
        default: r_.a = value; break;
    }
}

bool Cpu::condition(int cc) const {
    switch (cc) {
        case 0: return !flag(FZ);
        case 1: return flag(FZ);
        case 2: return !flag(FC);
        default: return flag(FC);
    }
}

// --------------------------------------------------------------- step ----

void Cpu::serviceInterrupt(u8 pending) {
    int bit = 0;
    while (!(pending & (1 << bit))) ++bit;
    ime_ = false;
    if (haltBug_) {
        // EI; HALT with an interrupt pending: the interrupt is taken right
        // away and returns to the HALT instruction itself.
        haltBug_ = false;
        r_.pc = static_cast<u16>(r_.pc - 1);
    }
    idle();
    push16(r_.pc);  // includes one internal cycle
    ic_.acknowledge(static_cast<u8>(1 << bit));
    r_.pc = kInterruptVectors[bit];
    idle();
}

void Cpu::step() {
    // CPU frozen by a VRAM DMA transfer.
    for (int stall = bus_.takeStallCycles(); stall > 0; --stall) idle();

    u8 pending = ic_.pending();
    if (halted_) {
        if (!pending) {
            idle();
            return;
        }
        halted_ = false;
    }

    if (ime_ && pending) {
        serviceInterrupt(pending);
        return;
    }

    execute(fetch8());

    if (eiDelay_ > 0 && --eiDelay_ == 0) ime_ = true;
}

void Cpu::halt() {
    if (!ime_ && ic_.pending()) {
        haltBug_ = true;  // HALT is skipped and the next byte is read twice
    } else {
        halted_ = true;
    }
}

void Cpu::stop() {
    fetch8();  // STOP is two bytes long
    if (bus_.speedSwitchArmed()) {
        bus_.switchSpeed();
    } else {
        halted_ = true;  // low power mode; any interrupt (joypad) wakes it
    }
}

// ---------------------------------------------------------------- ALU ----

void Cpu::alu(int operation, u8 v) {
    const u8 a = r_.a;
    const int carry = flag(FC) ? 1 : 0;
    int result;
    u8 f = 0;
    switch (operation) {
        case 0:  // ADD
            result = a + v;
            if (((a & 0xF) + (v & 0xF)) > 0xF) f |= FH;
            if (result > 0xFF) f |= FC;
            r_.a = static_cast<u8>(result);
            break;
        case 1:  // ADC
            result = a + v + carry;
            if (((a & 0xF) + (v & 0xF) + carry) > 0xF) f |= FH;
            if (result > 0xFF) f |= FC;
            r_.a = static_cast<u8>(result);
            break;
        case 2:  // SUB
        case 7:  // CP
            result = a - v;
            f |= FN;
            if ((a & 0xF) < (v & 0xF)) f |= FH;
            if (result < 0) f |= FC;
            if (operation == 2) r_.a = static_cast<u8>(result);
            break;
        case 3:  // SBC
            result = a - v - carry;
            f |= FN;
            if ((a & 0xF) - (v & 0xF) - carry < 0) f |= FH;
            if (result < 0) f |= FC;
            r_.a = static_cast<u8>(result);
            break;
        case 4:  // AND
            result = a & v;
            f |= FH;
            r_.a = static_cast<u8>(result);
            break;
        case 5:  // XOR
            result = a ^ v;
            r_.a = static_cast<u8>(result);
            break;
        default:  // OR
            result = a | v;
            r_.a = static_cast<u8>(result);
            break;
    }
    if ((result & 0xFF) == 0) f |= FZ;
    r_.f = f;
}

u8 Cpu::rotateShift(int operation, u8 v) {
    u8 result;
    bool carryOut;
    const u8 carryIn = flag(FC) ? 1 : 0;
    switch (operation) {
        case 0:  // RLC
            carryOut = (v & 0x80) != 0;
            result = static_cast<u8>((v << 1) | (v >> 7));
            break;
        case 1:  // RRC
            carryOut = (v & 0x01) != 0;
            result = static_cast<u8>((v >> 1) | (v << 7));
            break;
        case 2:  // RL
            carryOut = (v & 0x80) != 0;
            result = static_cast<u8>((v << 1) | carryIn);
            break;
        case 3:  // RR
            carryOut = (v & 0x01) != 0;
            result = static_cast<u8>((v >> 1) | (carryIn << 7));
            break;
        case 4:  // SLA
            carryOut = (v & 0x80) != 0;
            result = static_cast<u8>(v << 1);
            break;
        case 5:  // SRA
            carryOut = (v & 0x01) != 0;
            result = static_cast<u8>((v >> 1) | (v & 0x80));
            break;
        case 6:  // SWAP
            carryOut = false;
            result = static_cast<u8>((v << 4) | (v >> 4));
            break;
        default:  // SRL
            carryOut = (v & 0x01) != 0;
            result = static_cast<u8>(v >> 1);
            break;
    }
    r_.f = static_cast<u8>((result == 0 ? FZ : 0) | (carryOut ? FC : 0));
    return result;
}

u8 Cpu::inc8(u8 v) {
    u8 r = static_cast<u8>(v + 1);
    r_.f = static_cast<u8>((r_.f & FC) | (r == 0 ? FZ : 0) | ((v & 0xF) == 0xF ? FH : 0));
    return r;
}

u8 Cpu::dec8(u8 v) {
    u8 r = static_cast<u8>(v - 1);
    r_.f = static_cast<u8>((r_.f & FC) | FN | (r == 0 ? FZ : 0) | ((v & 0xF) == 0 ? FH : 0));
    return r;
}

void Cpu::addHl(u16 v) {
    u16 h = hl();
    int result = h + v;
    u8 f = r_.f & FZ;
    if (((h & 0x0FFF) + (v & 0x0FFF)) > 0x0FFF) f |= FH;
    if (result > 0xFFFF) f |= FC;
    r_.f = f;
    setHl(static_cast<u16>(result));
    idle();
}

// SP + signed immediate; shared by ADD SP,e and LD HL,SP+e.
u16 Cpu::addSpSigned() {
    u8 raw = fetch8();
    int offset = static_cast<s8>(raw);
    u16 sp = r_.sp;
    u8 f = 0;
    if (((sp & 0xF) + (raw & 0xF)) > 0xF) f |= FH;
    if (((sp & 0xFF) + raw) > 0xFF) f |= FC;
    r_.f = f;
    return static_cast<u16>(sp + offset);
}

void Cpu::daa() {
    u8 a = r_.a;
    bool carry = flag(FC);
    if (!flag(FN)) {
        if (carry || a > 0x99) {
            a = static_cast<u8>(a + 0x60);
            carry = true;
        }
        if (flag(FH) || (a & 0x0F) > 0x09) a = static_cast<u8>(a + 0x06);
    } else {
        if (carry) a = static_cast<u8>(a - 0x60);
        if (flag(FH)) a = static_cast<u8>(a - 0x06);
    }
    r_.a = a;
    r_.f = static_cast<u8>((r_.f & FN) | (a == 0 ? FZ : 0) | (carry ? FC : 0));
}

void Cpu::jumpRelative(bool taken) {
    s8 offset = static_cast<s8>(fetch8());
    if (taken) {
        r_.pc = static_cast<u16>(r_.pc + offset);
        idle();
    }
}

// ------------------------------------------------------------ opcodes ----

void Cpu::executeCb() {
    u8 op = fetch8();
    int reg = op & 7;
    int y = (op >> 3) & 7;
    u8 v = getReg(reg);
    switch (op >> 6) {
        case 0:
            setReg(reg, rotateShift(y, v));
            break;
        case 1:  // BIT
            r_.f = static_cast<u8>((r_.f & FC) | FH | (((v >> y) & 1) ? 0 : FZ));
            break;
        case 2:  // RES
            setReg(reg, static_cast<u8>(v & ~(1 << y)));
            break;
        default:  // SET
            setReg(reg, static_cast<u8>(v | (1 << y)));
            break;
    }
}

void Cpu::execute(u8 op) {
    // Regular blocks first: LD r,r' and ALU A,r.
    if (op >= 0x40 && op < 0x80) {
        if (op == 0x76) {
            halt();
        } else {
            setReg((op >> 3) & 7, getReg(op & 7));
        }
        return;
    }
    if (op >= 0x80 && op < 0xC0) {
        alu((op >> 3) & 7, getReg(op & 7));
        return;
    }

    switch (op) {
        case 0x00: break;  // NOP

        // LD rr,d16
        case 0x01: case 0x11: case 0x21: case 0x31:
            setPair(op >> 4, fetch16());
            break;

        // LD (rr),A / LD A,(rr)
        case 0x02: write8(bc(), r_.a); break;
        case 0x12: write8(de(), r_.a); break;
        case 0x22: write8(hl(), r_.a); setHl(static_cast<u16>(hl() + 1)); break;
        case 0x32: write8(hl(), r_.a); setHl(static_cast<u16>(hl() - 1)); break;
        case 0x0A: r_.a = read8(bc()); break;
        case 0x1A: r_.a = read8(de()); break;
        case 0x2A: r_.a = read8(hl()); setHl(static_cast<u16>(hl() + 1)); break;
        case 0x3A: r_.a = read8(hl()); setHl(static_cast<u16>(hl() - 1)); break;

        // INC rr / DEC rr
        case 0x03: case 0x13: case 0x23: case 0x33:
            setPair(op >> 4, static_cast<u16>(pair(op >> 4) + 1));
            idle();
            break;
        case 0x0B: case 0x1B: case 0x2B: case 0x3B:
            setPair(op >> 4, static_cast<u16>(pair(op >> 4) - 1));
            idle();
            break;

        // INC r / DEC r / LD r,d8
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
            int reg = (op >> 3) & 7;
            setReg(reg, inc8(getReg(reg)));
            break;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
            int reg = (op >> 3) & 7;
            setReg(reg, dec8(getReg(reg)));
            break;
        }
        case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E:
            setReg((op >> 3) & 7, fetch8());
            break;

        // Accumulator rotates (Z always cleared)
        case 0x07: r_.a = rotateShift(0, r_.a); r_.f &= FC; break;
        case 0x0F: r_.a = rotateShift(1, r_.a); r_.f &= FC; break;
        case 0x17: r_.a = rotateShift(2, r_.a); r_.f &= FC; break;
        case 0x1F: r_.a = rotateShift(3, r_.a); r_.f &= FC; break;

        case 0x08: {  // LD (a16),SP
            u16 addr = fetch16();
            write8(addr, static_cast<u8>(r_.sp));
            write8(static_cast<u16>(addr + 1), static_cast<u8>(r_.sp >> 8));
            break;
        }

        case 0x09: case 0x19: case 0x29: case 0x39:
            addHl(pair(op >> 4));
            break;

        case 0x10: stop(); break;

        // Relative jumps
        case 0x18: jumpRelative(true); break;
        case 0x20: jumpRelative(condition(0)); break;
        case 0x28: jumpRelative(condition(1)); break;
        case 0x30: jumpRelative(condition(2)); break;
        case 0x38: jumpRelative(condition(3)); break;

        case 0x27: daa(); break;
        case 0x2F: r_.a = static_cast<u8>(~r_.a); r_.f |= FN | FH; break;  // CPL
        case 0x37: r_.f = static_cast<u8>((r_.f & FZ) | FC); break;         // SCF
        case 0x3F: r_.f = static_cast<u8>((r_.f & FZ) | ((r_.f & FC) ^ FC)); break;  // CCF

        // RET cc / RET / RETI
        case 0xC0: case 0xC8: case 0xD0: case 0xD8:
            idle();
            if (condition((op >> 3) & 3)) {
                r_.pc = pop16();
                idle();
            }
            break;
        case 0xC9: r_.pc = pop16(); idle(); break;
        case 0xD9: r_.pc = pop16(); idle(); ime_ = true; break;

        // POP / PUSH
        case 0xC1: setBc(pop16()); break;
        case 0xD1: setDe(pop16()); break;
        case 0xE1: setHl(pop16()); break;
        case 0xF1: setAf(pop16()); break;
        case 0xC5: push16(bc()); break;
        case 0xD5: push16(de()); break;
        case 0xE5: push16(hl()); break;
        case 0xF5: push16(af()); break;

        // Absolute jumps
        case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
            u16 addr = fetch16();
            if (condition((op >> 3) & 3)) {
                r_.pc = addr;
                idle();
            }
            break;
        }
        case 0xC3: r_.pc = fetch16(); idle(); break;
        case 0xE9: r_.pc = hl(); break;

        // Calls
        case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
            u16 addr = fetch16();
            if (condition((op >> 3) & 3)) {
                push16(r_.pc);
                r_.pc = addr;
            }
            break;
        }
        case 0xCD: {
            u16 addr = fetch16();
            push16(r_.pc);
            r_.pc = addr;
            break;
        }
        case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
            push16(r_.pc);
            r_.pc = op & 0x38;
            break;

        // ALU A,d8
        case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
            alu((op >> 3) & 7, fetch8());
            break;

        case 0xCB: executeCb(); break;

        // High page loads
        case 0xE0: write8(static_cast<u16>(0xFF00 | fetch8()), r_.a); break;
        case 0xF0: r_.a = read8(static_cast<u16>(0xFF00 | fetch8())); break;
        case 0xE2: write8(static_cast<u16>(0xFF00 | r_.c), r_.a); break;
        case 0xF2: r_.a = read8(static_cast<u16>(0xFF00 | r_.c)); break;
        case 0xEA: write8(fetch16(), r_.a); break;
        case 0xFA: r_.a = read8(fetch16()); break;

        // Stack pointer arithmetic
        case 0xE8: r_.sp = addSpSigned(); idle(); idle(); break;
        case 0xF8: setHl(addSpSigned()); idle(); break;
        case 0xF9: r_.sp = hl(); idle(); break;

        case 0xF3: ime_ = false; eiDelay_ = 0; break;    // DI
        case 0xFB: if (!ime_ && eiDelay_ == 0) eiDelay_ = 2; break;  // EI

        default:
            // Illegal opcodes (D3, DB, DD, E3, E4, EB, EC, ED, F4, FC, FD)
            // lock up real hardware; treat as NOP to keep running.
            break;
    }
}

}  // namespace gb
