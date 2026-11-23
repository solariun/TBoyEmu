#ifndef TBOYEMU_CORE_CPU_H
#define TBOYEMU_CORE_CPU_H

#include "Bus.h"
#include "InterruptController.h"
#include "Types.h"

namespace gb {

// Sharp SM83 core.  Every memory access goes through read8()/write8(),
// which advance the rest of the machine by one M-cycle, so the timer, PPU
// and DMA observe accesses in the correct order (M-cycle accuracy).
class Cpu : private NonCopyable {
public:
    struct Registers {
        u8 a, f, b, c, d, e, h, l;
        u16 sp, pc;
    };

    Cpu(Bus& bus, InterruptController& ic);

    void reset(bool cgbMode);
    void step();  // one instruction, interrupt dispatch or halted M-cycle

    const Registers& registers() const { return r_; }

private:
    // ---- bus access (each one costs one M-cycle) ----
    u8 read8(u16 addr) {
        bus_.tick();
        return bus_.read(addr);
    }
    void write8(u16 addr, u8 value) {
        bus_.tick();
        bus_.write(addr, value);
    }
    void idle() { bus_.tick(); }
    u8 fetch8();
    u16 fetch16();
    void push16(u16 value);
    u16 pop16();

    // ---- register helpers ----
    u16 bc() const { return static_cast<u16>((r_.b << 8) | r_.c); }
    u16 de() const { return static_cast<u16>((r_.d << 8) | r_.e); }
    u16 hl() const { return static_cast<u16>((r_.h << 8) | r_.l); }
    u16 af() const { return static_cast<u16>((r_.a << 8) | r_.f); }
    void setBc(u16 v) { r_.b = static_cast<u8>(v >> 8); r_.c = static_cast<u8>(v); }
    void setDe(u16 v) { r_.d = static_cast<u8>(v >> 8); r_.e = static_cast<u8>(v); }
    void setHl(u16 v) { r_.h = static_cast<u8>(v >> 8); r_.l = static_cast<u8>(v); }
    void setAf(u16 v) { r_.a = static_cast<u8>(v >> 8); r_.f = static_cast<u8>(v & 0xF0); }
    u16 pair(int index) const;  // BC, DE, HL, SP
    void setPair(int index, u16 v);

    u8 getReg(int index);  // B C D E H L (HL) A
    void setReg(int index, u8 value);

    bool condition(int cc) const;  // NZ Z NC C
    bool flag(u8 mask) const { return (r_.f & mask) != 0; }

    // ---- instruction groups ----
    void execute(u8 opcode);
    void executeCb();
    void alu(int operation, u8 value);
    u8 rotateShift(int operation, u8 value);
    u8 inc8(u8 v);
    u8 dec8(u8 v);
    void addHl(u16 v);
    u16 addSpSigned();
    void daa();
    void jumpRelative(bool taken);
    void halt();
    void stop();

    void serviceInterrupt(u8 pending);

    Bus& bus_;
    InterruptController& ic_;
    Registers r_;
    bool ime_;
    int eiDelay_;  // EI takes effect after the following instruction
    bool halted_;
    bool haltBug_;
};

}  // namespace gb

#endif
