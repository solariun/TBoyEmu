// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Timers.h
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Declares two small I/O devices:
//   - Timers: the four 16-bit hardware timers TM0-TM3 (counter, reload value, prescaler,
//     cascade "count-up" mode, overflow interrupt);
//   - Keypad: the button state register KEYINPUT and the keypad interrupt control KEYCNT.
//
//  How it fits in
//  --------------
//  - Both are owned by Gba and share its Interrupts instance.
//  - Timers: Gba::runFrame() calls tick(cycles) after every CPU step, and uses
//    cyclesUntilOverflow() to fast-forward a halted CPU. Bus maps 0x04000100-0x0400010F to
//    read()/write().
//  - Keypad: the frontend sets buttons through gb::Machine::setButton() -> Gba::setButton()
//    -> Keypad::setButton(). Bus maps KEYINPUT (0x04000130) and KEYCNT (0x04000132).
//  Implementation: Timers.cpp.
//
//  Hardware notes
//  --------------
//  GBATEK "GBA Timers": TMxCNT_L (+0) reads the counter / writes the reload value;
//  TMxCNT_H (+2): bits 0-1 prescaler (1, 64, 256, 1024 cycles per tick), bit 2 count-up
//  (cascade: tick when the previous timer overflows; ignored for timer 0), bit 6 IRQ on
//  overflow, bit 7 start. On overflow the counter is reloaded.
//  GBATEK "GBA Keypad Input": KEYINPUT bits 0-9 = A, B, Select, Start, Right, Left, Up, Down,
//  R, L, 0 = pressed. KEYCNT: bits 0-9 button mask, bit 14 IRQ enable, bit 15 condition
//  (0 = any selected button, 1 = all selected buttons).
//
//  Contributing
//  ------------
//  - Timer overflows feed sound DMA on real hardware; sound is not emulated, so only the
//    interrupt side matters here.
//  - Test with the jsmolka gba-tests ROMs and games that rely on timer IRQs, e.g.
//    make && ./tboyemu --headless --frames 600 --screenshot out.png rom.gba
// ============================================================================
#ifndef TBOYEMU_GBA_TIMERS_H
#define TBOYEMU_GBA_TIMERS_H

#include "Common.h"

namespace gba {

// The four 16-bit timers (TM0CNT - TM3CNT) with prescalers and cascade.
// Owned by Gba; holds a reference to the shared Interrupts. Single-threaded.
class Timers : private NonCopyable {
public:
    explicit Timers(Interrupts& interrupts);

    // Stops all timers and zeroes counters, reload values and control registers.
    void reset();
    // Advances every running, non-cascaded timer by `cycles` CPU cycles (cascaded timers are
    // advanced by the overflows of their predecessor).
    void tick(int cycles);
    int cyclesUntilOverflow() const;  // lets a halted CPU skip ahead

    u16 read(u32 offset) const;  // offset from 0x04000100
    // Register write, offset from 0x04000100 (4 bytes per timer: +0 reload, +2 control).
    void write(u32 offset, u16 value);

private:
    // State of one timer. counter = current value (TMxCNT_L read), reload = value written to
    // TMxCNT_L (loaded on start and on overflow), control = TMxCNT_H (bits 0-2, 6, 7 kept).
    struct Timer {
        u16 counter;
        u16 reload;
        u16 control;
        int prescalerAccum;  // cycles accumulated towards the next tick
    };
    // Control bit 7: timer started.
    bool running(int i) const { return (timers_[i].control & 0x80) != 0; }
    // Control bit 2: count-up mode. Timer 0 has no predecessor, so the bit is ignored there.
    bool cascade(int i) const { return i > 0 && (timers_[i].control & 0x04); }
    // Adds `ticks` to timer i, handling any number of overflows (reload, IRQ, cascade).
    void increment(int i, unsigned ticks);

    // Shared interrupt controller (owned by Gba).
    Interrupts& interrupts_;
    // TM0..TM3.
    Timer timers_[4];
};

// KEYINPUT (active low) and KEYCNT (keypad interrupt).
// Owned by Gba. state_ uses the hardware polarity: bit = 0 while the button is held.
class Keypad : private NonCopyable {
public:
    // All 10 buttons released (0x03FF), keypad interrupt disabled.
    explicit Keypad(Interrupts& interrupts) : interrupts_(interrupts), state_(0x03FF), control_(0) {}

    // Updates one button from the frontend and re-evaluates the keypad interrupt.
    void setButton(gb::Button button, bool pressed);
    // KEYINPUT value (0x04000130).
    u16 keyInput() const { return state_; }
    // KEYCNT value (0x04000132) and its write, which may raise the interrupt immediately.
    u16 control() const { return control_; }
    void setControl(u16 v) {
        control_ = v;
        checkInterrupt();
    }

private:
    // Requests irq::Keypad if KEYCNT enables it and its button condition is met.
    void checkInterrupt();

    // Shared interrupt controller (owned by Gba).
    Interrupts& interrupts_;
    // KEYINPUT bits 0-9 (0 = pressed) and KEYCNT as written.
    u16 state_;
    u16 control_;
};

}  // namespace gba

#endif
