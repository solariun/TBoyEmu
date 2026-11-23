// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Cpu.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Declares Cpu, the Sharp SM83 processor of the Game Boy / Game Boy Color
//  (an 8-bit CPU similar to the Z80/8080). It holds the registers, fetches
//  and executes instructions (including the 0xCB-prefixed ones), handles
//  interrupts, HALT (with the HALT bug), STOP and the CGB speed switch.
//
//  How it fits in
//  --------------
//  Owned by GameBoy, constructed last. GameBoy::runFrame() calls step() in
//  a loop. Every memory access goes through read8()/write8(), and every
//  internal delay through idle(); each of these calls Bus::tick() once, so
//  the whole machine advances one M-cycle per CPU bus cycle. Interrupts come
//  from the InterruptController (IF/IE); IME lives here. Implementation:
//  Cpu.cpp.
//
//  Hardware notes
//  --------------
//  Pan Docs, "CPU Registers and Flags", "CPU Instruction Set", "Interrupts",
//  "halt", "Power Up Sequence":
//    Registers A F B C D E H L (pairs AF BC DE HL), SP, PC.
//    F: bit 7 Z (zero), 6 N (subtract), 5 H (half carry), 4 C (carry);
//       bits 0-3 always 0.
//    One M-cycle = 4 dots (T-cycles) in normal speed. Instruction lengths in
//    M-cycles come out naturally from the number of read8/write8/idle calls.
//  Opcode tables: https://gbdev.io/gb-opcodes/optables/
//
//  Contributing
//  ------------
//  Never touch memory without read8()/write8(), and add idle() for each
//  internal M-cycle an instruction takes, or timing tests will fail.
//  Test every change with Blargg cpu_instrs (all 11 must pass) and
//  instr_timing:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
// ============================================================================
#ifndef TBOYEMU_CORE_CPU_H
#define TBOYEMU_CORE_CPU_H

#include "Bus.h"
#include "InterruptController.h"
#include "Types.h"

namespace gb {

// Sharp SM83 core.  Every memory access goes through read8()/write8(),
// which advance the rest of the machine by one M-cycle, so the timer, PPU
// and DMA observe accesses in the correct order (M-cycle accuracy).
// Owned by GameBoy; holds references to the Bus and InterruptController.
class Cpu : private NonCopyable {
public:
    // The CPU registers. f holds the flags (Z N H C in bits 7-4).
    struct Registers {
        u8 a, f, b, c, d, e, h, l;
        u16 sp, pc;
    };

    Cpu(Bus& bus, InterruptController& ic);

    // Loads the register values the DMG or CGB boot ROM leaves behind and
    // starts at PC = 0x0100 (the cartridge entry point).
    void reset(bool cgbMode);
    // Executes one unit of work: first any VRAM DMA stall, then either one
    // halted M-cycle, one interrupt dispatch (5 M-cycles) or one instruction.
    void step();  // one instruction, interrupt dispatch or halted M-cycle

    // Read-only view of the registers (debugging / tests).
    const Registers& registers() const { return r_; }

private:
    // ---- bus access (each one costs one M-cycle) ----
    // The machine is ticked BEFORE the access, so the access sees the state
    // at the end of its M-cycle.
    u8 read8(u16 addr) {
        bus_.tick();
        return bus_.read(addr);
    }
    void write8(u16 addr, u8 value) {
        bus_.tick();
        bus_.write(addr, value);
    }
    // An internal M-cycle with no memory access.
    void idle() { bus_.tick(); }
    // Read the byte / little-endian word at PC and advance PC.
    u8 fetch8();
    u16 fetch16();
    // Stack: push16 costs 3 M-cycles (internal + 2 writes, high byte first),
    // pop16 costs 2 (low byte first).
    void push16(u16 value);
    u16 pop16();

    // ---- register helpers ----
    // 16-bit views of the register pairs. setAf() clears F bits 0-3.
    u16 bc() const { return static_cast<u16>((r_.b << 8) | r_.c); }
    u16 de() const { return static_cast<u16>((r_.d << 8) | r_.e); }
    u16 hl() const { return static_cast<u16>((r_.h << 8) | r_.l); }
    u16 af() const { return static_cast<u16>((r_.a << 8) | r_.f); }
    void setBc(u16 v) { r_.b = static_cast<u8>(v >> 8); r_.c = static_cast<u8>(v); }
    void setDe(u16 v) { r_.d = static_cast<u8>(v >> 8); r_.e = static_cast<u8>(v); }
    void setHl(u16 v) { r_.h = static_cast<u8>(v >> 8); r_.l = static_cast<u8>(v); }
    void setAf(u16 v) { r_.a = static_cast<u8>(v >> 8); r_.f = static_cast<u8>(v & 0xF0); }
    // Register pair by the 2-bit opcode field: 0 BC, 1 DE, 2 HL, 3 SP.
    u16 pair(int index) const;  // BC, DE, HL, SP
    void setPair(int index, u16 v);

    // 8-bit operand by the 3-bit opcode field: 0 B, 1 C, 2 D, 3 E, 4 H, 5 L,
    // 6 (HL) = memory at HL (costs an M-cycle), 7 A.
    u8 getReg(int index);  // B C D E H L (HL) A
    void setReg(int index, u8 value);

    // Branch condition by the 2-bit opcode field: 0 NZ, 1 Z, 2 NC, 3 C.
    bool condition(int cc) const;  // NZ Z NC C
    // true if any flag in mask is set.
    bool flag(u8 mask) const { return (r_.f & mask) != 0; }

    // ---- instruction groups ----
    // Executes a non-prefixed opcode that has already been fetched.
    void execute(u8 opcode);
    // Fetches and executes the opcode after the 0xCB prefix.
    void executeCb();
    // A = A <op> value and sets flags; operation 0-7 = ADD ADC SUB SBC AND
    // XOR OR CP.
    void alu(int operation, u8 value);
    // Rotates/shifts value and sets Z/C (N, H cleared); operation 0-7 =
    // RLC RRC RL RR SLA SRA SWAP SRL. Returns the result.
    u8 rotateShift(int operation, u8 value);
    // INC/DEC of an 8-bit value: Z, N, H updated, C preserved.
    u8 inc8(u8 v);
    u8 dec8(u8 v);
    // ADD HL,rr (2 M-cycles): Z preserved, H from bit 11, C from bit 15.
    void addHl(u16 v);
    // Fetches a signed offset e and returns SP + e with the flags of
    // ADD SP,e / LD HL,SP+e (Z and N cleared, H and C from the low byte).
    u16 addSpSigned();
    // Decimal adjust of A after a BCD addition or subtraction.
    void daa();
    // JR [cc],e: fetches e and jumps if taken (one extra M-cycle).
    void jumpRelative(bool taken);
    // HALT and STOP instruction semantics.
    void halt();
    void stop();

    // Dispatches the highest priority pending interrupt (5 M-cycles):
    // clears IME, pushes PC and jumps to its vector.
    void serviceInterrupt(u8 pending);

    Bus& bus_;
    InterruptController& ic_;
    Registers r_;
    // Interrupt Master Enable.
    bool ime_;
    int eiDelay_;  // EI takes effect after the following instruction
    // true while in HALT (or STOP low-power mode) waiting for an interrupt.
    bool halted_;
    // HALT bug pending: the next opcode fetch does not increment PC.
    bool haltBug_;
};

}  // namespace gb

#endif
