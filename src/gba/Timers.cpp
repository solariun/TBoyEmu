// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Timers.cpp
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Implementation of Timers and Keypad (see Timers.h for the register layouts):
//   - timer counting with prescalers, batch-advanced by whole CPU instructions;
//   - overflow handling: reload, optional interrupt, and cascade into the next timer;
//   - the "cycles until next overflow" estimate used to fast-forward HALT;
//   - mapping of emulator buttons to KEYINPUT bits and the KEYCNT interrupt condition.
//
//  How it fits in
//  --------------
//  Gba::runFrame() -> Timers::tick() after every CPU step; Bus -> read()/write() and the
//  Keypad accessors; frontend -> Gba::setButton() -> Keypad::setButton().
//
//  Hardware notes
//  --------------
//  GBATEK "GBA Timers" and "GBA Keypad Input". A timer is ticked every 1/64/256/1024 CPU
//  cycles; it raises IRQ Timer n (IF bit 3+n) on overflow from 0xFFFF when TMxCNT_H bit 6 is
//  set. Writing TMxCNT_L only changes the reload value; the counter is loaded from it when
//  the timer is started (bit 7 going 0 -> 1) and on each overflow.
//
//  Contributing
//  ------------
//  - The prescaler accumulator is reset on start, so the first tick happens a full prescaler
//    period after starting (real hardware has a small extra start-up delay, not modelled).
//  - Keep cyclesUntilOverflow() conservative (never later than the real overflow), otherwise
//    a halted CPU would wake up late.
// ============================================================================
#include "Timers.h"

namespace gba {

namespace {
// CPU cycles per timer tick for prescaler selection 0-3 (TMxCNT_H bits 0-1).
const int kPrescaler[4] = {1, 64, 256, 1024};
}

Timers::Timers(Interrupts& interrupts) : interrupts_(interrupts) { reset(); }

void Timers::reset() {
    for (int i = 0; i < 4; ++i) {
        timers_[i].counter = 0;
        timers_[i].reload = 0;
        timers_[i].control = 0;
        timers_[i].prescalerAccum = 0;
    }
}

// Adds `ticks` to timer i. Each time the counter passes 0xFFFF it is reloaded, the overflow
// IRQ is requested if enabled (control bit 6), and the next timer gets one tick if it is
// running in cascade mode. The loop handles several overflows per call (possible with a
// large batch of cycles, or a reload value close to 0xFFFF).
void Timers::increment(int i, unsigned ticks) {
    Timer& t = timers_[i];
    while (ticks > 0) {
        // Ticks left until the counter wraps (1..0x10000).
        unsigned toOverflow = 0x10000u - t.counter;
        if (ticks < toOverflow) {
            t.counter = static_cast<u16>(t.counter + ticks);
            return;
        }
        ticks -= toOverflow;
        t.counter = t.reload;
        if (t.control & 0x40) interrupts_.request(static_cast<u16>(irq::Timer0 << i));
        if (i < 3 && running(i + 1) && cascade(i + 1)) increment(i + 1, 1);
    }
}

// Converts elapsed CPU cycles into timer ticks for each free-running timer. The remainder
// that does not make a whole tick stays in prescalerAccum for the next call, so no cycles
// are lost. Cascaded timers are skipped here: they only move in increment().
void Timers::tick(int cycles) {
    for (int i = 0; i < 4; ++i) {
        Timer& t = timers_[i];
        if (!running(i) || cascade(i)) continue;
        int prescaler = kPrescaler[t.control & 3];
        t.prescalerAccum += cycles;
        if (t.prescalerAccum >= prescaler) {
            unsigned ticks = static_cast<unsigned>(t.prescalerAccum / prescaler);
            t.prescalerAccum -= static_cast<int>(ticks) * prescaler;
            increment(i, ticks);
        }
    }
}

// Smallest number of CPU cycles until a free-running timer with its IRQ enabled overflows,
// so a halted CPU can skip straight to it. Timers without IRQ are ignored, and so are
// cascaded timers (their overflow depends on the timer before them; they are still updated
// whenever tick() runs, so their IRQ is seen at the latest at the next skip boundary).
// Returns a large value (2^30) when nothing is pending, and at least 1.
int Timers::cyclesUntilOverflow() const {
    int best = 1 << 30;
    for (int i = 0; i < 4; ++i) {
        const Timer& t = timers_[i];
        if (!running(i) || cascade(i) || !(t.control & 0x40)) continue;
        int prescaler = kPrescaler[t.control & 3];
        int cycles = static_cast<int>(0x10000 - t.counter) * prescaler - t.prescalerAccum;
        if (cycles < best) best = cycles;
    }
    return best < 1 ? 1 : best;
}

// offset >> 2 selects the timer (4 bytes each); offset bit 1 selects control (TMxCNT_H)
// versus counter (TMxCNT_L). Reading TMxCNT_L returns the live counter, not the reload.
u16 Timers::read(u32 offset) const {
    int i = static_cast<int>(offset >> 2);
    if (i > 3) return 0;
    return (offset & 2) ? timers_[i].control : timers_[i].counter;
}

// TMxCNT_L write: new reload value only. TMxCNT_H write: keep bits 0-2 (prescaler, cascade)
// and 6-7 (IRQ, start) = mask 0xC7; a 0 -> 1 transition of the start bit restarts the timer.
void Timers::write(u32 offset, u16 value) {
    int i = static_cast<int>(offset >> 2);
    if (i > 3) return;
    Timer& t = timers_[i];
    if (!(offset & 2)) {
        t.reload = value;
        return;
    }
    bool wasRunning = running(i);
    t.control = value & 0xC7;
    if (!wasRunning && running(i)) {
        t.counter = t.reload;  // starting a timer loads the reload value
        t.prescalerAccum = 0;
    }
}

// ---------------------------------------------------------------- Keypad ----

// Maps gb::Button (core/Types.h order: Right, Left, Up, Down, A, B, Select, Start, L, R) to
// its KEYINPUT bit and updates the active-low state.
// Note for contributors: GBATEK lists bit 8 = R and bit 9 = L, while this table maps
// Button::L to bit 8 and Button::R to bit 9, i.e. the two shoulder buttons appear swapped
// relative to GBATEK. Verify against a game before changing it.
void Keypad::setButton(gb::Button button, bool pressed) {
    // KEYINPUT bit order: A B Select Start Right Left Up Down R L
    static const int bitFor[gb::kButtonCount] = {4, 5, 6, 7, 0, 1, 2, 3, 8, 9};
    u16 bit = static_cast<u16>(1u << bitFor[static_cast<int>(button)]);
    if (pressed) {
        state_ &= static_cast<u16>(~bit);
    } else {
        state_ |= bit;
    }
    checkInterrupt();
}

// KEYCNT: bit 14 enables the interrupt; bits 0-9 select buttons; bit 15 chooses the
// condition: 0 = logical OR (any selected button held), 1 = logical AND (all held).
// Evaluated on every button change and KEYCNT write (not continuously), so the IRQ is
// requested again on each change while the condition holds.
void Keypad::checkInterrupt() {
    if (!(control_ & 0x4000)) return;
    u16 wanted = control_ & 0x03FF;
    u16 held = static_cast<u16>(~state_ & 0x03FF);
    bool fire = (control_ & 0x8000) ? (held & wanted) == wanted : (held & wanted) != 0;
    if (fire) interrupts_.request(irq::Keypad);
}

}  // namespace gba
