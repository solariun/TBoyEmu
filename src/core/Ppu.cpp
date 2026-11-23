// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Ppu.cpp
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Implements the Ppu declared in Ppu.h:
//  - tick(): the LCD timing state machine (modes, LY, V-Blank, LCD off).
//  - updateStatLine(): STAT interrupt with rising-edge ("STAT blocking").
//  - readRegister()/writeRegister(): LCD and CGB palette registers.
//  - renderScanline(), renderBackground(), renderSprites(): a per-line
//    renderer that converts tiles, tile maps, attributes and palettes into
//    RGB555 pixels in the back buffer.
//
//  How it fits in
//  --------------
//  Bus::tick() calls tick() every M-cycle. When LY reaches 144 the back
//  buffer is swapped into front_ and EventFrame is returned; GameBoy then
//  returns from runFrame() and the frontend reads frame(). See Ppu.h.
//
//  Hardware notes
//  --------------
//  Pan Docs, "Rendering Overview", "STAT modes", "Tile Data", "Tile Maps",
//  "Object Attribute Memory", "Object priority", "Palettes", "Window":
//  - Tiles are 8x8, 16 bytes, 2 bits per pixel: byte 2*row holds the low
//    bits, byte 2*row+1 the high bits; bit 7 is the leftmost pixel.
//  - Tile maps are 32x32 entries at 0x9800 or 0x9C00. On CGB, VRAM bank 1
//    at the same address holds the attributes: bit 7 BG-over-OBJ priority,
//    6 vertical flip, 5 horizontal flip, 3 tile VRAM bank, 0-2 palette.
//  - OAM entry (4 bytes): Y+16, X+8, tile, flags. Flags: bit 7 BG over OBJ,
//    6 Y flip, 5 X flip, 4 DMG palette (OBP1 if set), 3 CGB VRAM bank,
//    0-2 CGB palette. At most 10 sprites per line, chosen in OAM order.
//  - The window is drawn from screen X = WX-7 once LY has matched WY, and
//    uses its own line counter.
//  - With the LCD off LY reads 0 and the screen is blank; frames are still
//    reported every 70224 dots so the frontend keeps its pace.
//
//  Contributing
//  ------------
//  Mode 3 has a fixed length here and the whole line is drawn at its start,
//  so effects that change registers in the middle of a line are not exact.
//  Test with dmg-acid2 and cgb-acid2 (README.md, "Testing"):
//      ./tboyemu --headless --frames 600 --screenshot shot.png dmg-acid2.gb
// ============================================================================
#include "Ppu.h"

#include <algorithm>
#include <cstring>

