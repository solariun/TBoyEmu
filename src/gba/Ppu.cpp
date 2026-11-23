// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Ppu.cpp
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Implementation of gba::Ppu (see Ppu.h for the interface and the register map):
//   - tick(): the scanline state machine (H-Draw -> H-Blank -> next line), DISPSTAT flags,
//     VCOUNT, the H-Blank / V-Blank / V-Counter interrupts and the frame buffer swap;
//   - readIo()/writeIo(): decoding of the LCD I/O registers;
//   - the line renderer: one function per layer type (text BG, affine BG, bitmap BG,
//     sprites) writing into per-line layer buffers, then compose() which resolves priority,
//     windows and color special effects into the final RGB555 line.
//
//  How it fits in
//  --------------
//  Gba::runFrame() calls tick() after every CPU step. renderLine() runs when a visible line
//  enters H-Blank and writes row vcount_ of back_; at line 160 back_ and front_ are swapped
//  and the frontend reads front_ through Gba::frame().
//
//  Hardware notes
//  --------------
//  GBATEK sections used: "LCD I/O Display Control" (DISPCNT, DISPSTAT, VCOUNT),
//  "LCD I/O BG Control", "LCD I/O BG Scrolling", "LCD I/O BG Rotation/Scaling",
//  "LCD VRAM Overview" / "LCD VRAM Character Data" / "LCD VRAM BG Screen Data Format",
//  "LCD OBJ - OAM Attributes", "LCD OBJ - OAM Rotation/Scaling Parameters",
//  "LCD I/O Window Feature", "LCD I/O Mosaic Function", "LCD I/O Color Special Effects".
//  Simplifications compared with real hardware (all intentional, keep them in mind when
//  debugging a game):
//   - whole-line rendering, so mid-line register writes take effect on the next line;
//   - no sprite cycle budget (the hardware drops sprites when a line is too busy);
//   - mosaic only on text backgrounds (not on affine/bitmap BGs or sprites);
//   - the V-Blank flag is cleared at line 227, as on hardware.
//
//  Contributing
//  ------------
//  - Layer buffers use bit 15 (kOpaque) as "pixel present"; any new layer code must set it,
//    otherwise compose() treats the pixel as transparent.
//  - VRAM reads in the BG renderers are bounds-checked against 0x10000 (the BG area) so a bad
//    tile/map base can never read sprite tiles or overflow the array; keep those checks.
//  - Verify visually with headless screenshots of a few games (tiled, affine and bitmap
//    modes):  make && ./tboyemu --headless --frames 600 --screenshot out.png rom.gba
// ============================================================================
#include "Ppu.h"

#include <algorithm>
#include <cstring>

