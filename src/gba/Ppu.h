// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Ppu.h
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Declares gba::Ppu, the GBA picture processing unit (LCD controller). It owns the three
//  video memories (palette RAM, VRAM, OAM), holds every LCD I/O register (0x04000000 -
//  0x04000055), advances the scanline timing (DISPSTAT / VCOUNT, H-Blank / V-Blank /
//  V-Counter interrupts) and renders one full scanline at a time into a double-buffered
//  240x160 RGB555 frame. Supported: tiled modes 0-2 (text and affine backgrounds), bitmap
//  modes 3-5, regular and affine sprites, windows (WIN0, WIN1, OBJ window), mosaic for
//  text backgrounds, and color special effects (alpha blend, brighten, darken).
//
//  How it fits in
//  --------------
//  - Owned by Gba (Gba.h), constructed with a reference to the shared Interrupts.
//  - Gba::runFrame() calls tick(cycles) after every CPU instruction; the returned Event bits
//    tell Gba to trigger H-Blank / V-Blank DMA and when a frame is complete.
//  - Bus forwards I/O accesses below 0x04000060 to readIo()/writeIo() and reads/writes the
//    raw memories through palette(), vram(), oam() (it also asks bitmapMode() to apply the
//    VRAM byte-write rules).
//  - The frontend reads the finished picture through frame() (via the gb::Machine
//    interface implemented by Gba).
//  Implementation: Ppu.cpp.
//
//  Hardware notes
//  --------------
//  - Memories: palette RAM 1 KiB at 0x05000000 (256 BG colors + 256 OBJ colors, RGB555,
//    little endian), VRAM 96 KiB at 0x06000000 (BG area 0x00000-0x0FFFF, OBJ tiles
//    0x10000-0x17FFF), OAM 1 KiB at 0x07000000 (128 sprites * 8 bytes, with the 32
//    affine parameter sets interleaved in the 4th halfword of each entry).
//  - Timing: 1232 cycles per line, H-Blank from cycle 960, 228 lines per frame, V-Blank
//    from line 160. See GBATEK "LCD Dimensions and Timings", "LCD I/O Display Control".
//  - Colors are 15-bit BGR: bits 0-4 red, 5-9 green, 10-14 blue.
//
//  Contributing
//  ------------
//  - Rendering happens once per line at the start of H-Blank, so mid-scanline register
//    changes are not visible (documented limitation in README.md).
//  - Test with the jsmolka gba-tests ROMs and real games in headless mode, comparing
//    screenshots:  make && ./tboyemu --headless --frames 600 --screenshot out.png rom.gba
// ============================================================================
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
// Ownership/lifetime: a member of Gba, lives as long as the console. Single-threaded.
// The object is large (about 100 KiB of video memory plus two frame buffers) and is never
// copied (NonCopyable).
class Ppu : private NonCopyable {
public:
    // Bit flags returned by tick(), OR-ed together when several happen in one call:
    //  EventHBlank - a visible line (0-159) entered H-Blank: start H-Blank DMA.
    //  EventVBlank - line 160 was reached: start V-Blank DMA.
    //  EventFrame  - a complete frame is in frame(); Gba::runFrame() returns on it.
    enum Event { EventHBlank = 1, EventVBlank = 2, EventFrame = 4 };

    // Keeps a reference to the shared interrupt controller (must outlive the Ppu).
    explicit Ppu(Interrupts& interrupts);

    // Clears all video memory and registers to power-on values (forced blank on, white
    // frame, identity affine matrices).
    void reset();
    // Advances the LCD by `cycles` CPU cycles, rendering lines and raising interrupts as
    // line boundaries are crossed. May cross several events in one call.
    u8 tick(int cycles);  // returns Event bits
    // Cycles until the next H-Blank start or end of line; used by Gba to fast-forward a
    // halted CPU straight to the next point where an interrupt may fire.
    int cyclesUntilEvent() const;

    // LCD register access for the Bus. Unknown and write-only registers read as 0.
    u16 readIo(u32 offset) const;  // offset from 0x04000000, halfword aligned
    // Writes one 16-bit LCD register (offset from 0x04000000, halfword aligned).
    void writeIo(u32 offset, u16 value);

    // Raw pointers to the video memories for the Bus (sizes: 0x400, 0x18000, 0x400 bytes).
    u8* palette() { return palette_; }
    u8* vram() { return vram_; }
    u8* oam() { return oam_; }
    // True in display modes 3-5 (DISPCNT bits 0-2 >= 3), where VRAM 0x10000-0x13FFF belongs
    // to the bitmap instead of sprite tiles.
    bool bitmapMode() const { return (dispcnt_ & 7) >= 3; }

    // Last completed frame (240x160, RGB555). Stays stable while the next one is drawn.
    const gb::FrameBuffer& frame() const { return front_; }

private:
    // Per-pixel layer output: bit 15 set = opaque, bits 0-14 = RGB555.
    // A value of 0 in a line buffer therefore means "transparent / nothing drawn here".
    static const u16 kOpaque = 0x8000;

    // Draws scanline vcount_ into back_: clears the layer buffers, renders the enabled
    // backgrounds for the current mode, then sprites, then calls compose().
    void renderLine();
    // Mode 0/1 text (scrolling, tiled) background `bg` (0-3) into bgLine_[bg].
    void renderTextBg(int bg);
    // Mode 1/2 rotation/scaling background `bg` (2 or 3) into bgLine_[bg].
    void renderAffineBg(int bg);
    // Bitmap modes 3/4/5 (BG2 only) into bgLine_[2]; `mode` is the DISPCNT mode.
    void renderBitmapBg(int mode);
    // All 128 OAM entries for the current line into objLine_ / objPriority_ /
    // objSemiTransparent_ / objWindow_.
    void renderSprites();
    // Merges the layer buffers into the final line: priority sorting, windows, blending.
    void compose();

