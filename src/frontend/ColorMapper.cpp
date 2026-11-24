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
//  Implementation of ColorMapper (see ColorMapper.h): builds the RGB555 ->
//  RGB lookup table and converts frames into gfx::Image.
//
//  How it fits in
//  --------------
//  Called once per drawn frame by TerminalRenderer, once per recorded frame
//  by Recorder and once per capture by AnsiArt.
//
//  Terminal concepts
//  -----------------
//  Correction (from Gambatte), with r, g, b the 5-bit channels (0-31):
//      R = (13 r + 2 g + b) / 2
//      G = (3 g + b) * 2
//      B = (3 r + 2 g + 11 b) / 2
//  Each result is at most 31 * 16 / 2 = 248, so it fits in a byte.
//  Without correction each channel is expanded as (v << 3) | (v >> 2), which
//  maps 0..31 onto 0..255 exactly.
//
//  Contributing
//  ------------
//  toImage() runs for every frame (up to 38,400 pixels for the GBA); keep it
//  a plain table lookup.
// ============================================================================
#include "ColorMapper.h"

namespace term {

ColorMapper::ColorMapper(bool colorCorrection) : lut_(32768) {
    for (int c = 0; c < 32768; ++c) {
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        if (colorCorrection) {
            lut_[static_cast<size_t>(c)] = gfx::rgb(static_cast<std::uint8_t>((r * 13 + g * 2 + b) >> 1),
                                                    static_cast<std::uint8_t>((g * 3 + b) << 1),
                                                    static_cast<std::uint8_t>((r * 3 + g * 2 + b * 11) >> 1));
        } else {
            lut_[static_cast<size_t>(c)] = gfx::rgb(static_cast<std::uint8_t>((r << 3) | (r >> 2)),
                                                    static_cast<std::uint8_t>((g << 3) | (g >> 2)),
                                                    static_cast<std::uint8_t>((b << 3) | (b >> 2)));
        }
    }
}

void ColorMapper::toImage(const gb::FrameBuffer& frame, gfx::Image& image) const {
    if (image.width() != frame.width() || image.height() != frame.height()) image.resize(frame.width(), frame.height());
    for (int y = 0; y < frame.height(); ++y) {
        const std::uint16_t* in = frame.row(y);
        gfx::Rgb* out = image.row(y);
        for (int x = 0; x < frame.width(); ++x) out[x] = lut_[in[x] & 0x7FFF];
    }
}

}  // namespace term
