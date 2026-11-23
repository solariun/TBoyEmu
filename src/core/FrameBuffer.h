// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/FrameBuffer.h
//  Layer:  Game Boy core (shared with the Game Boy Advance core and the frontend)
//
//  What this file does
//  -------------------
//  Defines FrameBuffer, a simple 2D array of RGB555 pixels. It is the single
//  picture format exchanged between the emulated consoles and the terminal
//  frontend: the Game Boy PPU draws 160x144 frames into it, the Game Boy
//  Advance PPU draws 240x160 frames, and the frontend (ColorMapper,
//  TerminalRenderer, AnsiArt, the PNG writer) reads it.
//
//  How it fits in
//  --------------
//  Each PPU owns two FrameBuffers (a back buffer being drawn and a front
//  buffer holding the last completed frame) and exposes the front one through
//  Machine::frame(). The frontend only ever reads it, once per emulated frame.
//
//  Hardware notes
//  --------------
//  RGB555 (bits 0-4 red, 5-9 green, 10-14 blue, bit 15 unused) is the native
//  color format of both the Game Boy Color palette RAM (Pan Docs, "Palettes")
//  and the GBA (GBATEK, "LCD Color Palettes"). Original Game Boy shades are
//  converted to RGB555 by the Game Boy PPU before they are stored here.
//
//  Contributing
//  ------------
//  Keep this class trivial: it is accessed per pixel in hot loops. No bounds
//  checking is done; callers must pass 0 <= x < width and 0 <= y < height.
//  Any change affects both cores; test with
//      make && ./tboyemu --headless --frames 600 --screenshot shot.png game.zip
//  and the dmg-acid2 / cgb-acid2 test ROMs mentioned in README.md.
// ============================================================================
#ifndef TBOYEMU_CORE_FRAMEBUFFER_H
#define TBOYEMU_CORE_FRAMEBUFFER_H

#include <algorithm>
#include <vector>

#include "Types.h"

namespace gb {

// A video frame, one RGB555 color (0bbbbbgggggrrrrr) per pixel.  Both the
// Game Boy (160x144) and the Game Boy Advance (240x160) produce these.
// Pixels are stored row by row (row-major), top-left first. The buffer owns
// its pixel memory (a std::vector), is copyable/swappable (the PPUs swap
// front and back buffers with std::swap) and is only used from the single
// emulation thread.
class FrameBuffer {
public:
    // Creates a width x height frame filled with white (0x7FFF).
    FrameBuffer(int width, int height)
        : width_(width), height_(height), pixels_(static_cast<size_t>(width * height), 0x7FFF) {}

    // Size in pixels.
    int width() const { return width_; }
    int height() const { return height_; }
    // RGB555 color of the pixel at column x, row y (no bounds check).
    u16 at(int x, int y) const { return pixels_[static_cast<size_t>(y * width_ + x)]; }
    // Pointer to the first pixel of row y; the row holds width() pixels.
    // Used by the renderers to write a whole scanline efficiently.
    u16* row(int y) { return &pixels_[static_cast<size_t>(y * width_)]; }
    const u16* row(int y) const { return &pixels_[static_cast<size_t>(y * width_)]; }
    // Sets every pixel to the given RGB555 color (e.g. white when the LCD is off).
    void fill(u16 color) { std::fill(pixels_.begin(), pixels_.end(), color); }

private:
    // Size in pixels (pixels per row, number of rows).
    int width_;
    int height_;
    // width_ * height_ RGB555 values, row-major.
    std::vector<u16> pixels_;
};

}  // namespace gb

#endif
