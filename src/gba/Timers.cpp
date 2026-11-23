#include "Timers.h"

namespace gba {

namespace {
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

void Timers::increment(int i, unsigned ticks) {
    Timer& t = timers_[i];
    while (ticks > 0) {
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

u16 Timers::read(u32 offset) const {
    int i = static_cast<int>(offset >> 2);
    if (i > 3) return 0;
    return (offset & 2) ? timers_[i].control : timers_[i].counter;
}

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

void Keypad::checkInterrupt() {
    if (!(control_ & 0x4000)) return;
    u16 wanted = control_ & 0x03FF;
    u16 held = static_cast<u16>(~state_ & 0x03FF);
    bool fire = (control_ & 0x8000) ? (held & wanted) == wanted : (held & wanted) != 0;
    if (fire) interrupts_.request(irq::Keypad);
}

}  // namespace gba
