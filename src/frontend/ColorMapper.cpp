#include "ColorMapper.h"

namespace term {

namespace {
const int kCubeLevels[6] = {0, 95, 135, 175, 215, 255};

// Perceptual-ish distance ("redmean" approximation).
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
        std::uint8_t v = static_cast<std::uint8_t>(8 + (index - 232) * 10);
        c.r = c.g = c.b = v;
    } else {
        int i = index - 16;
        c.r = static_cast<std::uint8_t>(kCubeLevels[i / 36]);
        c.g = static_cast<std::uint8_t>(kCubeLevels[(i / 6) % 6]);
        c.b = static_cast<std::uint8_t>(kCubeLevels[i % 6]);
    }
    return c;
}

ColorMapper::ColorMapper(bool colorCorrection) : rgbLut_(32768), xtermLut_(32768) {
    // Palette entries 0-15 are skipped: they are themed by the user.
    Rgb palette[240];
    for (int i = 0; i < 240; ++i) palette[i] = xtermColor(16 + i);

    for (int c = 0; c < 32768; ++c) {
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        Rgb out;
        if (colorCorrection) {
            // Channel mixing similar to the real CGB screen (as in Gambatte).
            out.r = static_cast<std::uint8_t>((r * 13 + g * 2 + b) >> 1);
            out.g = static_cast<std::uint8_t>((g * 3 + b) << 1);
            out.b = static_cast<std::uint8_t>((r * 3 + g * 2 + b * 11) >> 1);
        } else {
            out.r = static_cast<std::uint8_t>((r << 3) | (r >> 2));
            out.g = static_cast<std::uint8_t>((g << 3) | (g >> 2));
            out.b = static_cast<std::uint8_t>((b << 3) | (b >> 2));
        }
        rgbLut_[c] = out;

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
