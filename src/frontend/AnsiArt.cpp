// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/AnsiArt.cpp
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Implementation of AnsiArt (see AnsiArt.h): converts the frame to a
//  gfx::Image with the game's ColorMapper and lets gfx::render() (Graphicx)
//  produce the text at native size, then writes it to a file.
//
//  How it fits in
//  --------------
//  Called on demand (S key, --screenshot), never per frame, so simplicity
//  wins over reusing buffers here.
//
//  Terminal concepts
//  -----------------
//  gfx::Filter::Nearest with the grid equal to the native size maps every
//  cell to exactly one pair of source pixels: the capture is lossless with
//  respect to what the terminal can show.
//
//  Contributing
//  ------------
//  The file is written in binary mode so the bytes are exactly what
//  gfx::render() produced (no newline conversion on any platform).
// ============================================================================
#include "AnsiArt.h"

#include <fstream>

namespace term {

std::string AnsiArt::encode(const gb::FrameBuffer& frame, const ColorMapper& colors, gfx::ColorMode colorMode,
                            gfx::Style style, double contrast, double brightness) {
    gfx::Image image;
    colors.toImage(frame, image);
    gfx::Options options;  // columns = rows = 0: native size, 1x2 pixels per cell
    options.style = style;
    options.colorMode = colorMode;
    options.filter = gfx::Filter::Nearest;
    options.contrast = contrast;
    options.brightness = brightness;
    return gfx::render(image, options);
}

bool AnsiArt::save(const std::string& path, const gb::FrameBuffer& frame, const ColorMapper& colors,
                   gfx::ColorMode colorMode, gfx::Style style, double contrast, double brightness) {
    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    std::string text = encode(frame, colors, colorMode, style, contrast, brightness);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    // The stream's state tells whether every byte was written.
    return static_cast<bool>(file);
}

}  // namespace term
