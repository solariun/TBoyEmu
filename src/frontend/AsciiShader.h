// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/AsciiShader.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Defines the three ways a frame can be drawn as text (RenderStyle) and the
//  AsciiShader that turns one terminal cell of the picture into an ASCII
//  character for the two ASCII styles:
//    Blocks      (default) "upper half block" cells with foreground AND
//                background colors: 2 pixels per cell, full color.
//    Ascii       (--ascii) one plain ASCII character per cell, chosen by
//                brightness; no colors at all, the terminal's own foreground
//                and background are used.
//    AsciiColor  (--ascii-color) the same characters, each drawn in the
//                color of its pixels (foreground color only, no background).
//
//  How it fits in
//  --------------
//  TerminalRenderer (live picture) and AnsiArt (S key / --screenshot .txt)
//  both call AsciiShader::shade() for every cell in the ASCII styles, so the
//  screen and the saved text file always look the same. Options holds the
//  chosen RenderStyle (--ascii / --ascii-color). A cell covers the same 1x2
//  source pixels as a half-block cell, so the layout, scaling and frame diff
//  of TerminalRenderer are shared by all styles.
//
//  Terminal concepts
//  -----------------
//  Terminal cells are roughly twice as tall as they are wide, so one cell
//  per 1 (wide) x 2 (tall) pixels keeps the picture's proportions. In ASCII
//  mode the two pixels are averaged and their brightness picks a character
//  from a "ramp" ordered from emptiest (' ') to densest ('@'): on the usual
//  dark terminal background, dense characters read as bright areas.
//
//  Contributing
//  ------------
//  * Changing the ramp changes both the screen and saved captures; keep it
//    ordered by visual density, darkest (emptiest) first.
//  * Test: ./tboyemu --ascii <rom>, ./tboyemu --ascii-color <rom>, or
//    headless: ./tboyemu --ascii --headless --frames 600 --screenshot a.txt <rom>
//    then cat a.txt.
// ============================================================================
#ifndef TBOYEMU_FRONTEND_ASCIISHADER_H
#define TBOYEMU_FRONTEND_ASCIISHADER_H

#include <cstdint>

#include "ColorMapper.h"

namespace term {

// How frames are drawn as text (see the file header).
enum class RenderStyle { Blocks, Ascii, AsciiColor };

// One terminal cell in the ASCII styles.
struct AsciiCell {
    char glyph;           // character from the brightness ramp
    std::uint16_t color;  // average color of the cell's pixels (RGB555), for --ascii-color
};

// Converts the two source pixels of a cell into an ASCII character and a
// color.  Stateless; all members are static.
class AsciiShader {
public:
    // top, bottom: RGB555 colors of the upper and lower pixel of the cell
    // (pass the same value twice when the cell has only one pixel row).
    // colors: the game's color mapping, so brightness is measured on the
    // colors actually shown (after the optional CGB color correction).
    static AsciiCell shade(std::uint16_t top, std::uint16_t bottom, const ColorMapper& colors);

    // The brightness ramp, emptiest character first.
    static const char* ramp();
};

}  // namespace term

#endif
