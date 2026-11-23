// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/AsciiShader.cpp
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Implementation of AsciiShader (see AsciiShader.h): averages the two
//  pixels of a cell, measures their brightness and picks the matching
//  character from the ramp.
//
//  How it fits in
//  --------------
//  Called once per changed cell by TerminalRenderer::present() and once per
//  cell by AnsiArt::encode() when the render style is Ascii or AsciiColor.
//
//  Terminal concepts
//  -----------------
//  Brightness uses the Rec. 601 luma weights (0.299 R + 0.587 G + 0.114 B),
//  the usual approximation of how bright a color looks to the eye: green
//  counts most, blue least.
//
//  Contributing
//  ------------
//  Keep shade() cheap: at full size it runs up to 19,200 times per frame
//  (240x80 cells for GBA games) when the whole screen changes.
// ============================================================================
#include "AsciiShader.h"

#include <cstring>

namespace term {

namespace {

// Ordered from emptiest to densest.  Ten steps are plenty for Game Boy
// pictures (4 shades) and still give GBA pictures smooth gradients.
const char kRamp[] = " .:-=+*#%@";

}  // namespace

const char* AsciiShader::ramp() { return kRamp; }

AsciiCell AsciiShader::shade(std::uint16_t top, std::uint16_t bottom, const ColorMapper& colors) {
    // Average the two pixels per RGB555 channel (rounding up).
    int r = ((top & 31) + (bottom & 31) + 1) >> 1;
    int g = (((top >> 5) & 31) + ((bottom >> 5) & 31) + 1) >> 1;
    int b = (((top >> 10) & 31) + ((bottom >> 10) & 31) + 1) >> 1;
    std::uint16_t average = static_cast<std::uint16_t>(r | (g << 5) | (b << 10));

    // Brightness of the color as it is displayed (after color correction).
    Rgb shown = colors.rgb(average);
    int luma = (299 * shown.r + 587 * shown.g + 114 * shown.b) / 1000;  // 0..255

    const int steps = static_cast<int>(std::strlen(kRamp));
    int index = (luma * (steps - 1) + 127) / 255;  // round to the nearest step

    AsciiCell cell;
    cell.glyph = kRamp[index];
    cell.color = average;
    return cell;
}

}  // namespace term
