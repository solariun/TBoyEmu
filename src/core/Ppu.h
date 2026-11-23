#ifndef TBOYEMU_CORE_PPU_H
#define TBOYEMU_CORE_PPU_H

#include "FrameBuffer.h"
#include "InterruptController.h"
#include "Types.h"

namespace gb {

// Picture Processing Unit: LCD timing state machine, STAT interrupts,
// VRAM/OAM, CGB palettes and a scanline renderer.
class Ppu : private NonCopyable {
public:
    // Events reported by tick() so the bus can drive H-Blank DMA and the
    // machine can detect frame completion without callbacks.
    enum Event { EventNone = 0, EventHBlank = 1, EventFrame = 2 };

    Ppu(InterruptController& ic, bool cgbMode);

    void reset();
    u8 tick(int dots);

    u8 readVram(u16 addr) const { return vram_[vramBank_][addr & 0x1FFF]; }
    void writeVram(u16 addr, u8 v) { vram_[vramBank_][addr & 0x1FFF] = v; }
    u8 readOam(u16 addr) const { return oam_[addr & 0xFF]; }
    void writeOam(u16 addr, u8 v) { oam_[addr & 0xFF] = v; }
    void writeOamDirect(u8 index, u8 v) { oam_[index] = v; }

    u8 readRegister(u16 addr) const;
    void writeRegister(u16 addr, u8 value);

    bool lcdEnabled() const { return (lcdc_ & 0x80) != 0; }
    int mode() const { return mode_; }
    const FrameBuffer& frame() const { return front_; }

private:
    void setMode(int mode);
    void updateStatLine();
    void renderScanline();
    void renderBackground(u8* colorIndex, u8* bgPriority);
    void renderSprites(const u8* colorIndex, const u8* bgPriority);
    u16 dmgColor(u8 palette, u8 index) const;
    u16 cgbColor(const u8* paletteRam, int palette, int index) const;

    InterruptController& ic_;
    bool cgb_;

    u8 vram_[2][0x2000];
    u8 oam_[0xA0 + 0x60];  // padded so any 0xFExx index is valid
    u8 bgPalette_[64];
    u8 objPalette_[64];

    u8 lcdc_, stat_, scy_, scx_, ly_, lyc_, bgp_, obp0_, obp1_, wy_, wx_;
    u8 vramBank_, bcps_, ocps_, opri_;

    int mode_;
    int lineDot_;
    int offDots_;  // frame pacing while the LCD is off
    int windowLine_;
    bool windowYTriggered_;
    bool statLine_;

    FrameBuffer back_;   // being drawn
    FrameBuffer front_;  // last completed frame
};

}  // namespace gb

#endif