    // Little-endian 16-bit reads from the memories. vram16 takes a byte offset;
    // palette16 and oam16 take a halfword index. palette16 drops bit 15 (unused in colors).
    u16 vram16(u32 offset) const { return static_cast<u16>(vram_[offset] | (vram_[offset + 1] << 8)); }
    u16 palette16(u32 index) const {
        return static_cast<u16>((palette_[index * 2] | (palette_[index * 2 + 1] << 8)) & 0x7FFF);
    }
    u16 oam16(u32 index) const { return static_cast<u16>(oam_[index * 2] | (oam_[index * 2 + 1] << 8)); }
    // Returns the 6-bit layer-enable mask that applies to pixel x of the current line
    // (bits 0-3 BG0-3, bit 4 OBJ, bit 5 color effects), from WININ/WINOUT.
    u8 windowMask(int x) const;

    // Shared interrupt controller (owned by Gba).
    Interrupts& interrupts_;

    // Video memories: palette RAM (0x05000000, 1 KiB: BG colors 0-255 then OBJ colors
    // 256-511), VRAM (0x06000000, 96 KiB), OAM (0x07000000, 1 KiB).
    u8 palette_[0x400];
    u8 vram_[0x18000];
    u8 oam_[0x400];

    // Registers
    // dispcnt_  DISPCNT 0x000: bits 0-2 mode, 4 frame select (modes 4/5), 6 OBJ 1D mapping,
    //           7 forced blank, 8-11 BG0-3 enable, 12 OBJ enable, 13-15 WIN0/WIN1/OBJWIN enable.
    // dispstat_ DISPSTAT 0x004: bit 0 V-Blank, 1 H-Blank, 2 V-Counter match (read-only flags),
    //           3/4/5 V-Blank/H-Blank/V-Counter IRQ enable, bits 8-15 V-Count setting (LYC).
    // vcount_   VCOUNT 0x006: current line, 0-227.
    u16 dispcnt_, dispstat_, vcount_;
    // BGxCNT 0x008-0x00E: bits 0-1 priority, 2-3 char base (16 KiB units), 6 mosaic,
    // 7 256 colors, 8-12 screen base (2 KiB units), 13 affine wrap, 14-15 size.
    // BGxHOFS / BGxVOFS 0x010-0x01E: text BG scroll, 9 bits (0-511).
    u16 bgcnt_[4], bghofs_[4], bgvofs_[4];
    // BG2PA-PD / BG3PA-PD (0x020-0x026, 0x030-0x036): 8.8 fixed point; index 0 = BG2, 1 = BG3.
    s16 bgpa_[2], bgpb_[2], bgpc_[2], bgpd_[2];  // BG2 / BG3 affine matrix
    // BG2X/Y, BG3X/Y (0x028-0x02E, 0x038-0x03E): 20.8 fixed point, sign-extended from 28 bits.
    s32 bgx_[2], bgy_[2];                      // reference point registers
    // The hardware copies BGxX/Y into internal counters at V-Blank (and on a write) and adds
    // PB/PD to them after every line; refX_/refY_ are those internal counters.
    s32 refX_[2], refY_[2];                    // internal, advanced per line
    // WIN0H/WIN1H 0x040/0x042: X1 in bits 8-15, X2 (exclusive) in bits 0-7.
    // WIN0V/WIN1V 0x044/0x046: Y1 in bits 8-15, Y2 (exclusive) in bits 0-7.
    // WININ 0x048: bits 0-5 inside WIN0, bits 8-13 inside WIN1.
    // WINOUT 0x04A: bits 0-5 outside all windows, bits 8-13 inside the OBJ window.
    // MOSAIC 0x04C: bits 0-3 BG H size-1, 4-7 BG V size-1 (OBJ mosaic bits are not used here).
    u16 winh_[2], winv_[2], winin_, winout_, mosaic_;
    // BLDCNT 0x050: bits 0-5 1st target (BG0-3, OBJ, backdrop), 6-7 effect (0 none, 1 alpha,
    //   2 brighten, 3 darken), bits 8-13 2nd target.
    // BLDALPHA 0x052: EVA in bits 0-4, EVB in bits 8-12 (1/16 units, capped at 16).
    // BLDY 0x054: EVY in bits 0-4 for brighten/darken.
    u16 bldcnt_, bldalpha_, bldy_;

    // Position inside the current line in CPU cycles (0..1231) and whether H-Blank has begun.
    int lineCycle_;
    bool inHBlank_;

    // Scanline buffers
    // One entry per screen pixel of the line being drawn, cleared at the start of each line.
    // bgLine_ / objLine_ use the kOpaque | RGB555 encoding (0 = transparent).
    // objPriority_: priority (0-3) of the sprite pixel in objLine_.
    // objSemiTransparent_: that sprite pixel comes from a semi-transparent (mode 1) sprite.
    // objWindow_: an OBJ-window (mode 2) sprite covers this pixel.
    u16 bgLine_[4][kScreenWidth];
    u16 objLine_[kScreenWidth];
    u8 objPriority_[kScreenWidth];
    bool objSemiTransparent_[kScreenWidth];
    bool objWindow_[kScreenWidth];

    // Double buffer: lines are drawn into back_; at line 160 (V-Blank) the buffers are
    // swapped so front_ always holds a complete frame for the frontend.
    gb::FrameBuffer back_;
    gb::FrameBuffer front_;
};

}  // namespace gba

#endif
