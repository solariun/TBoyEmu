#ifndef TBOYEMU_CORE_DMA_H
#define TBOYEMU_CORE_DMA_H

#include "Types.h"

namespace gb {

class Bus;
class Ppu;

// OAM DMA (0xFF46): copies 160 bytes into sprite memory, one per M-cycle.
class OamDma : private NonCopyable {
public:
    OamDma(Bus& bus, Ppu& ppu);

    void start(u8 page);
    void tick();
    u8 page() const { return page_; }

private:
    Bus& bus_;
    Ppu& ppu_;
    u8 page_;
    int index_;  // next byte to copy, -1 when idle
    int delay_;  // start-up delay in M-cycles
};

// CGB VRAM DMA (0xFF51-0xFF55): general purpose (all at once) and H-Blank
// (16 bytes per H-Blank) transfers.  The CPU is stalled while copying.
class Hdma : private NonCopyable {
public:
    Hdma(Bus& bus, Ppu& ppu);

    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);
    void onHBlank();

    // M-cycles the CPU must stay stalled because of finished transfers.
    int takeStallCycles();

private:
    void copyBlock();
    int stallPerBlock() const;

    Bus& bus_;
    Ppu& ppu_;
    u16 source_;
    u16 dest_;
    u8 remaining_;  // blocks left minus one (FF55 low 7 bits)
    bool hblankActive_;
    int stall_;
};

}  // namespace gb

#endif
