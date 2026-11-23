#include "Timer.h"

namespace gb {

namespace {
// Bit of the system counter feeding TIMA for each TAC clock select.
const u16 kTacBit[4] = {1u << 9, 1u << 3, 1u << 5, 1u << 7};
}  // namespace

Timer::Timer(InterruptController& ic)
    : ic_(ic), counter_(0), tima_(0), tma_(0), tac_(0xF8), reloadDelay_(0) {}

void Timer::reset(u16 initialCounter) {
    counter_ = initialCounter;
    tima_ = 0;
    tma_ = 0;
    tac_ = 0xF8;
    reloadDelay_ = 0;
}

bool Timer::selectedBit() const {
    return (tac_ & 0x04) && (counter_ & kTacBit[tac_ & 3]);
}

void Timer::incrementTima() {
    if (++tima_ == 0) {
        // TIMA reads 0 for one M-cycle, then TMA is loaded and IRQ fires.
        reloadDelay_ = 4;
    }
}

void Timer::tick(int cycles) {
    for (; cycles > 0; cycles -= 4) {
        if (reloadDelay_ > 0) {
            reloadDelay_ -= 4;
            if (reloadDelay_ <= 0) {
                tima_ = tma_;
                ic_.request(irq::Timer);
            }
        }
        bool before = selectedBit();
        counter_ = static_cast<u16>(counter_ + 4);
        if (before && !selectedBit()) incrementTima();
    }
}

u8 Timer::read(u16 addr) const {
    switch (addr) {
        case 0xFF04: return static_cast<u8>(counter_ >> 8);
        case 0xFF05: return tima_;
        case 0xFF06: return tma_;
        case 0xFF07: return static_cast<u8>(tac_ | 0xF8);
        default: return 0xFF;
    }
}

void Timer::write(u16 addr, u8 value) {
    switch (addr) {
        case 0xFF04: {
            bool before = selectedBit();
            counter_ = 0;
            if (before) incrementTima();
            break;
        }
        case 0xFF05:
            tima_ = value;
            reloadDelay_ = 0;  // writing during the reload window cancels it
            break;
        case 0xFF06:
            tma_ = value;
            break;
        case 0xFF07: {
            bool before = selectedBit();
            tac_ = static_cast<u8>(value | 0xF8);
            if (before && !selectedBit()) incrementTima();
            break;
        }
        default:
            break;
    }
}

}  // namespace gb
