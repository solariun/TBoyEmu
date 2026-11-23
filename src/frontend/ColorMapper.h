// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/ColorMapper.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Declares term::ColorMapper, which converts the 15-bit colors produced by
//  the emulated consoles (RGB555: 5 bits each of red, green, blue) into what
//  a terminal can display: either an index into the xterm 256-color palette
//  or a 24-bit RGB triple for "truecolor" terminals. All 32768 possible
//  inputs are converted once, in the constructor, into lookup tables, so the
//  per-pixel cost while drawing is a single array access.
//
//  How it fits in
//  --------------
//  EmulatorSession owns up to two ColorMappers (with and without CGB color
//  correction) and hands the right one to TerminalRenderer and AnsiArt.
//  The cores (src/core, src/gba) write RGB555 pixels into gb::FrameBuffer;
//  this class is the only place where those values become terminal colors.
//
//  Terminal concepts
//  -----------------
//  * xterm-256 palette: indices 0-15 are the 16 "ANSI" colors, which every
//    user theme redefines, so they are never used here. 16-231 form a
//    6x6x6 color cube (index = 16 + 36*r + 6*g + b, each level one of
//    0, 95, 135, 175, 215, 255), and 232-255 are a 24-step gray ramp
//    (8, 18, ..., 238). A color is selected with SGR "CSI 38;5;n m"
//    (foreground) or "CSI 48;5;n m" (background).
//  * Truecolor: "CSI 38;2;r;g;b m" / "CSI 48;2;r;g;b m" pick an exact 24-bit
//    color; supported by most modern terminals, but not all of them, which
//    is why xterm-256 is the default. Enabled with --truecolor.
//  * CGB color correction: the Game Boy Color LCD was dim and its colors
//    bled into each other, so games chose very saturated RGB555 values.
//    Shown raw on a modern display they look garish; the optional
//    correction mixes the channels to approximate the original screen.
//
//  Contributing
//  ------------
//  * The constructor does 32768 x 240 distance computations; it is built
//    lazily (once per mode) by EmulatorSession, so keep it out of loops.
//  * Compare --truecolor with the default to judge palette quantization,
//    and --no-color-correction to see the raw CGB colors.
// ============================================================================
#ifndef TBOYEMU_FRONTEND_COLORMAPPER_H
#define TBOYEMU_FRONTEND_COLORMAPPER_H

#include <cstdint>
#include <vector>

namespace term {

// An 8-bit-per-channel color (0-255 each), as sent in truecolor SGR codes.
struct Rgb {
    std::uint8_t r, g, b;
};

// Converts Game Boy RGB555 colors to what the terminal can show: an
// xterm-256 palette index (nearest color in the 6x6x6 cube + gray ramp) or a
// 24-bit color.  Optionally applies a CGB LCD color-correction curve, since
// raw RGB555 values look far too saturated on a modern display.
//
// Immutable after construction; cheap const lookups, safe to share.
// RGB555 layout (as stored by the cores): bits 0-4 red, 5-9 green,
// 10-14 blue; bit 15 is ignored (masked off).
class ColorMapper {
public:
    // Builds both lookup tables. colorCorrection = true applies the CGB LCD
    // curve (used only for Game Boy Color screens, see EmulatorSession).
    explicit ColorMapper(bool colorCorrection);

    // xterm-256 palette index (16-255) for an RGB555 color.
    std::uint8_t xterm(std::uint16_t rgb555) const { return xtermLut_[rgb555 & 0x7FFF]; }
    // 24-bit color (after optional correction) for an RGB555 color.
    Rgb rgb(std::uint16_t rgb555) const { return rgbLut_[rgb555 & 0x7FFF]; }

    // The standard RGB value of xterm palette entry `index` (valid 16-255;
    // entries 0-15 are user-themed and not handled).
    static Rgb xtermColor(int index);

private:
    std::vector<Rgb> rgbLut_;            // 32768 entries, indexed by RGB555
    std::vector<std::uint8_t> xtermLut_; // 32768 entries, palette index 16-255
};

}  // namespace term

#endif
