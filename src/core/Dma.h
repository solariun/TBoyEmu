// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Dma.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Declares the two Game Boy DMA engines:
//  - OamDma: the OAM DMA started by writing 0xFF46, which copies 160 bytes
//    (40 sprites x 4 bytes) from page XX00-XX9F into OAM, one byte per
//    M-cycle.
//  - Hdma: the Game Boy Color VRAM DMA (0xFF51-0xFF55), either a "general
//    purpose" transfer that copies everything at once, or an "H-Blank"
//    transfer that copies 16 bytes at the start of each H-Blank.
//
//  How it fits in
//  --------------
//  Both are members of Bus. Bus::tick() calls OamDma::tick() every M-cycle
//  and Hdma::onHBlank() when the PPU reports H-Blank. They read the source
//  through Bus::dmaRead() and write into the Ppu (writeOamDirect/writeVram).
//  The CPU learns how long it must stall for VRAM DMA through
//  Bus::takeStallCycles() -> Hdma::takeStallCycles(). Implementation: Dma.cpp.
//
//  Hardware notes
//  --------------
//  Pan Docs, "OAM DMA Transfer": writing XX to 0xFF46 copies XX00-XX9F to
//  FE00-FE9F, taking 160 M-cycles.
//  Pan Docs, "VRAM DMA Transfers" (CGB):
//    HDMA1/2 (FF51/52) source, low 4 bits ignored.
//    HDMA3/4 (FF53/54) destination inside VRAM, only bits 12-4 are used.
//    HDMA5 (FF55) write: bit 7 mode (0 general purpose, 1 H-Blank),
//                        bits 0-6 = (length / 16) - 1.
//    HDMA5 read: bit 7 = 1 when no H-Blank transfer is active, bits 0-6
//                remaining blocks minus one.
//    Writing bit 7 = 0 during an H-Blank transfer cancels it.
//  The CPU is halted while each block is copied.
//
//  Contributing
//  ------------
//  Not emulated: CPU bus conflicts during OAM DMA (the CPU can still read
//  all memory here). Test with cgb-acid2 and a CGB game using H-Blank DMA:
//      ./tboyemu --headless --frames 600 --screenshot shot.png game.zip
// ============================================================================
#ifndef TBOYEMU_CORE_DMA_H
#define TBOYEMU_CORE_DMA_H

#include "Types.h"

namespace gb {

class Bus;
class Ppu;

// OAM DMA (0xFF46): copies 160 bytes into sprite memory, one per M-cycle.
// Owned by Bus; holds references to the Bus (source) and Ppu (destination).
class OamDma : private NonCopyable {
public:
    OamDma(Bus& bus, Ppu& ppu);

    // Starts a transfer from page * 0x100 (restarts one already running).
    void start(u8 page);
    // Copies one byte if a transfer is running; called once per M-cycle.
    void tick();
    // Last value written to 0xFF46 (reads of 0xFF46 return it).
    u8 page() const { return page_; }

private:
    Bus& bus_;
    Ppu& ppu_;
    // High byte of the source address.
    u8 page_;
    int index_;  // next byte to copy, -1 when idle
    int delay_;  // start-up delay in M-cycles
};

// CGB VRAM DMA (0xFF51-0xFF55): general purpose (all at once) and H-Blank
// (16 bytes per H-Blank) transfers.  The CPU is stalled while copying.
// Owned by Bus; holds references to the Bus (source, speed) and Ppu (VRAM).
class Hdma : private NonCopyable {
public:
    Hdma(Bus& bus, Ppu& ppu);

    // HDMA5 (0xFF55) status read; other addresses read 0xFF.
    u8 read(u16 addr) const;
    // HDMA1-5 writes. Writing HDMA5 starts (or cancels) a transfer.
    void write(u16 addr, u8 value);
    // Called by the Bus at the start of each H-Blank: copies one 16-byte
    // block if an H-Blank transfer is active.
    void onHBlank();

    // M-cycles the CPU must stay stalled because of finished transfers.
    // Returns the accumulated count and resets it to 0.
    int takeStallCycles();

private:
    // Copies 16 bytes from source_ to VRAM at dest_ and advances both.
    void copyBlock();
    // CPU stall per 16-byte block, in M-cycles (8 normal, 16 double speed).
    int stallPerBlock() const;

    Bus& bus_;
    Ppu& ppu_;
    // Current source address (any memory, 16-byte aligned when written).
    u16 source_;
    // Current destination offset inside VRAM (0x0000-0x1FF0, 16-byte aligned).
    u16 dest_;
    u8 remaining_;  // blocks left minus one (FF55 low 7 bits)
    // An H-Blank transfer is armed and waiting for H-Blanks.
    bool hblankActive_;
    // Stall M-cycles not yet collected by the CPU.
    int stall_;
};

}  // namespace gb

#endif
