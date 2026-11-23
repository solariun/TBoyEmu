#ifndef TBOYEMU_FRONTEND_COLORMAPPER_H
#define TBOYEMU_FRONTEND_COLORMAPPER_H

#include <cstdint>
#include <vector>

namespace term {

struct Rgb {
    std::uint8_t r, g, b;
};

// Converts Game Boy RGB555 colors to what the terminal can show: an
// xterm-256 palette index (nearest color in the 6x6x6 cube + gray ramp) or a
// 24-bit color.  Optionally applies a CGB LCD color-correction curve, since
// raw RGB555 values look far too saturated on a modern display.
class ColorMapper {
public:
    explicit ColorMapper(bool colorCorrection);

    std::uint8_t xterm(std::uint16_t rgb555) const { return xtermLut_[rgb555 & 0x7FFF]; }
    Rgb rgb(std::uint16_t rgb555) const { return rgbLut_[rgb555 & 0x7FFF]; }

    static Rgb xtermColor(int index);

private:
    std::vector<Rgb> rgbLut_;
    std::vector<std::uint8_t> xtermLut_;
};

}  // namespace term

#endif
