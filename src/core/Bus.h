#ifndef TBOYEMU_CORE_BUS_H
#define TBOYEMU_CORE_BUS_H

#include "Apu.h"
#include "Cartridge.h"
#include "Dma.h"
#include "InterruptController.h"
#include "Joypad.h"
#include "Ppu.h"
#include "Serial.h"
#include "Timer.h"
#include "Types.h"

namespace gb {

// The memory map.  Routes every CPU access to the component that owns the
// address, owns work RAM / high RAM and the CGB speed switch, and advances
// all clocked hardware one M-cycle at a time (tick()).
class Bus : private NonCopyable {
public:
    Bus(Cartridge& cart, Ppu& ppu, Timer& timer, Joypad& joypad, Serial& serial, Apu& apu,
        InterruptController& ic, bool cgbMode);

    void reset();

    u8 read(u16 addr);
    void write(u16 addr, u8 value);
    u8 dmaRead(u16 addr);  // used by the DMA engines (no I/O side effects)

    // Advance the machine by one CPU M-cycle.
    void tick();

    int takeStallCycles() { return hdma_.takeStallCycles(); }
    bool consumeFrameReady() {
        bool f = frameReady_;
        frameReady_ = false;
        return f;
    }

    // CGB double speed (KEY1 / STOP).
    bool doubleSpeed() const { return doubleSpeed_; }
    bool speedSwitchArmed() const { return cgb_ && (key1_ & 1); }
    void switchSpeed();

private:
    u8 readIo(u16 addr);
    void writeIo(u16 addr, u8 value);

    Cartridge& cart_;
    Ppu& ppu_;
    Timer& timer_;
    Joypad& joypad_;
    Serial& serial_;
    Apu& apu_;
    InterruptController& ic_;
    bool cgb_;

    OamDma oamDma_;
    Hdma hdma_;

    u8 wram_[8][0x1000];
    u8 hram_[0x80];
    u8 wramBank_;
    u8 key1_;
    bool doubleSpeed_;
    bool frameReady_;
};

}  // namespace gb

#endif
