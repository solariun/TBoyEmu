// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/ColorMapper.cpp
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Implements ColorMapper (see ColorMapper.h): builds the RGB555 -> RGB888
//  table (plain bit expansion, or CGB LCD color correction) and the
//  RGB555 -> xterm-256 table by brute-force nearest-color search over the
//  240 non-themed palette entries using the "redmean" color distance.
//
//  How it fits in
//  --------------
//  Constructed by EmulatorSession::mapperFor() and HeadlessRunner (for .txt
//  screenshots); queried per pixel by TerminalRenderer and AnsiArt.
//
//  Terminal concepts
//  -----------------
//  * Quantization: the xterm-256 palette has only 240 usable colors while
//    the consoles can produce 32768, so many inputs share one index. A plain
//    Euclidean RGB distance picks visibly wrong hues; "redmean" weights the
//    channels by how sensitive the eye is to them (green most, and red/blue
//    depending on how red the colors are), which is cheap and much better.
//  * Expanding 5 bits to 8: (v << 3) | (v >> 2) maps 0 -> 0 and 31 -> 255
//    exactly, spreading the values evenly.
//  * Color correction: the matrix used here is the integer channel mix
//    known from the Gambatte emulator; each output channel is a weighted
//    sum of all three inputs, which desaturates the colors and shifts them
//    toward what the real CGB screen showed.
//
//  Contributing
//  ------------
//  * If you change the correction formula, make sure every result still
//    fits in 0-255: max r = (31*13 + 31*2 + 31) >> 1 = 248,
//    g = (31*3 + 31) << 1 = 248, b = (31*3 + 31*2 + 31*11) >> 1 = 248.
//  * Visual check: run a GBC game, press S to save a screenshot, and compare
//    with --truecolor and --no-color-correction.
// ============================================================================
#include "ColorMapper.h"

namespace term {

namespace {
// Channel intensities of the 6 levels of the xterm 6x6x6 color cube.
const int kCubeLevels[6] = {0, 95, 135, 175, 215, 255};

// Perceptual-ish distance ("redmean" approximation).
// Returns a squared distance (no sqrt needed for comparisons):
//   (2 + rmean/256) * dR^2 + 4 * dG^2 + (2 + (255 - rmean)/256) * dB^2
// written with integer shifts; rmean is the average red of both colors.
long distance(const Rgb& a, const Rgb& b) {
    long rmean = (static_cast<long>(a.r) + b.r) / 2;
    long dr = static_cast<long>(a.r) - b.r;
    long dg = static_cast<long>(a.g) - b.g;
    long db = static_cast<long>(a.b) - b.b;
    return (((512 + rmean) * dr * dr) >> 8) + 4 * dg * dg + (((767 - rmean) * db * db) >> 8);
}
}  // namespace

Rgb ColorMapper::xtermColor(int index) {
    Rgb c;
    if (index >= 232) {
        // Gray ramp: 232 -> 8, 233 -> 18, ... 255 -> 238 (steps of 10).
        std::uint8_t v = static_cast<std::uint8_t>(8 + (index - 232) * 10);
        c.r = c.g = c.b = v;
    } else {
        // Color cube: index - 16 = 36*r + 6*g + b, each digit 0-5.
        int i = index - 16;
        c.r = static_cast<std::uint8_t>(kCubeLevels[i / 36]);
        c.g = static_cast<std::uint8_t>(kCubeLevels[(i / 6) % 6]);
        c.b = static_cast<std::uint8_t>(kCubeLevels[i % 6]);
    }
    return c;
}

ColorMapper::ColorMapper(bool colorCorrection) : rgbLut_(32768), xtermLut_(32768) {
    // Palette entries 0-15 are skipped: they are themed by the user.
    // palette[i] holds the RGB value of xterm color 16 + i.
    Rgb palette[240];
    for (int i = 0; i < 240; ++i) palette[i] = xtermColor(16 + i);

    // For each of the 32768 RGB555 values...
    for (int c = 0; c < 32768; ++c) {
        // Unpack 5-bit channels: bits 0-4 red, 5-9 green, 10-14 blue.
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        Rgb out;
        if (colorCorrection) {
            // Channel mixing similar to the real CGB screen (as in Gambatte).
            // Weights sum to 16 (red, blue) or 4 (green, then x2), so a 0-31
            // input lands in 0-248 after the shift.
            out.r = static_cast<std::uint8_t>((r * 13 + g * 2 + b) >> 1);
            out.g = static_cast<std::uint8_t>((g * 3 + b) << 1);
            out.b = static_cast<std::uint8_t>((r * 3 + g * 2 + b * 11) >> 1);
        } else {
            // Plain 5-bit -> 8-bit expansion (replicates the top bits).
            out.r = static_cast<std::uint8_t>((r << 3) | (r >> 2));
            out.g = static_cast<std::uint8_t>((g << 3) | (g >> 2));
            out.b = static_cast<std::uint8_t>((b << 3) | (b >> 2));
        }
        rgbLut_[c] = out;

        // Nearest xterm color by exhaustive search (240 candidates). This is
        // the expensive part of construction, done once per mapper.
        int best = 0;
        long bestDist = -1;
        for (int i = 0; i < 240; ++i) {
            long d = distance(out, palette[i]);
            if (bestDist < 0 || d < bestDist) {
                bestDist = d;
                best = i;
            }
        }
        xtermLut_[c] = static_cast<std::uint8_t>(16 + best);
    }
}

}  // namespace term
