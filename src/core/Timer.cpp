// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Timer.cpp
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Implements the DIV/TIMA/TMA/TAC timer declared in Timer.h using the
//  system-counter + falling-edge-detector model, including the delayed TMA
//  reload after an overflow and the glitches caused by DIV and TAC writes.
//
//  How it fits in
//  --------------
//  Bus::tick() calls Timer::tick(4) once per CPU M-cycle (before the CPU's
//  memory access of that cycle). Bus::readIo()/writeIo() call read()/write().
//  GameBoy::reset() calls reset() with the post-boot counter value.
//
//  Hardware notes
//  --------------
//  Pan Docs, "Timer and Divider Registers" / "Timer obscure behaviour":
//  - TIMA increments when (TAC.2 AND counter bit N) goes from 1 to 0.
//    Writing DIV (counter reset to 0) or changing TAC can cause that edge
//    too, which is why write() checks the detector before and after.
//  - On overflow TIMA reads 0x00 for one M-cycle (4 cycles); then TMA is
//    copied into TIMA and the Timer interrupt is requested.
//  - Writing TIMA during that M-cycle cancels the reload and the interrupt.
//  The timer runs from the CPU clock, so in CGB double speed it runs twice
//  as fast in real time (the Bus still passes 4 cycles per M-cycle).
//
//  Contributing
//  ------------
//  Test with: make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
//  and Blargg instr_timing (README.md, "Testing").
// ============================================================================
#include "Timer.h"

namespace gb {

namespace {
// Bit of the system counter feeding TIMA for each TAC clock select.
// Index = TAC bits 0-1: 0 -> bit 9 (4096 Hz), 1 -> bit 3 (262144 Hz),
// 2 -> bit 5 (65536 Hz), 3 -> bit 7 (16384 Hz). The frequency is
// 4194304 / 2^(bit+1) because one full period of bit N is 2^(N+1) cycles.
const u16 kTacBit[4] = {1u << 9, 1u << 3, 1u << 5, 1u << 7};
}  // namespace

Timer::Timer(InterruptController& ic)
    : ic_(ic), counter_(0), tima_(0), tma_(0), tac_(0xF8), reloadDelay_(0) {}

// initialCounter: the internal counter value at the moment the boot ROM
// hands control to the cartridge (chosen by GameBoy::reset()).
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
    // Process one M-cycle (4 clock cycles) per iteration.
    for (; cycles > 0; cycles -= 4) {
        // First finish a pending overflow reload from the previous M-cycle.
        if (reloadDelay_ > 0) {
            reloadDelay_ -= 4;
            if (reloadDelay_ <= 0) {
                tima_ = tma_;
                ic_.request(irq::Timer);
            }
        }
        // Then advance the system counter and look for a falling edge on the
        // selected bit (1 -> 0), which clocks TIMA.
        bool before = selectedBit();
        counter_ = static_cast<u16>(counter_ + 4);
        if (before && !selectedBit()) incrementTima();
    }
}

u8 Timer::read(u16 addr) const {
    switch (addr) {
        // DIV: upper byte of the system counter.
        case 0xFF04: return static_cast<u8>(counter_ >> 8);
        case 0xFF05: return tima_;
        case 0xFF06: return tma_;
        // TAC: bits 3-7 unused, read as 1.
        case 0xFF07: return static_cast<u8>(tac_ | 0xF8);
        default: return 0xFF;
    }
}

void Timer::write(u16 addr, u8 value) {
    switch (addr) {
        // DIV write: resets the whole 16-bit counter. If the selected bit
        // was 1, it drops to 0 and TIMA is incremented (DIV write glitch).
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
        // TAC write: disabling the timer or switching to a bit that is 0 can
        // produce a falling edge on the detector input (TAC write glitch).
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
