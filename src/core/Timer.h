#ifndef TBOYEMU_CORE_TIMER_H
#define TBOYEMU_CORE_TIMER_H

#include "InterruptController.h"
#include "Types.h"

namespace gb {

// DIV / TIMA / TMA / TAC.  Modeled on the real hardware: a 16-bit system
// counter whose selected bit drives TIMA through a falling-edge detector,
// which reproduces the well-known DIV/TAC write glitches.
class Timer : private NonCopyable {
public:
    explicit Timer(InterruptController& ic);

    void reset(u16 initialCounter);
    void tick(int cycles);  // CPU clock cycles (multiple of 4)

    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);

private:
    bool selectedBit() const;
    void incrementTima();

    InterruptController& ic_;
    u16 counter_;
    u8 tima_;
    u8 tma_;
    u8 tac_;
    int reloadDelay_;  // cycles until TMA is reloaded after an overflow
};

}  // namespace gb

#endif
