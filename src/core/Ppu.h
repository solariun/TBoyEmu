// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Ppu.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Declares Ppu, the Game Boy / Game Boy Color Picture Processing Unit. It
//  owns VRAM (2 banks of 8 KiB), OAM (sprite attributes), the CGB palette
//  RAM and the LCD registers (0xFF40-0xFF4B, plus 0xFF4F and 0xFF68-0xFF6C
//  on CGB). It runs the LCD timing state machine (modes 2 -> 3 -> 0 per
//  line, mode 1 during V-Blank), raises the V-Blank and STAT interrupts,
//  and draws each visible scanline (background, window, sprites) into an
//  RGB555 FrameBuffer.
//
//  How it fits in
//  --------------
//  Owned by GameBoy. Bus::tick() calls tick(dots) every CPU M-cycle and
//  reacts to the returned events (H-Blank -> Hdma, frame -> GameBoy::
//  runFrame() returns). The Bus routes VRAM, OAM and LCD register accesses
//  here; OamDma/Hdma write through writeOamDirect()/writeVram(). The
//  frontend reads the finished picture through frame(). Implementation:
//  Ppu.cpp.
//
//  Hardware notes
//  --------------
//  Pan Docs, "Rendering Overview", "LCD Control", "LCD Status Registers",
//  "Scrolling", "Palettes", "VRAM Tile Data/Tile Maps", "OAM":
//    LCDC 0xFF40: bit 7 LCD on, 6 window map (0x9C00 if set), 5 window on,
//      4 BG/window tile data (1 = 0x8000 unsigned, 0 = 0x8800 signed),
//      3 BG map (0x9C00 if set), 2 sprite size 8x16, 1 sprites on,
//      0 BG/window on (DMG) or BG/window priority (CGB).
//    STAT 0xFF41: bit 6 LYC=LY int, 5 mode-2 int, 4 mode-1 int,
//      3 mode-0 int, 2 LYC=LY flag, bits 0-1 current mode.
//    SCY/SCX 0xFF42/43, LY 0xFF44, LYC 0xFF45, BGP/OBP0/OBP1 0xFF47-49,
//    WY/WX 0xFF4A/4B; CGB: VBK 0xFF4F, BCPS/BCPD 0xFF68/69,
//    OCPS/OCPD 0xFF6A/6B, OPRI 0xFF6C.
//  A line is 456 dots: mode 2 (OAM scan) 80 dots, mode 3 (drawing) fixed at
//  172 dots here (it varies on hardware), then mode 0 (H-Blank). Lines
//  144-153 are V-Blank (mode 1).
//
//  Contributing
//  ------------
//  The renderer draws a whole scanline at the start of mode 3, so
//  mid-scanline register changes are not visible (see README.md, "Not
//  supported"). VRAM/OAM are not blocked during modes 2/3. Test rendering
//  with dmg-acid2 and cgb-acid2:
//      ./tboyemu --headless --frames 600 --screenshot shot.png cgb-acid2.gbc
// ============================================================================
#ifndef TBOYEMU_CORE_PPU_H
#define TBOYEMU_CORE_PPU_H

#include "FrameBuffer.h"
#include "InterruptController.h"
#include "Types.h"

namespace gb {

// Picture Processing Unit: LCD timing state machine, STAT interrupts,
// VRAM/OAM, CGB palettes and a scanline renderer.
// Owned by GameBoy; holds a reference to the InterruptController.
// Single-threaded; the frontend only reads frame() between runFrame() calls.
class Ppu : private NonCopyable {
public:
    // Events reported by tick() so the bus can drive H-Blank DMA and the
    // machine can detect frame completion without callbacks.
    // tick() returns a bit mask of these values.
    enum Event { EventNone = 0, EventHBlank = 1, EventFrame = 2 };

    // cgbMode: enables the second VRAM bank, color palettes, BG attributes
    // and CGB sprite priority rules.
    Ppu(InterruptController& ic, bool cgbMode);

