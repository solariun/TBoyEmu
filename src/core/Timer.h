// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Timer.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Declares Timer, the Game Boy divider and programmable timer: DIV (0xFF04),
//  TIMA (0xFF05), TMA (0xFF06) and TAC (0xFF07). Instead of counting each
//  register separately it models the real circuit: one free-running 16-bit
//  system counter (DIV is its upper byte) and a falling-edge detector on the
//  counter bit selected by TAC, which increments TIMA.
//
//  How it fits in
//  --------------
//  Owned by GameBoy. Bus::tick() calls tick(4) every CPU M-cycle; the Bus
//  routes 0xFF04-0xFF07 to read()/write(); Bus::switchSpeed() writes DIV
//  (a speed switch resets it). On TIMA overflow it requests irq::Timer.
//  Implementation: Timer.cpp.
//
//  Hardware notes
//  --------------
//  Pan Docs, "Timer and Divider Registers" and "Timer obscure behaviour":
//    DIV  upper 8 bits of the 16-bit system counter; any write resets the
//         whole counter to 0.
//    TIMA incremented on each falling edge of (TAC enable AND counter bit).
//    TMA  value loaded into TIMA after an overflow.
//    TAC  bit 2 enable, bits 0-1 clock select:
//         00 = 4096 Hz (bit 9), 01 = 262144 Hz (bit 3),
//         10 = 65536 Hz (bit 5), 11 = 16384 Hz (bit 7).
//  Because of the edge detector, writing DIV or changing TAC can increment
//  TIMA spuriously ("DIV/TAC write glitches"), and an overflow leaves TIMA
//  at 0 for one M-cycle before TMA is reloaded and the interrupt fires.
//
//  Contributing
//  ------------
//  Keep the edge-detector design: test ROMs and some games depend on the
//  glitches. Cycles are always a multiple of 4 (whole M-cycles). Test with:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
//  and Blargg instr_timing (which uses the timer to measure instructions).
// ============================================================================
#ifndef TBOYEMU_CORE_TIMER_H
#define TBOYEMU_CORE_TIMER_H

#include "InterruptController.h"
#include "Types.h"

namespace gb {

// DIV / TIMA / TMA / TAC.  Modeled on the real hardware: a 16-bit system
// counter whose selected bit drives TIMA through a falling-edge detector,
// which reproduces the well-known DIV/TAC write glitches.
// Owned by GameBoy; holds a reference to the shared InterruptController.
class Timer : private NonCopyable {
public:
    explicit Timer(InterruptController& ic);

    // Sets the system counter to the value the boot ROM leaves behind
    // (it differs between DMG and CGB) and clears TIMA/TMA/TAC.
    void reset(u16 initialCounter);
    // Advances the timer; processed in steps of 4 cycles (one M-cycle).
    void tick(int cycles);  // CPU clock cycles (multiple of 4)

    // CPU access to 0xFF04-0xFF07 (other addresses read 0xFF / are ignored).
    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);

private:
    // Current value of the edge detector input: TAC enabled AND the counter
    // bit selected by TAC bits 0-1.
    bool selectedBit() const;
    // TIMA++; on overflow starts the 4-cycle delayed reload from TMA.
    void incrementTima();

    InterruptController& ic_;
    // 16-bit system counter, incremented every clock cycle; DIV = counter_ >> 8.
    u16 counter_;
    // TIMA, TMA and TAC register values (TAC stored with bits 3-7 set).
    u8 tima_;
    u8 tma_;
    u8 tac_;
    int reloadDelay_;  // cycles until TMA is reloaded after an overflow
};

}  // namespace gb

#endif