namespace gb {

namespace {

// STAT mode numbers (STAT bits 0-1).
enum Mode { ModeHBlank = 0, ModeVBlank = 1, ModeOamScan = 2, ModeTransfer = 3 };

// Line timing in dots: mode 2 lasts 80 dots; mode 3 ends 172 dots later
// (the minimum mode 3 length on hardware); H-Blank lasts until dot 456.
const int kOamScanDots = 80;
const int kTransferEnd = 80 + 172;

// Packs 8-bit-per-channel RGB into RGB555 (red in the low bits).
inline u16 rgb555(int r, int g, int b) {
    return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

// Shades used when running original (non-color) Game Boy software.
// Index 0 is the lightest (a pale green) and 3 the darkest, approximating
// the greenish look of the original DMG screen.
const u16 kDmgShades[4] = {rgb555(0xE0, 0xF8, 0xD0), rgb555(0x88, 0xC0, 0x70),
                           rgb555(0x34, 0x68, 0x56), rgb555(0x08, 0x18, 0x20)};
const u16 kWhite = 0x7FFF;

// A sprite selected for the current line, decoded from its OAM entry.
// x/y are screen coordinates (OAM X-8 / Y-16); oamIndex is 0-39.
struct SpriteEntry {
    int x;
    int y;
    u8 tile;
    u8 flags;
    int oamIndex;
};

}  // namespace

Ppu::Ppu(InterruptController& ic, bool cgbMode)
    : ic_(ic), cgb_(cgbMode), back_(kScreenWidth, kScreenHeight), front_(kScreenWidth, kScreenHeight) {
    reset();
}

// Register values the boot ROM leaves (LCDC 0x91: LCD on, BG on, tile data
// at 0x8000; BGP 0xFC). OPRI selects DMG-style sprite priority in DMG mode.
void Ppu::reset() {
    std::memset(vram_, 0, sizeof(vram_));
    std::memset(oam_, 0, sizeof(oam_));
    std::memset(bgPalette_, 0xFF, sizeof(bgPalette_));  // boot ROM leaves white
    std::memset(objPalette_, 0xFF, sizeof(objPalette_));
    lcdc_ = 0x91;
    stat_ = 0x00;
    scy_ = scx_ = 0;
    ly_ = 0;
    lyc_ = 0;
    bgp_ = 0xFC;
    obp0_ = obp1_ = 0xFF;
    wy_ = wx_ = 0;
    vramBank_ = 0;
    bcps_ = ocps_ = 0;
    opri_ = cgb_ ? 0 : 1;
    mode_ = ModeOamScan;
    lineDot_ = 0;
    offDots_ = 0;
    windowLine_ = 0;
    windowYTriggered_ = false;
    statLine_ = false;
    back_.fill(kWhite);
    front_.fill(kWhite);
}

void Ppu::setMode(int mode) {
    mode_ = mode;
}

// The four STAT interrupt sources are ORed into one internal line.
// STAT bits: 6 = LY==LYC, 3 = mode 0, 4 = mode 1, 5 = mode 2.
void Ppu::updateStatLine() {
    bool line = false;
    if (lcdEnabled()) {
        if ((stat_ & 0x40) && ly_ == lyc_) line = true;
        if ((stat_ & 0x08) && mode_ == ModeHBlank) line = true;
        if ((stat_ & 0x10) && mode_ == ModeVBlank) line = true;
        if ((stat_ & 0x20) && mode_ == ModeOamScan) line = true;
    }
    // The STAT interrupt fires only on a rising edge of the combined line
    // ("STAT blocking").
    if (line && !statLine_) ic_.request(irq::LcdStat);
    statLine_ = line;
}

u8 Ppu::tick(int dots) {
    u8 events = EventNone;

    // LCD off: nothing is drawn, but a (white) frame is still reported
    // every 70224 dots so the emulator keeps running at the normal pace.
    if (!lcdEnabled()) {
        offDots_ += dots;
        if (offDots_ >= kDotsPerFrame) {
            offDots_ -= kDotsPerFrame;
            front_.fill(kWhite);
            events |= EventFrame;
        }
        return events;
    }

    lineDot_ += dots;

    // Visible lines: mode 2 -> mode 3 at dot 80 (the line is rendered right
    // away), mode 3 -> mode 0 at dot 252 (reported for H-Blank DMA).
    if (ly_ < kScreenHeight) {
        if (mode_ == ModeOamScan && lineDot_ >= kOamScanDots) {
            setMode(ModeTransfer);
            renderScanline();
        } else if (mode_ == ModeTransfer && lineDot_ >= kTransferEnd) {
            setMode(ModeHBlank);
            events |= EventHBlank;
        }
    }

    // End of line: advance LY.
    if (lineDot_ >= kDotsPerLine) {
        lineDot_ -= kDotsPerLine;
        ++ly_;
        if (ly_ == kScreenHeight) {
            // Line 144: V-Blank starts; publish the finished frame.
            setMode(ModeVBlank);
            ic_.request(irq::VBlank);
            std::swap(front_, back_);
            events |= EventFrame;
        } else if (ly_ >= kLinesPerFrame) {
            // After line 153: new frame, reset the window state.
            ly_ = 0;
            windowLine_ = 0;
            windowYTriggered_ = false;
            setMode(ModeOamScan);
        } else if (ly_ < kScreenHeight) {
            setMode(ModeOamScan);
        }
    }

    updateStatLine();
    return events;
}

// ----------------------------------------------------------- registers ----

u8 Ppu::readRegister(u16 addr) const {
    switch (addr) {
        case 0xFF40: return lcdc_;
        // STAT: bit 7 reads 1, bits 3-6 as written; the coincidence flag
        // (bit 2) and mode (bits 0-1) read 0 while the LCD is off.
        case 0xFF41: {
            u8 v = static_cast<u8>(0x80 | (stat_ & 0x78));
            if (lcdEnabled()) {
                if (ly_ == lyc_) v |= 0x04;
                v |= static_cast<u8>(mode_);
            }
            return v;
        }
        case 0xFF42: return scy_;
        case 0xFF43: return scx_;
        case 0xFF44: return lcdEnabled() ? ly_ : 0;
        case 0xFF45: return lyc_;
        case 0xFF47: return bgp_;
        case 0xFF48: return obp0_;
        case 0xFF49: return obp1_;
        case 0xFF4A: return wy_;
        case 0xFF4B: return wx_;
        default: break;
    }
    // CGB-only registers read 0xFF in DMG mode.
    if (!cgb_) return 0xFF;
    switch (addr) {
        // VBK: bit 0 bank, other bits read 1.
        case 0xFF4F: return static_cast<u8>(0xFE | vramBank_);
        // BCPS/OCPS: bit 6 unused (reads 1). BCPD/OCPD: byte at the index.
        case 0xFF68: return static_cast<u8>(bcps_ | 0x40);
        case 0xFF69: return bgPalette_[bcps_ & 0x3F];
        case 0xFF6A: return static_cast<u8>(ocps_ | 0x40);
        case 0xFF6B: return objPalette_[ocps_ & 0x3F];
        case 0xFF6C: return static_cast<u8>(0xFE | opri_);
        default: return 0xFF;
    }
}

void Ppu::writeRegister(u16 addr, u8 value) {
    switch (addr) {
        // LCDC: turning the LCD off resets LY and the line timer; turning it
        // on restarts from line 0 in mode 2.
        case 0xFF40: {
            bool wasOn = lcdEnabled();
            lcdc_ = value;
            if (wasOn && !lcdEnabled()) {
                ly_ = 0;
                lineDot_ = 0;
                offDots_ = 0;
                mode_ = ModeHBlank;
                statLine_ = false;
            } else if (!wasOn && lcdEnabled()) {
                ly_ = 0;
                lineDot_ = 0;
                windowLine_ = 0;
                windowYTriggered_ = false;
                mode_ = ModeOamScan;
                updateStatLine();
            }
            return;
        }
        // STAT: only the interrupt enable bits 3-6 are writable.
        case 0xFF41:
            stat_ = static_cast<u8>((stat_ & 0x07) | (value & 0x78));
            updateStatLine();
            return;
        case 0xFF42: scy_ = value; return;
        case 0xFF43: scx_ = value; return;
        case 0xFF44: return;  // read only
        // LYC: a new compare value may raise the STAT line immediately.
        case 0xFF45:
            lyc_ = value;
            updateStatLine();
            return;
        case 0xFF47: bgp_ = value; return;
        case 0xFF48: obp0_ = value; return;
        case 0xFF49: obp1_ = value; return;
        case 0xFF4A: wy_ = value; return;
        case 0xFF4B: wx_ = value; return;
        default: break;
    }
    if (!cgb_) return;
    switch (addr) {
        case 0xFF4F: vramBank_ = value & 1; return;
        // BCPS/OCPS: bit 7 auto-increment, bits 0-5 byte index (bit 6 dropped).
        case 0xFF68: bcps_ = value & 0xBF; return;
        // BCPD/OCPD: write the byte at the index, then auto-increment the
        // index (wrapping within 0-63) if bit 7 of BCPS/OCPS is set.
        case 0xFF69:
            bgPalette_[bcps_ & 0x3F] = value;
            if (bcps_ & 0x80) bcps_ = static_cast<u8>(0x80 | ((bcps_ + 1) & 0x3F));
            return;
        case 0xFF6A: ocps_ = value & 0xBF; return;
        case 0xFF6B:
            objPalette_[ocps_ & 0x3F] = value;
            if (ocps_ & 0x80) ocps_ = static_cast<u8>(0x80 | ((ocps_ + 1) & 0x3F));
            return;
        case 0xFF6C: opri_ = value & 1; return;
        default: return;
    }
}

// ------------------------------------------------------------ renderer ----

// DMG palette register: bits 1-0 give the shade of color 0, bits 3-2 of
// color 1, bits 5-4 of color 2 and bits 7-6 of color 3.
u16 Ppu::dmgColor(u8 palette, u8 index) const {
    return kDmgShades[(palette >> (index * 2)) & 3];
}

// CGB palette RAM: palette p, color i is the little-endian 16-bit word at
// byte p * 8 + i * 2; bit 15 is ignored.
u16 Ppu::cgbColor(const u8* paletteRam, int palette, int index) const {
    int offset = palette * 8 + index * 2;
    return static_cast<u16>((paletteRam[offset] | (paletteRam[offset + 1] << 8)) & 0x7FFF);
}

// Per-pixel scratch buffers shared between the background and sprite passes.
void Ppu::renderScanline() {
    u8 colorIndex[kScreenWidth];
    u8 bgPriority[kScreenWidth];
    renderBackground(colorIndex, bgPriority);
    // LCDC bit 1: sprites enabled.
    if (lcdc_ & 0x02) renderSprites(colorIndex, bgPriority);
}

void Ppu::renderBackground(u8* colorIndex, u8* bgPriority) {
    u16* out = back_.row(ly_);

    // The window becomes possible for the rest of the frame once LY == WY.
    if (ly_ == wy_) windowYTriggered_ = true;

    // On DMG, LCDC bit 0 blanks BG and window.  On CGB it only removes their
    // priority over sprites.
    if (!cgb_ && !(lcdc_ & 0x01)) {
        for (int x = 0; x < kScreenWidth; ++x) {
            out[x] = kDmgShades[0];
            colorIndex[x] = 0;
            bgPriority[x] = 0;
        }
        return;
    }

    // Window: LCDC bit 5 enables it; it starts at screen X = WX - 7 and is
    // off-screen for WX > 166.
    bool windowOn = (lcdc_ & 0x20) && windowYTriggered_ && wx_ <= 166;
    int windowStartX = static_cast<int>(wx_) - 7;
    bool windowDrawn = false;
    // LCDC bit 4 clear: "0x8800 method", signed tile IDs relative to 0x9000.
    bool signedTiles = !(lcdc_ & 0x10);

    for (int x = 0; x < kScreenWidth; ++x) {
        // Pick the tile map and the (px, py) position inside the 256x256
        // background or the window.
        bool inWindow = windowOn && x >= windowStartX;
        u16 mapBase;
        unsigned px, py;
        if (inWindow) {
            // Window map: LCDC bit 6 (0x9C00 or 0x9800); window coordinates
            // come from the window's own line counter.
            mapBase = (lcdc_ & 0x40) ? 0x1C00 : 0x1800;
            px = static_cast<unsigned>(x - windowStartX);
            py = static_cast<unsigned>(windowLine_);
            windowDrawn = true;
        } else {
            // Background map: LCDC bit 3; scrolled by SCX/SCY, wrapping at 256.
            mapBase = (lcdc_ & 0x08) ? 0x1C00 : 0x1800;
            px = (x + scx_) & 0xFF;
            py = (ly_ + scy_) & 0xFF;
        }

        // Tile map entry (32 tiles per row) and, on CGB, its attribute byte
        // from VRAM bank 1.
        u16 mapAddr = static_cast<u16>(mapBase + ((py >> 3) << 5) + (px >> 3));
        u8 tile = vram_[0][mapAddr];
        u8 attr = cgb_ ? vram_[1][mapAddr] : 0;

        // Tile row (attribute bit 6 = vertical flip) and its two bitplanes,
        // read from the VRAM bank chosen by attribute bit 3.
        unsigned row = py & 7;
        if (attr & 0x40) row = 7 - row;
        u16 tileAddr = signedTiles ? static_cast<u16>(0x1000 + static_cast<s8>(tile) * 16)
                                   : static_cast<u16>(tile * 16);
        const u8* bank = vram_[(attr & 0x08) ? 1 : 0];
        u8 lo = bank[tileAddr + row * 2];
        u8 hi = bank[tileAddr + row * 2 + 1];

        // Pixel within the tile (attribute bit 5 = horizontal flip); bit 7 is
        // the leftmost pixel. color = 2-bit index (high plane, low plane).
        unsigned bit = px & 7;
        int shift = (attr & 0x20) ? static_cast<int>(bit) : 7 - static_cast<int>(bit);
        u8 color = static_cast<u8>((((hi >> shift) & 1) << 1) | ((lo >> shift) & 1));

        colorIndex[x] = color;
        bgPriority[x] = attr & 0x80;
        out[x] = cgb_ ? cgbColor(bgPalette_, attr & 0x07, color) : dmgColor(bgp_, color);
    }

    // The window line counter only advances on lines that showed the window.
    if (windowDrawn) ++windowLine_;
}

void Ppu::renderSprites(const u8* colorIndex, const u8* bgPriority) {
    // LCDC bit 2: 8x16 sprites.
    const int height = (lcdc_ & 0x04) ? 16 : 8;
    u16* out = back_.row(ly_);

    // OAM scan: first 10 sprites overlapping this line, in OAM order.
    SpriteEntry sprites[10];
    int count = 0;
    for (int i = 0; i < 40 && count < 10; ++i) {
        int y = static_cast<int>(oam_[i * 4]) - 16;
        if (ly_ >= y && ly_ < y + height) {
            SpriteEntry& s = sprites[count++];
            s.y = y;
            s.x = static_cast<int>(oam_[i * 4 + 1]) - 8;
            s.tile = oam_[i * 4 + 2];
            s.flags = oam_[i * 4 + 3];
            s.oamIndex = i;
        }
    }

    // Priority order: CGB uses OAM index; DMG (or OPRI=1) uses X then index.
    // stable_sort keeps OAM order among sprites with the same X.
    if (!cgb_ || opri_) {
        std::stable_sort(sprites, sprites + count, [](const SpriteEntry& a, const SpriteEntry& b) {
            return a.x < b.x;
        });
    }

    // Sprites are drawn highest priority first; taken[x] marks pixels
    // already claimed by an opaque sprite pixel.
    bool taken[kScreenWidth];
    std::memset(taken, 0, sizeof(taken));
    // On CGB, LCDC bit 0 = 0 makes sprites always win over BG and window.
    const bool bgMasterPriority = !cgb_ || (lcdc_ & 0x01);

    for (int n = 0; n < count; ++n) {
        const SpriteEntry& s = sprites[n];
        // Row inside the sprite (flag bit 6 = vertical flip). In 8x16 mode
        // the tile number's bit 0 is ignored (top tile even, bottom odd).
        int row = ly_ - s.y;
        if (s.flags & 0x40) row = height - 1 - row;
        u8 tile = (height == 16) ? (s.tile & 0xFE) : s.tile;
        // Sprite tiles always use the 0x8000 addressing; flag bit 3 selects
        // the VRAM bank on CGB.
        const u8* bank = vram_[(cgb_ && (s.flags & 0x08)) ? 1 : 0];
        u16 addr = static_cast<u16>(tile * 16 + row * 2);
        u8 lo = bank[addr];
        u8 hi = bank[addr + 1];

        for (int col = 0; col < 8; ++col) {
            int x = s.x + col;
            if (x < 0 || x >= kScreenWidth || taken[x]) continue;
            // Flag bit 5 = horizontal flip.
            int shift = (s.flags & 0x20) ? col : 7 - col;
            u8 color = static_cast<u8>((((hi >> shift) & 1) << 1) | ((lo >> shift) & 1));
            if (color == 0) continue;  // transparent
            taken[x] = true;           // hides lower priority sprites

            // The background wins over this sprite pixel when its color index
            // is 1-3 and either the sprite's flag bit 7 or (CGB) the tile's
            // attribute bit 7 asks for it. BG color 0 never hides sprites.
            bool bgWins = false;
            if (bgMasterPriority && colorIndex[x] != 0) {
                bgWins = (s.flags & 0x80) || (cgb_ && bgPriority[x]);
            }
            if (bgWins) continue;

            // CGB: palette 0-7 from flag bits 0-2. DMG: OBP0 or OBP1 (flag bit 4).
            if (cgb_) {
                out[x] = cgbColor(objPalette_, s.flags & 0x07, color);
            } else {
                out[x] = dmgColor((s.flags & 0x10) ? obp1_ : obp0_, color);
            }
        }
    }
}

}  // namespace gb