    // Post-boot state: LCD on (LCDC 0x91), white palettes, empty VRAM/OAM.
    void reset();
    // Advances the LCD by `dots` dots (4 per M-cycle, 2 in double speed).
    // Returns a mask of Event bits that happened during this call.
    u8 tick(int dots);

    // VRAM access (0x8000-0x9FFF) through the bank selected by VBK.
    u8 readVram(u16 addr) const { return vram_[vramBank_][addr & 0x1FFF]; }
    void writeVram(u16 addr, u8 v) { vram_[vramBank_][addr & 0x1FFF] = v; }
    // OAM access (0xFE00-0xFE9F); only the low byte of addr is used.
    u8 readOam(u16 addr) const { return oam_[addr & 0xFF]; }
    void writeOam(u16 addr, u8 v) { oam_[addr & 0xFF] = v; }
    // OAM write by index 0-159, used by OAM DMA.
    void writeOamDirect(u8 index, u8 v) { oam_[index] = v; }

    // LCD register access (see the register list in the file header).
    u8 readRegister(u16 addr) const;
    void writeRegister(u16 addr, u8 value);

    // LCDC bit 7.
    bool lcdEnabled() const { return (lcdc_ & 0x80) != 0; }
    // Current STAT mode: 0 H-Blank, 1 V-Blank, 2 OAM scan, 3 drawing.
    int mode() const { return mode_; }
    // Last completed frame (160x144 RGB555).
    const FrameBuffer& frame() const { return front_; }

private:
    // Changes the STAT mode.
    void setMode(int mode);
    // Recomputes the STAT interrupt line and requests irq::LcdStat on a
    // rising edge.
    void updateStatLine();
    // Draws line ly_ into back_.
    void renderScanline();
    // Draws background + window for line ly_. Fills, per pixel, the color
    // index 0-3 (for sprite priority) and the CGB BG-priority attribute bit.
    void renderBackground(u8* colorIndex, u8* bgPriority);
    // Draws the sprites of line ly_ on top of the background, honoring the
    // priority information computed by renderBackground().
    void renderSprites(const u8* colorIndex, const u8* bgPriority);
    // RGB555 color of color index 0-3 through a DMG palette register.
    u16 dmgColor(u8 palette, u8 index) const;
    // RGB555 color index 0-3 of CGB palette 0-7 in paletteRam (64 bytes).
    u16 cgbColor(const u8* paletteRam, int palette, int index) const;

    InterruptController& ic_;
    // Game Boy Color mode.
    bool cgb_;

    // VRAM: bank 0 = tile data + tile maps; bank 1 (CGB) = more tile data +
    // BG map attributes.
    u8 vram_[2][0x2000];
    u8 oam_[0xA0 + 0x60];  // padded so any 0xFExx index is valid
    // CGB palette RAM: 8 palettes x 4 colors x 2 bytes (little-endian RGB555).
    u8 bgPalette_[64];
    u8 objPalette_[64];

    // LCD registers (see the file header for addresses and bit layouts).
    u8 lcdc_, stat_, scy_, scx_, ly_, lyc_, bgp_, obp0_, obp1_, wy_, wx_;
    // CGB registers: VBK (0-1), BCPS, OCPS (bit 7 auto-increment, bits 0-5
    // index) and OPRI (0 = CGB OAM-order priority, 1 = DMG X priority).
    u8 vramBank_, bcps_, ocps_, opri_;

    // Current STAT mode (0-3).
    int mode_;
    // Dot position within the current line, 0-455.
    int lineDot_;
    int offDots_;  // frame pacing while the LCD is off
    // Window internal line counter: increments only on lines where the
    // window was actually drawn.
    int windowLine_;
    // Set once LY == WY during this frame; the window can then appear.
    bool windowYTriggered_;
    // Combined STAT interrupt line of the previous update (edge detection).
    bool statLine_;

    FrameBuffer back_;   // being drawn
    FrameBuffer front_;  // last completed frame
};

}  // namespace gb

#endif
