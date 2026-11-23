#include "Ppu.h"

#include <algorithm>
#include <cstring>

namespace gba {

namespace {

// Sprite sizes [shape][size] in pixels (width, height).
const int kObjWidth[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
const int kObjHeight[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

// Layer ids used by BLDCNT / window masks.
const int kLayerObj = 4;
const int kLayerBackdrop = 5;

s32 signExtend28(u32 v) { return static_cast<s32>(v << 4) >> 4; }

bool insideRange(int v, int start, int end, int limit) {
    if (end > limit || start > end) {
        // Out-of-range end or wrap-around window.
        if (start > end) return v >= start || v < end;
        end = limit;
    }
    return v >= start && v < end;
}

u16 blendAlpha(u16 a, u16 b, int eva, int evb) {
    int r = std::min(31, ((a & 31) * eva + (b & 31) * evb) >> 4);
    int g = std::min(31, (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4);
    int bl = std::min(31, (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4);
    return static_cast<u16>(r | (g << 5) | (bl << 10));
}

u16 brighten(u16 c, int evy) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return static_cast<u16>(r | (g << 5) | (b << 10));
}

u16 darken(u16 c, int evy) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return static_cast<u16>(r | (g << 5) | (b << 10));
}

}  // namespace

Ppu::Ppu(Interrupts& interrupts)
    : interrupts_(interrupts), back_(kScreenWidth, kScreenHeight), front_(kScreenWidth, kScreenHeight) {
    reset();
}

void Ppu::reset() {
    std::memset(palette_, 0, sizeof(palette_));
    std::memset(vram_, 0, sizeof(vram_));
    std::memset(oam_, 0, sizeof(oam_));
    dispcnt_ = 0x0080;
    dispstat_ = 0;
    vcount_ = 0;
    for (int i = 0; i < 4; ++i) bgcnt_[i] = bghofs_[i] = bgvofs_[i] = 0;
    for (int i = 0; i < 2; ++i) {
        bgpa_[i] = 0x100;
        bgpb_[i] = 0;
        bgpc_[i] = 0;
        bgpd_[i] = 0x100;
        bgx_[i] = bgy_[i] = refX_[i] = refY_[i] = 0;
        winh_[i] = winv_[i] = 0;
    }
    winin_ = winout_ = mosaic_ = 0;
    bldcnt_ = bldalpha_ = bldy_ = 0;
    lineCycle_ = 0;
    inHBlank_ = false;
    back_.fill(0x7FFF);
    front_.fill(0x7FFF);
}

// --------------------------------------------------------------- timing ----

int Ppu::cyclesUntilEvent() const {
    return inHBlank_ ? kCyclesPerLine - lineCycle_ : kHDrawCycles - lineCycle_;
}

u8 Ppu::tick(int cycles) {
    u8 events = 0;
    lineCycle_ += cycles;
    for (;;) {
        if (!inHBlank_) {
            if (lineCycle_ < kHDrawCycles) break;
            inHBlank_ = true;
            dispstat_ |= 0x0002;
            if (vcount_ < kVisibleLines) {
                renderLine();
                events |= EventHBlank;
            }
            if (dispstat_ & 0x0010) interrupts_.request(irq::HBlank);
        } else {
            if (lineCycle_ < kCyclesPerLine) break;
            lineCycle_ -= kCyclesPerLine;
            inHBlank_ = false;
            dispstat_ &= static_cast<u16>(~0x0002);
            ++vcount_;
            if (vcount_ == kVisibleLines) {
                dispstat_ |= 0x0001;
                if (dispstat_ & 0x0008) interrupts_.request(irq::VBlank);
                std::swap(front_, back_);
                // Affine reference points are reloaded every frame.
                for (int i = 0; i < 2; ++i) {
                    refX_[i] = bgx_[i];
                    refY_[i] = bgy_[i];
                }
                events |= EventVBlank | EventFrame;
            } else if (vcount_ == kLinesPerFrame - 1) {
                dispstat_ &= static_cast<u16>(~0x0001);
            } else if (vcount_ >= kLinesPerFrame) {
                vcount_ = 0;
            }
            if (vcount_ == (dispstat_ >> 8)) {
                dispstat_ |= 0x0004;
                if (dispstat_ & 0x0020) interrupts_.request(irq::VCount);
            } else {
                dispstat_ &= static_cast<u16>(~0x0004);
            }
        }
    }
    return events;
}

// ------------------------------------------------------------ registers ----

u16 Ppu::readIo(u32 offset) const {
    switch (offset) {
        case 0x00: return dispcnt_;
        case 0x04: return dispstat_;
        case 0x06: return vcount_;
        case 0x08: case 0x0A: case 0x0C: case 0x0E: return bgcnt_[(offset - 0x08) / 2];
        case 0x48: return winin_ & 0x3F3F;
        case 0x4A: return winout_ & 0x3F3F;
        case 0x50: return bldcnt_ & 0x3FFF;
        case 0x52: return bldalpha_ & 0x1F1F;
        default: return 0;  // write-only registers
    }
}

void Ppu::writeIo(u32 offset, u16 v) {
    if (offset >= 0x10 && offset < 0x20) {
        int bg = static_cast<int>((offset - 0x10) / 4);
        if (offset & 2) {
            bgvofs_[bg] = v & 0x1FF;
        } else {
            bghofs_[bg] = v & 0x1FF;
        }
        return;
    }
    if (offset >= 0x20 && offset < 0x40) {
        int i = offset >= 0x30 ? 1 : 0;
        u32 r = offset - (i ? 0x30 : 0x20);
        switch (r) {
            case 0x0: bgpa_[i] = static_cast<s16>(v); break;
            case 0x2: bgpb_[i] = static_cast<s16>(v); break;
            case 0x4: bgpc_[i] = static_cast<s16>(v); break;
            case 0x6: bgpd_[i] = static_cast<s16>(v); break;
            case 0x8: bgx_[i] = signExtend28((static_cast<u32>(bgx_[i]) & 0xFFFF0000u) | v); refX_[i] = bgx_[i]; break;
            case 0xA: bgx_[i] = signExtend28((static_cast<u32>(bgx_[i]) & 0xFFFFu) | (static_cast<u32>(v) << 16)); refX_[i] = bgx_[i]; break;
            case 0xC: bgy_[i] = signExtend28((static_cast<u32>(bgy_[i]) & 0xFFFF0000u) | v); refY_[i] = bgy_[i]; break;
            case 0xE: bgy_[i] = signExtend28((static_cast<u32>(bgy_[i]) & 0xFFFFu) | (static_cast<u32>(v) << 16)); refY_[i] = bgy_[i]; break;
        }
        return;
    }
    switch (offset) {
        case 0x00: dispcnt_ = v; break;
        case 0x04: dispstat_ = static_cast<u16>((dispstat_ & 0x0007) | (v & 0xFF38)); break;
        case 0x08: case 0x0A: case 0x0C: case 0x0E: bgcnt_[(offset - 0x08) / 2] = v; break;
        case 0x40: winh_[0] = v; break;
        case 0x42: winh_[1] = v; break;
        case 0x44: winv_[0] = v; break;
        case 0x46: winv_[1] = v; break;
        case 0x48: winin_ = v; break;
        case 0x4A: winout_ = v; break;
        case 0x4C: mosaic_ = v; break;
        case 0x50: bldcnt_ = v; break;
        case 0x52: bldalpha_ = v; break;
        case 0x54: bldy_ = v; break;
        default: break;
    }
}

// ------------------------------------------------------------- renderer ----

void Ppu::renderLine() {
    u16* out = back_.row(vcount_);
    if (dispcnt_ & 0x0080) {  // forced blank
        for (int x = 0; x < kScreenWidth; ++x) out[x] = 0x7FFF;
        return;
    }
    for (int bg = 0; bg < 4; ++bg) std::memset(bgLine_[bg], 0, sizeof(bgLine_[bg]));

    const int mode = dispcnt_ & 7;
    switch (mode) {
        case 0:
            for (int bg = 0; bg < 4; ++bg) {
                if (dispcnt_ & (0x100 << bg)) renderTextBg(bg);
            }
            break;
        case 1:
            if (dispcnt_ & 0x100) renderTextBg(0);
            if (dispcnt_ & 0x200) renderTextBg(1);
            if (dispcnt_ & 0x400) renderAffineBg(2);
            break;
        case 2:
            if (dispcnt_ & 0x400) renderAffineBg(2);
            if (dispcnt_ & 0x800) renderAffineBg(3);
            break;
        case 3:
        case 4:
        case 5:
            if (dispcnt_ & 0x400) renderBitmapBg(mode);
            break;
        default:
            break;
    }

    // Affine reference points advance once per scanline.
    for (int i = 0; i < 2; ++i) {
        refX_[i] += bgpb_[i];
        refY_[i] += bgpd_[i];
    }

    std::memset(objLine_, 0, sizeof(objLine_));
    std::memset(objWindow_, 0, sizeof(objWindow_));
    std::memset(objSemiTransparent_, 0, sizeof(objSemiTransparent_));
    if (dispcnt_ & 0x1000) renderSprites();

    compose();
}

void Ppu::renderTextBg(int bg) {
    const u16 cnt = bgcnt_[bg];
    const u32 charBase = ((cnt >> 2) & 3) * 0x4000u;
    const u32 screenBase = ((cnt >> 8) & 0x1F) * 0x800u;
    const bool color256 = (cnt & 0x80) != 0;
    const int size = cnt >> 14;
    const int width = (size & 1) ? 512 : 256;
    const int height = (size & 2) ? 512 : 256;
    const bool mosaic = (cnt & 0x40) != 0;
    const int mosaicH = (mosaic_ & 0xF) + 1;
    const int mosaicV = ((mosaic_ >> 4) & 0xF) + 1;

    int line = vcount_;
    if (mosaic) line -= line % mosaicV;
    const int y = (line + bgvofs_[bg]) & (height - 1);
    u16* out = bgLine_[bg];

    for (int x = 0; x < kScreenWidth; ++x) {
        int srcX = mosaic ? x - x % mosaicH : x;
        int sx = (srcX + bghofs_[bg]) & (width - 1);
        int block = 0;
        if (size == 1) block = sx >> 8;
        else if (size == 2) block = y >> 8;
        else if (size == 3) block = (sx >> 8) + ((y >> 8) << 1);
        u32 entryAddr = screenBase + static_cast<u32>(block) * 0x800u + static_cast<u32>(((y & 255) >> 3) * 64 + ((sx & 255) >> 3) * 2);
        if (entryAddr >= 0x10000) continue;
        u16 entry = vram16(entryAddr);
        u32 tile = entry & 0x3FF;
        int px = sx & 7, py = y & 7;
        if (entry & 0x400) px = 7 - px;
        if (entry & 0x800) py = 7 - py;
        u32 index;
        if (color256) {
            u32 addr = charBase + tile * 64 + static_cast<u32>(py * 8 + px);
            if (addr >= 0x10000) continue;
            index = vram_[addr];
        } else {
            u32 addr = charBase + tile * 32 + static_cast<u32>(py * 4 + (px >> 1));
            if (addr >= 0x10000) continue;
            index = (vram_[addr] >> ((px & 1) * 4)) & 0xF;
            if (index) index += (entry >> 12) * 16u;
        }
        if (index) out[x] = static_cast<u16>(palette16(index) | kOpaque);
    }
}

void Ppu::renderAffineBg(int bg) {
    const int i = bg - 2;
    const u16 cnt = bgcnt_[bg];
    const u32 charBase = ((cnt >> 2) & 3) * 0x4000u;
    const u32 screenBase = ((cnt >> 8) & 0x1F) * 0x800u;
    const int size = 128 << (cnt >> 14);
    const bool wrap = (cnt & 0x2000) != 0;
    const int tilesPerRow = size / 8;
    u16* out = bgLine_[bg];

    s32 fx = refX_[i];
    s32 fy = refY_[i];
    for (int x = 0; x < kScreenWidth; ++x, fx += bgpa_[i], fy += bgpc_[i]) {
        int tx = fx >> 8;
        int ty = fy >> 8;
        if (wrap) {
            tx &= size - 1;
            ty &= size - 1;
        } else if (tx < 0 || ty < 0 || tx >= size || ty >= size) {
            continue;
        }
        u32 mapAddr = screenBase + static_cast<u32>((ty >> 3) * tilesPerRow + (tx >> 3));
        if (mapAddr >= 0x10000) continue;
        u32 tile = vram_[mapAddr];
        u32 addr = charBase + tile * 64 + static_cast<u32>((ty & 7) * 8 + (tx & 7));
        if (addr >= 0x10000) continue;
        u8 index = vram_[addr];
        if (index) out[x] = static_cast<u16>(palette16(index) | kOpaque);
    }
}

void Ppu::renderBitmapBg(int mode) {
    const int width = mode == 5 ? 160 : 240;
    const int height = mode == 5 ? 128 : 160;
    const u32 page = (mode != 3 && (dispcnt_ & 0x10)) ? 0xA000 : 0;
    u16* out = bgLine_[2];

    s32 fx = refX_[0];
    s32 fy = refY_[0];
    for (int x = 0; x < kScreenWidth; ++x, fx += bgpa_[0], fy += bgpc_[0]) {
        int tx = fx >> 8;
        int ty = fy >> 8;
        if (tx < 0 || ty < 0 || tx >= width || ty >= height) continue;
        if (mode == 4) {
            u8 index = vram_[page + static_cast<u32>(ty * width + tx)];
            if (index) out[x] = static_cast<u16>(palette16(index) | kOpaque);
        } else {
            u16 color = vram16(page + static_cast<u32>(ty * width + tx) * 2);
            out[x] = static_cast<u16>((color & 0x7FFF) | kOpaque);
        }
    }
}

void Ppu::renderSprites() {
    const bool oneDimensional = (dispcnt_ & 0x40) != 0;
    const bool bitmap = bitmapMode();
    const int line = vcount_;

    for (int i = 0; i < 128; ++i) {
        u16 a0 = oam16(static_cast<u32>(i * 4));
        u16 a1 = oam16(static_cast<u32>(i * 4 + 1));
        u16 a2 = oam16(static_cast<u32>(i * 4 + 2));
        const bool affine = (a0 & 0x100) != 0;
        if (!affine && (a0 & 0x200)) continue;  // disabled
        const int objMode = (a0 >> 10) & 3;
        if (objMode == 3) continue;
        const int shape = a0 >> 14;
        if (shape == 3) continue;
        const int w = kObjWidth[shape][a1 >> 14];
        const int h = kObjHeight[shape][a1 >> 14];
        const bool doubleSize = affine && (a0 & 0x200);
        const int boundW = doubleSize ? w * 2 : w;
        const int boundH = doubleSize ? h * 2 : h;

        int y = a0 & 0xFF;
        if (y >= kScreenHeight) y -= 256;
        int x = a1 & 0x1FF;
        if (x >= kScreenWidth) x -= 512;
        if (line < y || line >= y + boundH) continue;

        const u32 tileBase = a2 & 0x3FF;
        if (bitmap && tileBase < 512) continue;  // tiles overlapped by the bitmap
        const int priority = (a2 >> 10) & 3;
        const bool color256 = (a0 & 0x2000) != 0;
        const u32 palBank = (a2 >> 12) * 16u;

        s16 pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            u32 p = ((a1 >> 9) & 0x1F) * 16u;
            pa = static_cast<s16>(oam16(p + 3));
            pb = static_cast<s16>(oam16(p + 7));
            pc = static_cast<s16>(oam16(p + 11));
            pd = static_cast<s16>(oam16(p + 15));
        }
        const int dy = line - y;

        for (int px = 0; px < boundW; ++px) {
            int sx = x + px;
            if (sx < 0 || sx >= kScreenWidth) continue;
            int tx, ty;
            if (affine) {
                int cx = px - boundW / 2;
                int cy = dy - boundH / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - px : px;
                ty = (a1 & 0x2000) ? h - 1 - dy : dy;
            }
            u32 tileX = static_cast<u32>(tx >> 3);
            u32 tileY = static_cast<u32>(ty >> 3);
            u32 tileNum;
            if (color256) {
                tileNum = tileBase + (oneDimensional ? tileY * static_cast<u32>(w / 8) * 2 : tileY * 32) + tileX * 2;
            } else {
                tileNum = tileBase + (oneDimensional ? tileY * static_cast<u32>(w / 8) : tileY * 32) + tileX;
            }
            u32 tileAddr = 0x10000 + (tileNum & 0x3FF) * 32;
            u32 index;
            if (color256) {
                index = vram_[tileAddr + static_cast<u32>((ty & 7) * 8 + (tx & 7))];
                if (index) index += 256;
            } else {
                u8 b = vram_[tileAddr + static_cast<u32>((ty & 7) * 4 + ((tx & 7) >> 1))];
                index = (b >> ((tx & 1) * 4)) & 0xF;
                if (index) index += 256 + palBank;
            }
            if (!index) continue;
            if (objMode == 2) {
                objWindow_[sx] = true;
                continue;
            }
            // Lower OAM index wins between sprites; first one drawn stays.
            if (objLine_[sx] & kOpaque) {
                if (objPriority_[sx] <= priority) continue;
            }
            objLine_[sx] = static_cast<u16>(palette16(index) | kOpaque);
            objPriority_[sx] = static_cast<u8>(priority);
            objSemiTransparent_[sx] = objMode == 1;
        }
    }
}

u8 Ppu::windowMask(int x) const {
    const bool win0 = (dispcnt_ & 0x2000) != 0;
    const bool win1 = (dispcnt_ & 0x4000) != 0;
    const bool winObj = (dispcnt_ & 0x8000) != 0;
    if (!win0 && !win1 && !winObj) return 0x3F;
    const int line = vcount_;
    for (int w = 0; w < 2; ++w) {
        if (!(w == 0 ? win0 : win1)) continue;
        int x1 = winh_[w] >> 8, x2 = winh_[w] & 0xFF;
        int y1 = winv_[w] >> 8, y2 = winv_[w] & 0xFF;
        if (insideRange(x, x1, x2, kScreenWidth) && insideRange(line, y1, y2, kScreenHeight)) {
            return static_cast<u8>((winin_ >> (w * 8)) & 0x3F);
        }
    }
    if (winObj && objWindow_[x]) return static_cast<u8>((winout_ >> 8) & 0x3F);
    return static_cast<u8>(winout_ & 0x3F);
}

void Ppu::compose() {
    u16* out = back_.row(vcount_);
    const u16 backdrop = palette16(0);
    const int effect = (bldcnt_ >> 6) & 3;
    const int eva = std::min(16, bldalpha_ & 0x1F);
    const int evb = std::min(16, (bldalpha_ >> 8) & 0x1F);
    const int evy = std::min(16, bldy_ & 0x1F);

    // Backgrounds sorted by priority (ties: lower BG number first).
    int order[4];
    int count = 0;
    for (int p = 0; p < 4; ++p) {
        for (int bg = 0; bg < 4; ++bg) {
            if ((bgcnt_[bg] & 3) == p) order[count++] = bg;
        }
    }

    for (int x = 0; x < kScreenWidth; ++x) {
        const u8 mask = windowMask(x);
        int topLayer = kLayerBackdrop, secondLayer = kLayerBackdrop;
        u16 topColor = backdrop, secondColor = backdrop;
        int found = 0;

        const bool objHere = (mask & 0x10) && (objLine_[x] & kOpaque);
        const int objPrio = objHere ? objPriority_[x] : 4;
        bool objPlaced = false;

        for (int n = 0; n < 4 && found < 2; ++n) {
            int bg = order[n];
            int prio = bgcnt_[bg] & 3;
            if (!objPlaced && objHere && objPrio <= prio) {
                objPlaced = true;
                if (found == 0) {
                    topLayer = kLayerObj;
                    topColor = objLine_[x] & 0x7FFF;
                } else {
                    secondLayer = kLayerObj;
                    secondColor = objLine_[x] & 0x7FFF;
                }
                ++found;
                if (found == 2) break;
            }
            if (!(mask & (1 << bg)) || !(bgLine_[bg][x] & kOpaque)) continue;
            if (found == 0) {
                topLayer = bg;
                topColor = bgLine_[bg][x] & 0x7FFF;
            } else {
                secondLayer = bg;
                secondColor = bgLine_[bg][x] & 0x7FFF;
            }
            ++found;
        }
        if (!objPlaced && objHere && found < 2) {
            if (found == 0) {
                topLayer = kLayerObj;
                topColor = objLine_[x] & 0x7FFF;
            } else {
                secondLayer = kLayerObj;
                secondColor = objLine_[x] & 0x7FFF;
            }
        }

        u16 color = topColor;
        const bool effectsOn = (mask & 0x20) != 0;
        const bool secondIsTarget = (bldcnt_ & (0x100 << secondLayer)) != 0;
        if (topLayer == kLayerObj && objSemiTransparent_[x] && secondIsTarget) {
            color = blendAlpha(topColor, secondColor, eva, evb);  // semi-transparent sprite
        } else if (effectsOn && (bldcnt_ & (1 << topLayer))) {
            if (effect == 1 && secondIsTarget) {
                color = blendAlpha(topColor, secondColor, eva, evb);
            } else if (effect == 2) {
                color = brighten(topColor, evy);
            } else if (effect == 3) {
                color = darken(topColor, evy);
            }
        }
        out[x] = color;
    }
}

}  // namespace gba
