// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Dma.cpp
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Implements OamDma (sprite table copy, one byte per M-cycle after a
//  one M-cycle start-up delay) and Hdma (CGB VRAM DMA in general purpose
//  and H-Blank modes, with the CPU stall it causes). See Dma.h for the
//  register layout and the interface.
//
//  How it fits in
//  --------------
//  Bus::tick() -> OamDma::tick(); Bus::tick() on a PPU H-Blank event ->
//  Hdma::onHBlank(); Bus::writeIo() -> OamDma::start() / Hdma::write();
//  Cpu::step() idles for Bus::takeStallCycles() M-cycles before each step.
//
//  Hardware notes
//  --------------
//  Pan Docs, "OAM DMA Transfer" and "VRAM DMA Transfers". Sources at
//  0xE000 and above are folded down by 0x2000 for OAM DMA (on hardware those
//  pages read the WRAM mirror). Each 16-byte VRAM DMA block stalls the CPU
//  for 8 M-cycles in normal speed (16 in double speed, as coded in
//  stallPerBlock()). When an H-Blank DMA is armed with the LCD off, one block
//  is copied immediately because no H-Blank would ever arrive.
//
//  Contributing
//  ------------
//  remaining_ uses the hardware encoding (blocks - 1, 0x7F = done) so reads
//  of 0xFF55 need no conversion. Test with cgb-acid2 (README.md, "Testing").
// ============================================================================
#include "Dma.h"

#include "Bus.h"
#include "Ppu.h"

namespace gb {

// ------------------------------------------------------------- OAM DMA ----

OamDma::OamDma(Bus& bus, Ppu& ppu) : bus_(bus), ppu_(ppu), page_(0xFF), index_(-1), delay_(0) {}

// The first byte is copied one M-cycle after the write to 0xFF46.
void OamDma::start(u8 page) {
    page_ = page;
    index_ = 0;
    delay_ = 1;
}

void OamDma::tick() {
    if (index_ < 0) return;
    if (delay_ > 0) {
        --delay_;
        return;
    }
    // Source = page * 0x100 + index (0x00-0x9F).
    u16 src = static_cast<u16>((page_ << 8) | index_);
    if (src >= 0xE000) src = static_cast<u16>(src - 0x2000);  // echo RAM / high pages
    ppu_.writeOamDirect(static_cast<u8>(index_), bus_.dmaRead(src));
    // 0xA0 = 160 bytes copied: transfer finished.
    if (++index_ >= 0xA0) index_ = -1;
}

// ---------------------------------------------------------------- HDMA ----

Hdma::Hdma(Bus& bus, Ppu& ppu)
    : bus_(bus), ppu_(ppu), source_(0), dest_(0), remaining_(0x7F), hblankActive_(false),
      stall_(0) {}

// HDMA5: bit 7 = 0 while an H-Blank transfer is active, 1 when idle;
// bits 0-6 = blocks remaining minus one (0x7F when done).
u8 Hdma::read(u16 addr) const {
    if (addr != 0xFF55) return 0xFF;
    return hblankActive_ ? remaining_ : static_cast<u8>(0x80 | remaining_);
}

int Hdma::stallPerBlock() const { return bus_.doubleSpeed() ? 16 : 8; }

void Hdma::write(u16 addr, u8 value) {
    switch (addr) {
        // HDMA1/HDMA2: source high / low byte (low 4 bits forced to 0).
        case 0xFF51: source_ = static_cast<u16>((value << 8) | (source_ & 0x00F0)); break;
        case 0xFF52: source_ = static_cast<u16>((source_ & 0xFF00) | (value & 0xF0)); break;
        // HDMA3/HDMA4: destination high (5 bits) / low byte (low 4 bits forced to 0),
        // stored as an offset inside the 8 KiB VRAM.
        case 0xFF53: dest_ = static_cast<u16>(((value & 0x1F) << 8) | (dest_ & 0x00F0)); break;
        case 0xFF54: dest_ = static_cast<u16>((dest_ & 0x1F00) | (value & 0xF0)); break;
        // HDMA5: length, mode and start.
        case 0xFF55:
            if (hblankActive_ && !(value & 0x80)) {
                hblankActive_ = false;  // cancel; remaining length stays readable
                break;
            }
            remaining_ = value & 0x7F;
            if (value & 0x80) {
                hblankActive_ = true;
                // With the LCD off no H-Blank ever comes: copy a block now.
                if (!ppu_.lcdEnabled()) onHBlank();
            } else {
                // General purpose DMA: everything at once, CPU halted.
                int blocks = remaining_ + 1;
                for (int i = 0; i < blocks; ++i) copyBlock();
                remaining_ = 0x7F;
                stall_ += blocks * stallPerBlock();
            }
            break;
        default: break;
    }
}

// Copies 16 bytes. Writes go to the currently selected VRAM bank (VBK),
// and the destination wraps inside the 8 KiB VRAM window.
void Hdma::copyBlock() {
    for (int i = 0; i < 16; ++i) {
        u8 v = bus_.dmaRead(source_);
        ppu_.writeVram(static_cast<u16>(0x8000 | (dest_ & 0x1FFF)), v);
        source_ = static_cast<u16>(source_ + 1);
        dest_ = static_cast<u16>((dest_ + 1) & 0x1FFF);
    }
}

// One block per H-Blank; after the last block the transfer ends and
// HDMA5 reads 0xFF (inactive, 0x7F remaining).
void Hdma::onHBlank() {
    if (!hblankActive_) return;
    copyBlock();
    stall_ += stallPerBlock();
    if (remaining_ == 0) {
        remaining_ = 0x7F;
        hblankActive_ = false;
    } else {
        --remaining_;
    }
}

int Hdma::takeStallCycles() {
    int s = stall_;
    stall_ = 0;
    return s;
}

}  // namespace gb
