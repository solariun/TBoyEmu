#include "Ppu.h"

#include <algorithm>
#include <cstring>

namespace gb {

namespace {

enum Mode { ModeHBlank = 0, ModeVBlank = 1, ModeOamScan = 2, ModeTransfer = 3 };

const int kOamScanDots = 80;
const int kTransferEnd = 80 + 172;

inline u16 rgb555(int r, int g, int b) {
    return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

// Shades used when running original (non-color) Game Boy software.
const u16 kDmgShades[4] = {rgb555(0xE0, 0xF8, 0xD0), rgb555(0x88, 0xC0, 0x70),
                           rgb555(0x34, 0x68, 0x56), rgb555(0x08, 0x18, 0x20)};
const u16 kWhite = 0x7FFF;

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

    if (ly_ < kScreenHeight) {
        if (mode_ == ModeOamScan && lineDot_ >= kOamScanDots) {
            setMode(ModeTransfer);
            renderScanline();
        } else if (mode_ == ModeTransfer && lineDot_ >= kTransferEnd) {
            setMode(ModeHBlank);
            events |= EventHBlank;
        }
    }

    if (lineDot_ >= kDotsPerLine) {
        lineDot_ -= kDotsPerLine;
        ++ly_;
        if (ly_ == kScreenHeight) {
            setMode(ModeVBlank);
            ic_.request(irq::VBlank);
            std::swap(front_, back_);
            events |= EventFrame;
        } else if (ly_ >= kLinesPerFrame) {
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
    if (!cgb_) return 0xFF;
    switch (addr) {
        case 0xFF4F: return static_cast<u8>(0xFE | vramBank_);
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
        case 0xFF41:
            stat_ = static_cast<u8>((stat_ & 0x07) | (value & 0x78));
            updateStatLine();
            return;
        case 0xFF42: scy_ = value; return;
        case 0xFF43: scx_ = value; return;
        case 0xFF44: return;  // read only
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
        case 0xFF68: bcps_ = value & 0xBF; return;
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

u16 Ppu::dmgColor(u8 palette, u8 index) const {
    return kDmgShades[(palette >> (index * 2)) & 3];
}

u16 Ppu::cgbColor(const u8* paletteRam, int palette, int index) const {
    int offset = palette * 8 + index * 2;
    return static_cast<u16>((paletteRam[offset] | (paletteRam[offset + 1] << 8)) & 0x7FFF);
}

void Ppu::renderScanline() {
    u8 colorIndex[kScreenWidth];
    u8 bgPriority[kScreenWidth];
    renderBackground(colorIndex, bgPriority);
    if (lcdc_ & 0x02) renderSprites(colorIndex, bgPriority);
}

void Ppu::renderBackground(u8* colorIndex, u8* bgPriority) {
    u16* out = back_.row(ly_);

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

    bool windowOn = (lcdc_ & 0x20) && windowYTriggered_ && wx_ <= 166;
    int windowStartX = static_cast<int>(wx_) - 7;
    bool windowDrawn = false;
    bool signedTiles = !(lcdc_ & 0x10);

    for (int x = 0; x < kScreenWidth; ++x) {
        bool inWindow = windowOn && x >= windowStartX;
        u16 mapBase;
        unsigned px, py;
        if (inWindow) {
            mapBase = (lcdc_ & 0x40) ? 0x1C00 : 0x1800;
            px = static_cast<unsigned>(x - windowStartX);
            py = static_cast<unsigned>(windowLine_);
            windowDrawn = true;
        } else {
            mapBase = (lcdc_ & 0x08) ? 0x1C00 : 0x1800;
            px = (x + scx_) & 0xFF;
            py = (ly_ + scy_) & 0xFF;
        }

        u16 mapAddr = static_cast<u16>(mapBase + ((py >> 3) << 5) + (px >> 3));
        u8 tile = vram_[0][mapAddr];
        u8 attr = cgb_ ? vram_[1][mapAddr] : 0;

        unsigned row = py & 7;
        if (attr & 0x40) row = 7 - row;
        u16 tileAddr = signedTiles ? static_cast<u16>(0x1000 + static_cast<s8>(tile) * 16)
                                   : static_cast<u16>(tile * 16);
        const u8* bank = vram_[(attr & 0x08) ? 1 : 0];
        u8 lo = bank[tileAddr + row * 2];
        u8 hi = bank[tileAddr + row * 2 + 1];

        unsigned bit = px & 7;
        int shift = (attr & 0x20) ? static_cast<int>(bit) : 7 - static_cast<int>(bit);
        u8 color = static_cast<u8>((((hi >> shift) & 1) << 1) | ((lo >> shift) & 1));

        colorIndex[x] = color;
        bgPriority[x] = attr & 0x80;
        out[x] = cgb_ ? cgbColor(bgPalette_, attr & 0x07, color) : dmgColor(bgp_, color);
    }

    if (windowDrawn) ++windowLine_;
}

void Ppu::renderSprites(const u8* colorIndex, const u8* bgPriority) {
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
    if (!cgb_ || opri_) {
        std::stable_sort(sprites, sprites + count, [](const SpriteEntry& a, const SpriteEntry& b) {
            return a.x < b.x;
        });
    }

    bool taken[kScreenWidth];
    std::memset(taken, 0, sizeof(taken));
    const bool bgMasterPriority = !cgb_ || (lcdc_ & 0x01);

    for (int n = 0; n < count; ++n) {
        const SpriteEntry& s = sprites[n];
        int row = ly_ - s.y;
        if (s.flags & 0x40) row = height - 1 - row;
        u8 tile = (height == 16) ? (s.tile & 0xFE) : s.tile;
        const u8* bank = vram_[(cgb_ && (s.flags & 0x08)) ? 1 : 0];
        u16 addr = static_cast<u16>(tile * 16 + row * 2);
        u8 lo = bank[addr];
        u8 hi = bank[addr + 1];

        for (int col = 0; col < 8; ++col) {
            int x = s.x + col;
            if (x < 0 || x >= kScreenWidth || taken[x]) continue;
            int shift = (s.flags & 0x20) ? col : 7 - col;
            u8 color = static_cast<u8>((((hi >> shift) & 1) << 1) | ((lo >> shift) & 1));
            if (color == 0) continue;  // transparent
            taken[x] = true;           // hides lower priority sprites

            bool bgWins = false;
            if (bgMasterPriority && colorIndex[x] != 0) {
                bgWins = (s.flags & 0x80) || (cgb_ && bgPriority[x]);
            }
            if (bgWins) continue;

            if (cgb_) {
                out[x] = cgbColor(objPalette_, s.flags & 0x07, color);
            } else {
                out[x] = dmgColor((s.flags & 0x10) ? obp1_ : obp0_, color);
            }
        }
    }
}

}  // namespace gb
