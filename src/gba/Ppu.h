#ifndef TBOYEMU_GBA_PPU_H
#define TBOYEMU_GBA_PPU_H

#include "../core/FrameBuffer.h"
#include "Common.h"

namespace gba {

// GBA LCD controller: owns palette RAM, VRAM and OAM, keeps the scanline
// timing (DISPSTAT/VCOUNT and their interrupts) and draws one scanline at
// the start of each H-Blank: tiled modes 0-2 (text and affine
// backgrounds), bitmap modes 3-5, regular and affine sprites, windows
// (WIN0, WIN1, OBJ window), mosaic and color effects (alpha, brighten,
// darken).
class Ppu : private NonCopyable {
public:
    enum Event { EventHBlank = 1, EventVBlank = 2, EventFrame = 4 };

    explicit Ppu(Interrupts& interrupts);

    void reset();
    u8 tick(int cycles);  // returns Event bits
    int cyclesUntilEvent() const;

    u16 readIo(u32 offset) const;  // offset from 0x04000000, halfword aligned
    void writeIo(u32 offset, u16 value);

    u8* palette() { return palette_; }
    u8* vram() { return vram_; }
    u8* oam() { return oam_; }
    bool bitmapMode() const { return (dispcnt_ & 7) >= 3; }

    const gb::FrameBuffer& frame() const { return front_; }

private:
    // Per-pixel layer output: bit 15 set = opaque, bits 0-14 = RGB555.
    static const u16 kOpaque = 0x8000;

    void renderLine();
    void renderTextBg(int bg);
    void renderAffineBg(int bg);
    void renderBitmapBg(int mode);
    void renderSprites();
    void compose();

    u16 vram16(u32 offset) const { return static_cast<u16>(vram_[offset] | (vram_[offset + 1] << 8)); }
    u16 palette16(u32 index) const {
        return static_cast<u16>((palette_[index * 2] | (palette_[index * 2 + 1] << 8)) & 0x7FFF);
    }
    u16 oam16(u32 index) const { return static_cast<u16>(oam_[index * 2] | (oam_[index * 2 + 1] << 8)); }
    u8 windowMask(int x) const;

    Interrupts& interrupts_;

    u8 palette_[0x400];
    u8 vram_[0x18000];
    u8 oam_[0x400];

    // Registers
    u16 dispcnt_, dispstat_, vcount_;
    u16 bgcnt_[4], bghofs_[4], bgvofs_[4];
    s16 bgpa_[2], bgpb_[2], bgpc_[2], bgpd_[2];  // BG2 / BG3 affine matrix
    s32 bgx_[2], bgy_[2];                      // reference point registers
    s32 refX_[2], refY_[2];                    // internal, advanced per line
    u16 winh_[2], winv_[2], winin_, winout_, mosaic_;
    u16 bldcnt_, bldalpha_, bldy_;

    int lineCycle_;
    bool inHBlank_;

    // Scanline buffers
    u16 bgLine_[4][kScreenWidth];
    u16 objLine_[kScreenWidth];
    u8 objPriority_[kScreenWidth];
    bool objSemiTransparent_[kScreenWidth];
    bool objWindow_[kScreenWidth];

    gb::FrameBuffer back_;
    gb::FrameBuffer front_;
};

}  // namespace gba

#endif