namespace gba {

namespace {

// Sprite sizes [shape][size] in pixels (width, height).
// shape = OAM attribute 0 bits 14-15 (0 square, 1 horizontal, 2 vertical, 3 prohibited),
// size  = OAM attribute 1 bits 14-15. E.g. shape 1 size 0 = 16x8, shape 2 size 3 = 32x64.
const int kObjWidth[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
const int kObjHeight[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

// Layer ids used by BLDCNT / window masks.
// BG0-BG3 are layers 0-3 (their own number); OBJ is bit 4 and the backdrop bit 5, matching
// the bit positions in BLDCNT and WININ/WINOUT.
const int kLayerObj = 4;
const int kLayerBackdrop = 5;

// Sign-extends a 28-bit value (BGxX / BGxY reference point registers are 20.8 fixed point
// with bit 27 as the sign) to a full 32-bit signed integer.
s32 signExtend28(u32 v) { return static_cast<s32>(v << 4) >> 4; }

// Window coordinate test for one axis: is v inside [start, end)?
// Hardware quirks reproduced here (GBATEK "LCD I/O Window Feature"):
//  - if start > end the window wraps around the screen edge: inside means v >= start OR
//    v < end;
//  - if end is larger than the screen (limit), it is treated as the screen size.
bool insideRange(int v, int start, int end, int limit) {
    if (end > limit || start > end) {
        // Out-of-range end or wrap-around window.
        if (start > end) return v >= start || v < end;
        end = limit;
    }
    return v >= start && v < end;
}

// Alpha blending: result = (a * EVA + b * EVB) / 16 per 5-bit channel, saturated at 31.
// a is the 1st target (top) color, b the 2nd target (below) color; eva/evb are 0-16.
u16 blendAlpha(u16 a, u16 b, int eva, int evb) {
    int r = std::min(31, ((a & 31) * eva + (b & 31) * evb) >> 4);
    int g = std::min(31, (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4);
    int bl = std::min(31, (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4);
    return static_cast<u16>(r | (g << 5) | (bl << 10));
}

// Brightness increase: each channel moves towards white by EVY/16 of the remaining
// distance: I = I + (31 - I) * EVY / 16 (evy is 0-16).
u16 brighten(u16 c, int evy) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return static_cast<u16>(r | (g << 5) | (b << 10));
}

// Brightness decrease: each channel moves towards black: I = I - I * EVY / 16.
u16 darken(u16 c, int evy) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return static_cast<u16>(r | (g << 5) | (b << 10));
}

}  // namespace

// Both frame buffers are 240x160; reset() puts everything into its power-on state.
Ppu::Ppu(Interrupts& interrupts)
    : interrupts_(interrupts), back_(kScreenWidth, kScreenHeight), front_(kScreenWidth, kScreenHeight) {
    reset();
}

// Power-on state. DISPCNT starts with bit 7 (forced blank) set, so the screen is white until
// the game (or the BIOS HLE) configures the display. The affine matrices start as identity
// (PA = PD = 0x100 = 1.0 in 8.8 fixed point) so an affine BG with untouched parameters
// shows the picture unscaled.
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
    // 0x7FFF = white in RGB555.
    back_.fill(0x7FFF);
    front_.fill(0x7FFF);
}

// --------------------------------------------------------------- timing ----

// While drawing, the next event is the H-Blank start at cycle 960; during H-Blank it is the
// end of the line at cycle 1232. Always > 0 because tick() consumes boundaries eagerly.
int Ppu::cyclesUntilEvent() const {
    return inHBlank_ ? kCyclesPerLine - lineCycle_ : kHDrawCycles - lineCycle_;
}

// Scanline state machine. The loop handles any number of boundaries in one call (a halted
// CPU can skip many cycles at once). Each iteration either:
//  (a) H-Draw -> H-Blank at cycle 960: set DISPSTAT bit 1, render the line if visible,
//      and request the H-Blank IRQ if DISPSTAT bit 4 enables it. Note that, as on hardware,
//      the H-Blank flag and IRQ also happen during V-Blank lines, but the H-Blank DMA
//      event (EventHBlank) is only reported for visible lines (GBATEK "DMA Transfers").
//  (b) H-Blank -> next line at cycle 1232: clear the H-Blank flag, increment VCOUNT, handle
//      the V-Blank start (line 160), the V-Blank flag clear (line 227) and the wrap to
//      line 0, then compare VCOUNT with the V-Count setting (DISPSTAT bits 8-15).
u8 Ppu::tick(int cycles) {
    u8 events = 0;
    lineCycle_ += cycles;
    for (;;) {
        if (!inHBlank_) {
            if (lineCycle_ < kHDrawCycles) break;
            inHBlank_ = true;
            // DISPSTAT bit 1: H-Blank flag.
            dispstat_ |= 0x0002;
            if (vcount_ < kVisibleLines) {
                renderLine();
                events |= EventHBlank;
            }
            // DISPSTAT bit 4: H-Blank IRQ enable.
            if (dispstat_ & 0x0010) interrupts_.request(irq::HBlank);
        } else {
            if (lineCycle_ < kCyclesPerLine) break;
            lineCycle_ -= kCyclesPerLine;
            inHBlank_ = false;
            dispstat_ &= static_cast<u16>(~0x0002);
            ++vcount_;
            if (vcount_ == kVisibleLines) {
                // Line 160: V-Blank begins. DISPSTAT bit 0 = V-Blank flag, bit 3 = its IRQ
                // enable. The finished picture in back_ becomes the visible front_.
                dispstat_ |= 0x0001;
                if (dispstat_ & 0x0008) interrupts_.request(irq::VBlank);
                std::swap(front_, back_);
                // Affine reference points are reloaded every frame.
                // (the internal counters restart from the BGxX/BGxY register values)
                for (int i = 0; i < 2; ++i) {
                    refX_[i] = bgx_[i];
                    refY_[i] = bgy_[i];
                }
                events |= EventVBlank | EventFrame;
            } else if (vcount_ == kLinesPerFrame - 1) {
                // Line 227: the hardware clears the V-Blank flag on the last line.
                dispstat_ &= static_cast<u16>(~0x0001);
            } else if (vcount_ >= kLinesPerFrame) {
                // Past line 227: wrap to the first line of the next frame.
                vcount_ = 0;
            }
            // V-Counter match: DISPSTAT bits 8-15 hold the line to compare with. Bit 2 is the
            // match flag (kept up to date every line), bit 5 enables the V-Counter IRQ.
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

// Readable LCD registers. The masks drop the bits that read back as 0 on hardware (unused
// bits of WININ/WINOUT/BLDCNT/BLDALPHA). Scroll, affine, window-coordinate, MOSAIC and BLDY
// registers are write-only and read as 0 here.
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

// LCD register writes, in three groups: scroll registers, affine registers, the rest.
void Ppu::writeIo(u32 offset, u16 v) {
    // 0x10-0x1F: BG0HOFS, BG0VOFS, BG1HOFS, ... BG3VOFS (4 bytes per BG; bit 1 of the
    // offset selects vertical). Only 9 bits are used (0-511).
    if (offset >= 0x10 && offset < 0x20) {
        int bg = static_cast<int>((offset - 0x10) / 4);
        if (offset & 2) {
            bgvofs_[bg] = v & 0x1FF;
        } else {
            bghofs_[bg] = v & 0x1FF;
        }
        return;
    }
    // 0x20-0x3F: BG2 (0x20-0x2F) and BG3 (0x30-0x3F) affine parameters:
    //  +0 PA, +2 PB, +4 PC, +6 PD (8.8 signed), +8/+A X low/high, +C/+E Y low/high.
    // The 28-bit reference point is written as two halfwords; each write merges the new
    // half, sign-extends, and (as on hardware) also reloads the internal counter so the
    // new value applies from the next line on.
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
        // DISPSTAT: bits 0-2 are read-only status flags (kept from our state), bits 3-5 IRQ
        // enables and 8-15 the V-Count setting are writable (mask 0xFF38).
        case 0x04: dispstat_ = static_cast<u16>((dispstat_ & 0x0007) | (v & 0xFF38)); break;
        case 0x08: case 0x0A: case 0x0C: case 0x0E: bgcnt_[(offset - 0x08) / 2] = v; break;
        // Window coordinates, window layer masks, mosaic and blending registers are stored
        // as written; the renderer extracts the fields it needs.
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

// Renders line vcount_ (0-159). Order: backgrounds of the current mode -> advance affine
// counters -> sprites -> compose. Each layer function only writes opaque pixels, so the
// buffers are cleared to 0 (transparent) first.
void Ppu::renderLine() {
    u16* out = back_.row(vcount_);
    if (dispcnt_ & 0x0080) {  // forced blank
        // DISPCNT bit 7: the LCD shows white and nothing is rendered.
        for (int x = 0; x < kScreenWidth; ++x) out[x] = 0x7FFF;
        return;
    }
    for (int bg = 0; bg < 4; ++bg) std::memset(bgLine_[bg], 0, sizeof(bgLine_[bg]));

    // Which backgrounds exist in each mode (GBATEK "LCD I/O Display Control"):
    //  mode 0: BG0-3 text;  mode 1: BG0-1 text + BG2 affine;  mode 2: BG2-3 affine;
    //  modes 3-5: BG2 is a bitmap. DISPCNT bits 8-11 enable BG0-3 individually.
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
            // Modes 6 and 7 are prohibited: no background is drawn.
            break;
    }

    // Affine reference points advance once per scanline.
    // (by the PB/PD column of the matrix: moving one line down on screen moves (dmx, dmy)
    // in texture space). Done even when the BG is off, like the hardware counters.
    for (int i = 0; i < 2; ++i) {
        refX_[i] += bgpb_[i];
        refY_[i] += bgpd_[i];
    }

    std::memset(objLine_, 0, sizeof(objLine_));
    std::memset(objWindow_, 0, sizeof(objWindow_));
    std::memset(objSemiTransparent_, 0, sizeof(objSemiTransparent_));
    // DISPCNT bit 12: OBJ (sprite) layer enable.
    if (dispcnt_ & 0x1000) renderSprites();

    compose();
}

// Text ("regular") background: a scrolling tile map of 8x8 tiles.
// BGxCNT fields: char base = bits 2-3 * 16 KiB (tile pixel data), screen base = bits 8-12
// * 2 KiB (tile map), bit 7 = 256-color (8bpp) tiles instead of 16-color (4bpp), bits 14-15
// = map size (0: 256x256, 1: 512x256, 2: 256x512, 3: 512x512 pixels), bit 6 = mosaic.
// The map is made of 32x32-entry "screen blocks" of 2 KiB each; a larger map stores
// several blocks one after another (left->right, then top->bottom).
// Map entry (16 bits): bits 0-9 tile number, 10 horizontal flip, 11 vertical flip,
// 12-15 palette bank (4bpp only).
void Ppu::renderTextBg(int bg) {
    const u16 cnt = bgcnt_[bg];
    const u32 charBase = ((cnt >> 2) & 3) * 0x4000u;
    const u32 screenBase = ((cnt >> 8) & 0x1F) * 0x800u;
    const bool color256 = (cnt & 0x80) != 0;
    const int size = cnt >> 14;
    const int width = (size & 1) ? 512 : 256;
    const int height = (size & 2) ? 512 : 256;
    const bool mosaic = (cnt & 0x40) != 0;
    // MOSAIC register: block sizes are stored minus one (0 = 1 pixel = no effect).
    const int mosaicH = (mosaic_ & 0xF) + 1;
    const int mosaicV = ((mosaic_ >> 4) & 0xF) + 1;

    // Mosaic snaps the source line / column down to the start of its mosaic block, so
    // every pixel of a block repeats the top-left pixel.
    int line = vcount_;
    if (mosaic) line -= line % mosaicV;
    // y: row inside the (wrapping) background map, after vertical scroll.
    const int y = (line + bgvofs_[bg]) & (height - 1);
    u16* out = bgLine_[bg];

    for (int x = 0; x < kScreenWidth; ++x) {
        int srcX = mosaic ? x - x % mosaicH : x;
        // sx: column inside the map after horizontal scroll (maps wrap around).
        int sx = (srcX + bghofs_[bg]) & (width - 1);
        // Pick the 256x256 screen block that contains (sx, y) for the larger map sizes.
        int block = 0;
        if (size == 1) block = sx >> 8;
        else if (size == 2) block = y >> 8;
        else if (size == 3) block = (sx >> 8) + ((y >> 8) << 1);
        // Map entry address: 32 entries (64 bytes) per map row, 2 bytes per entry.
        u32 entryAddr = screenBase + static_cast<u32>(block) * 0x800u + static_cast<u32>(((y & 255) >> 3) * 64 + ((sx & 255) >> 3) * 2);
        // BG data must stay inside the 64 KiB BG area of VRAM.
        if (entryAddr >= 0x10000) continue;
        u16 entry = vram16(entryAddr);
        u32 tile = entry & 0x3FF;
        // Pixel inside the 8x8 tile, with the entry's flip bits applied.
        int px = sx & 7, py = y & 7;
        if (entry & 0x400) px = 7 - px;
        if (entry & 0x800) py = 7 - py;
        u32 index;
        if (color256) {
            // 8bpp tile: 64 bytes, one palette index (0-255) per byte.
            u32 addr = charBase + tile * 64 + static_cast<u32>(py * 8 + px);
            if (addr >= 0x10000) continue;
            index = vram_[addr];
        } else {
            // 4bpp tile: 32 bytes, two pixels per byte (low nibble = left pixel), index 1-15
            // into the 16-color palette bank selected by the map entry.
            u32 addr = charBase + tile * 32 + static_cast<u32>(py * 4 + (px >> 1));
            if (addr >= 0x10000) continue;
            index = (vram_[addr] >> ((px & 1) * 4)) & 0xF;
            if (index) index += (entry >> 12) * 16u;
        }
        // Color index 0 is transparent in every palette.
        if (index) out[x] = static_cast<u16>(palette16(index) | kOpaque);
    }
}

// Affine (rotation/scaling) background, BG2 or BG3. Always 8bpp tiles; the map is a square
// of 128 << (bits 14-15) pixels (128, 256, 512 or 1024) with ONE byte per entry (tile number
// only: no flips, no palette bank). BGxCNT bit 13 selects wrap-around vs. transparent
// outside the map.
// For each screen pixel the texture coordinate (fx, fy) in 20.8 fixed point starts at the
// internal reference point of this line and moves by (PA, PC) per pixel to the right.
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
        // Integer texture coordinates (drop the 8 fraction bits).
        int tx = fx >> 8;
        int ty = fy >> 8;
        if (wrap) {
            // size is a power of two, so masking wraps the coordinate around the map.
            tx &= size - 1;
            ty &= size - 1;
        } else if (tx < 0 || ty < 0 || tx >= size || ty >= size) {
            // Outside the map and no wrap: transparent.
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

// Bitmap modes (BG2 only, drawn through the BG2 affine matrix like an affine BG, so games can
// rotate/scale a bitmap):
//  mode 3: 240x160, 16-bit direct RGB555 colors, single frame (75 KiB);
//  mode 4: 240x160, 8-bit palette indices, two frames at 0x0000 and 0xA000;
//  mode 5: 160x128, 16-bit direct colors, two frames at 0x0000 and 0xA000.
// DISPCNT bit 4 selects the displayed frame (page flipping) in modes 4 and 5.
// Pixels outside the bitmap are transparent (no wrap-around).
void Ppu::renderBitmapBg(int mode) {
    const int width = mode == 5 ? 160 : 240;
    const int height = mode == 5 ? 128 : 160;
    const u32 page = (mode != 3 && (dispcnt_ & 0x10)) ? 0xA000 : 0;
    u16* out = bgLine_[2];

    // Index 0 = the BG2 affine parameters.
    s32 fx = refX_[0];
    s32 fy = refY_[0];
    for (int x = 0; x < kScreenWidth; ++x, fx += bgpa_[0], fy += bgpc_[0]) {
        int tx = fx >> 8;
        int ty = fy >> 8;
        if (tx < 0 || ty < 0 || tx >= width || ty >= height) continue;
        if (mode == 4) {
            // Palette index 0 is transparent, as in tiled modes.
            u8 index = vram_[page + static_cast<u32>(ty * width + tx)];
            if (index) out[x] = static_cast<u16>(palette16(index) | kOpaque);
        } else {
            // Direct color: always opaque (bit 15 of the stored value is ignored).
            u16 color = vram16(page + static_cast<u32>(ty * width + tx) * 2);
            out[x] = static_cast<u16>((color & 0x7FFF) | kOpaque);
        }
    }
}

// Sprites (OBJs). Walks all 128 OAM entries in order and draws the pixels that fall on the
// current line. OAM entry layout (4 halfwords, 8 bytes; GBATEK "LCD OBJ - OAM Attributes"):
//  attr0 (a0): bits 0-7 Y, 8 affine flag, 9 double-size (affine) / disable (regular),
//              10-11 mode (0 normal, 1 semi-transparent, 2 OBJ window, 3 prohibited),
//              12 mosaic (ignored here), 13 256-color, 14-15 shape.
//  attr1 (a1): bits 0-8 X, 9-13 affine parameter set (affine) or 12/13 H/V flip (regular),
//              14-15 size.
//  attr2 (a2): bits 0-9 tile number, 10-11 priority, 12-15 palette bank (4bpp).
//  The 4th halfword of entries 4n..4n+3 holds PA, PB, PC, PD of affine parameter set n.
// Sprite tiles live in VRAM 0x10000-0x17FFF; colors come from the OBJ palette (256-511).
void Ppu::renderSprites() {
    // DISPCNT bit 6: 1D tile mapping (a sprite's tiles are consecutive) vs 2D (the tile area
    // is a 32-tile-wide matrix and each sprite row starts 32 tiles further).
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
        // Double-size affine sprites get a bounding box twice as big (so a rotated sprite is
        // not clipped); the texture itself is still w x h.
        const bool doubleSize = affine && (a0 & 0x200);
        const int boundW = doubleSize ? w * 2 : w;
        const int boundH = doubleSize ? h * 2 : h;

        // Y is 8 bits and X 9 bits; values past the screen edge wrap to negative positions
        // so sprites can be partly off the top/left edge.
        int y = a0 & 0xFF;
        if (y >= kScreenHeight) y -= 256;
        int x = a1 & 0x1FF;
        if (x >= kScreenWidth) x -= 512;
        if (line < y || line >= y + boundH) continue;

        const u32 tileBase = a2 & 0x3FF;
        // In bitmap modes the first half of the OBJ tile area (0x10000-0x13FFF) is used by
        // the bitmap, so tile numbers below 512 are not usable for sprites.
        if (bitmap && tileBase < 512) continue;  // tiles overlapped by the bitmap
        const int priority = (a2 >> 10) & 3;
        const bool color256 = (a0 & 0x2000) != 0;
        const u32 palBank = (a2 >> 12) * 16u;

        // Affine matrix of the sprite in 8.8 fixed point (identity for regular sprites).
        // Parameter set n is spread over OAM halfwords 16n+3, 16n+7, 16n+11, 16n+15.
        s16 pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            u32 p = ((a1 >> 9) & 0x1F) * 16u;
            pa = static_cast<s16>(oam16(p + 3));
            pb = static_cast<s16>(oam16(p + 7));
            pc = static_cast<s16>(oam16(p + 11));
            pd = static_cast<s16>(oam16(p + 15));
        }
        // dy: row of the bounding box that this screen line crosses.
        const int dy = line - y;

        for (int px = 0; px < boundW; ++px) {
            int sx = x + px;
            if (sx < 0 || sx >= kScreenWidth) continue;
            // (tx, ty): texel inside the w x h sprite image.
            int tx, ty;
            if (affine) {
                // Rotate/scale around the center: take the offset from the bounding box
                // center, transform it by the matrix, then move to the texture center.
                int cx = px - boundW / 2;
                int cy = dy - boundH / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                // a1 bit 12 = horizontal flip, bit 13 = vertical flip.
                tx = (a1 & 0x1000) ? w - 1 - px : px;
                ty = (a1 & 0x2000) ? h - 1 - dy : dy;
            }
            // Tile number of the 8x8 tile containing the texel. Tile numbers always count
            // 32-byte units, so an 8bpp tile (64 bytes) takes two numbers (hence * 2).
            // 1D: rows of the sprite follow each other (row stride = sprite width in tiles);
            // 2D: rows are 32 tile numbers apart.
            u32 tileX = static_cast<u32>(tx >> 3);
            u32 tileY = static_cast<u32>(ty >> 3);
            u32 tileNum;
            if (color256) {
                tileNum = tileBase + (oneDimensional ? tileY * static_cast<u32>(w / 8) * 2 : tileY * 32) + tileX * 2;
            } else {
                tileNum = tileBase + (oneDimensional ? tileY * static_cast<u32>(w / 8) : tileY * 32) + tileX;
            }
            // OBJ tiles start at VRAM 0x10000; the tile number wraps within the 32 KiB area.
            u32 tileAddr = 0x10000 + (tileNum & 0x3FF) * 32;
            u32 index;
            if (color256) {
                // 8bpp: palette index into the OBJ palette (offset 256 in palette RAM).
                index = vram_[tileAddr + static_cast<u32>((ty & 7) * 8 + (tx & 7))];
                if (index) index += 256;
            } else {
                // 4bpp: nibble per pixel, plus the sprite's 16-color palette bank.
                u8 b = vram_[tileAddr + static_cast<u32>((ty & 7) * 4 + ((tx & 7) >> 1))];
                index = (b >> ((tx & 1) * 4)) & 0xF;
                if (index) index += 256 + palBank;
            }
            if (!index) continue;
            // OBJ window sprites are invisible; they only mark pixels for the OBJ window.
            if (objMode == 2) {
                objWindow_[sx] = true;
                continue;
            }
            // Lower OAM index wins between sprites; first one drawn stays.
            // ...unless a later sprite has a strictly better (lower) priority value; in that
            // case it replaces the pixel.
            if (objLine_[sx] & kOpaque) {
                if (objPriority_[sx] <= priority) continue;
            }
            objLine_[sx] = static_cast<u16>(palette16(index) | kOpaque);
            objPriority_[sx] = static_cast<u8>(priority);
            objSemiTransparent_[sx] = objMode == 1;
        }
    }
}

// Layer-enable mask for pixel x of the current line. Window priority on hardware is
// WIN0 > WIN1 > OBJ window > outside. With no window enabled in DISPCNT (bits 13-15) every
// layer and the color effects are allowed (0x3F).
// Returned bits: 0-3 BG0-3, 4 OBJ, 5 color special effects.
u8 Ppu::windowMask(int x) const {
    const bool win0 = (dispcnt_ & 0x2000) != 0;
    const bool win1 = (dispcnt_ & 0x4000) != 0;
    const bool winObj = (dispcnt_ & 0x8000) != 0;
    if (!win0 && !win1 && !winObj) return 0x3F;
    const int line = vcount_;
    for (int w = 0; w < 2; ++w) {
        if (!(w == 0 ? win0 : win1)) continue;
        // WINxH: X1 = left (bits 8-15), X2 = right + 1 (bits 0-7); WINxV likewise for Y.
        int x1 = winh_[w] >> 8, x2 = winh_[w] & 0xFF;
        int y1 = winv_[w] >> 8, y2 = winv_[w] & 0xFF;
        if (insideRange(x, x1, x2, kScreenWidth) && insideRange(line, y1, y2, kScreenHeight)) {
            // WININ: low byte for WIN0, high byte for WIN1.
            return static_cast<u8>((winin_ >> (w * 8)) & 0x3F);
        }
    }
    // WINOUT: high byte = inside the OBJ window, low byte = outside every window.
    if (winObj && objWindow_[x]) return static_cast<u8>((winout_ >> 8) & 0x3F);
    return static_cast<u8>(winout_ & 0x3F);
}

// Final per-pixel merge of the line. For every pixel it finds the two front-most visible
// layers ("top" = 1st target candidate, "second" = what lies directly below), honoring:
//  - BG priority (BGxCNT bits 0-1, 0 = front); among equal priorities the lower BG number
//    is in front;
//  - sprites are in front of BGs that have the same or a lower (numerically higher)
//    priority value;
//  - the window mask (a layer disabled by the window is skipped);
//  - the backdrop (palette color 0) behind everything.
// Then it applies the BLDCNT color effect if the window allows effects and the top layer is
// a 1st target. Semi-transparent sprites always alpha-blend with a 2nd target below them,
// whatever the effect mode and the window effect bit (as implemented here).
void Ppu::compose() {
    u16* out = back_.row(vcount_);
    const u16 backdrop = palette16(0);
    const int effect = (bldcnt_ >> 6) & 3;
    // Blend coefficients are in 1/16 units; values above 16 behave as 16.
    const int eva = std::min(16, bldalpha_ & 0x1F);
    const int evb = std::min(16, (bldalpha_ >> 8) & 0x1F);
    const int evy = std::min(16, bldy_ & 0x1F);

    // Backgrounds sorted by priority (ties: lower BG number first).
    // Disabled BGs are still in the list; their line buffers are empty so they never win.
    int order[4];
    int count = 0;
    for (int p = 0; p < 4; ++p) {
        for (int bg = 0; bg < 4; ++bg) {
            if ((bgcnt_[bg] & 3) == p) order[count++] = bg;
        }
    }

    for (int x = 0; x < kScreenWidth; ++x) {
        const u8 mask = windowMask(x);
        // Defaults: if fewer than two layers are found, the backdrop fills the gap.
        int topLayer = kLayerBackdrop, secondLayer = kLayerBackdrop;
        u16 topColor = backdrop, secondColor = backdrop;
        int found = 0;

        // Sprite pixel present and allowed by the window (mask bit 4)?
        const bool objHere = (mask & 0x10) && (objLine_[x] & kOpaque);
        const int objPrio = objHere ? objPriority_[x] : 4;
        bool objPlaced = false;

        // Walk BGs front to back; insert the sprite pixel before the first BG whose
        // priority is not better than the sprite's. Stop after two layers.
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
            // Skip BGs hidden by the window or transparent at this pixel.
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
        // Sprite behind every BG (or no opaque BG at all): it goes after the BGs found.
        if (!objPlaced && objHere && found < 2) {
            if (found == 0) {
                topLayer = kLayerObj;
                topColor = objLine_[x] & 0x7FFF;
            } else {
                secondLayer = kLayerObj;
                secondColor = objLine_[x] & 0x7FFF;
            }
        }

        // Color special effects. BLDCNT bits 0-5 select 1st targets, bits 8-13 2nd targets
        // (bit index = layer id: BG0-3, OBJ = 4, backdrop = 5).
        u16 color = topColor;
        const bool effectsOn = (mask & 0x20) != 0;
        const bool secondIsTarget = (bldcnt_ & (0x100 << secondLayer)) != 0;
        if (topLayer == kLayerObj && objSemiTransparent_[x] && secondIsTarget) {
            color = blendAlpha(topColor, secondColor, eva, evb);  // semi-transparent sprite
        } else if (effectsOn && (bldcnt_ & (1 << topLayer))) {
            // effect 1: alpha blend (needs a 2nd target below), 2: brighten, 3: darken.
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
