#ifndef TBOYEMU_GBA_TIMERS_H
#define TBOYEMU_GBA_TIMERS_H

#include "Common.h"

namespace gba {

// The four 16-bit timers (TM0CNT - TM3CNT) with prescalers and cascade.
class Timers : private NonCopyable {
public:
    explicit Timers(Interrupts& interrupts);

    void reset();
    void tick(int cycles);
    int cyclesUntilOverflow() const;  // lets a halted CPU skip ahead

    u16 read(u32 offset) const;  // offset from 0x04000100
    void write(u32 offset, u16 value);

private:
    struct Timer {
        u16 counter;
        u16 reload;
        u16 control;
        int prescalerAccum;  // cycles accumulated towards the next tick
    };
    bool running(int i) const { return (timers_[i].control & 0x80) != 0; }
    bool cascade(int i) const { return i > 0 && (timers_[i].control & 0x04); }
    void increment(int i, unsigned ticks);

    Interrupts& interrupts_;
    Timer timers_[4];
};

// KEYINPUT (active low) and KEYCNT (keypad interrupt).
class Keypad : private NonCopyable {
public:
    explicit Keypad(Interrupts& interrupts) : interrupts_(interrupts), state_(0x03FF), control_(0) {}

    void setButton(gb::Button button, bool pressed);
    u16 keyInput() const { return state_; }
    u16 control() const { return control_; }
    void setControl(u16 v) {
        control_ = v;
        checkInterrupt();
    }

private:
    void checkInterrupt();

    Interrupts& interrupts_;
    u16 state_;
    u16 control_;
};

}  // namespace gba

#endif
