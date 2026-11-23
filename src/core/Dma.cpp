#include "Dma.h"

#include "Bus.h"
#include "Ppu.h"

namespace gb {

// ------------------------------------------------------------- OAM DMA ----

OamDma::OamDma(Bus& bus, Ppu& ppu) : bus_(bus), ppu_(ppu), page_(0xFF), index_(-1), delay_(0) {}

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
    u16 src = static_cast<u16>((page_ << 8) | index_);
    if (src >= 0xE000) src = static_cast<u16>(src - 0x2000);  // echo RAM / high pages
    ppu_.writeOamDirect(static_cast<u8>(index_), bus_.dmaRead(src));
    if (++index_ >= 0xA0) index_ = -1;
}

// ---------------------------------------------------------------- HDMA ----

Hdma::Hdma(Bus& bus, Ppu& ppu)
    : bus_(bus), ppu_(ppu), source_(0), dest_(0), remaining_(0x7F), hblankActive_(false),
      stall_(0) {}

u8 Hdma::read(u16 addr) const {
    if (addr != 0xFF55) return 0xFF;
    return hblankActive_ ? remaining_ : static_cast<u8>(0x80 | remaining_);
}

int Hdma::stallPerBlock() const { return bus_.doubleSpeed() ? 16 : 8; }

void Hdma::write(u16 addr, u8 value) {
    switch (addr) {
        case 0xFF51: source_ = static_cast<u16>((value << 8) | (source_ & 0x00F0)); break;
        case 0xFF52: source_ = static_cast<u16>((source_ & 0xFF00) | (value & 0xF0)); break;
        case 0xFF53: dest_ = static_cast<u16>(((value & 0x1F) << 8) | (dest_ & 0x00F0)); break;
        case 0xFF54: dest_ = static_cast<u16>((dest_ & 0x1F00) | (value & 0xF0)); break;
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

void Hdma::copyBlock() {
    for (int i = 0; i < 16; ++i) {
        u8 v = bus_.dmaRead(source_);
        ppu_.writeVram(static_cast<u16>(0x8000 | (dest_ & 0x1FFF)), v);
        source_ = static_cast<u16>(source_ + 1);
        dest_ = static_cast<u16>((dest_ + 1) & 0x1FFF);
    }
}

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
