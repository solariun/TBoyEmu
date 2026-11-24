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
//  Declares term::ColorMapper, which turns an emulated frame (RGB555 pixels,
//  the native color format of the Game Boy Color and the GBA) into a
//  gfx::Image (24-bit RGB) for the Graphicx library, optionally applying the
//  Game Boy Color LCD color correction on the way.
//
//  How it fits in
//  --------------
//  EmulatorSession creates at most two mappers (with and without correction)
//  and picks one per game: correction only for Game Boy Color games when
//  enabled; original Game Boy games already use a hand-picked palette and GBA
//  colors are shown as they are.  TerminalRenderer (live screen), AnsiArt
//  (S key captures) and Recorder (movies) call toImage() once per frame and
//  hand the result to Graphicx (external/Graphicx), which does everything
//  else: resizing, the xterm-256 palette matching, half blocks / ASCII.
//
//  Terminal concepts
//  -----------------
//  A real Game Boy Color screen is much less saturated than raw RGB555
//  values on a modern display; the correction mixes the channels the way
//  that LCD does (the formula used by the Gambatte emulator), so games look
//  as intended.
//
//  Contributing
//  ------------
//  * The lookup table has one entry per RGB555 value (32768): building it
//    is instant and toImage() is a single table lookup per pixel.
//  * Test: ./tboyemu --no-color-correction <GBC rom> vs. the default.
// ============================================================================
#ifndef TBOYEMU_FRONTEND_COLORMAPPER_H
#define TBOYEMU_FRONTEND_COLORMAPPER_H

#include <graphicx/graphicx.h>

#include <cstdint>
#include <vector>

#include "../core/FrameBuffer.h"

namespace term {

// RGB555 -> 24-bit color conversion with optional CGB LCD color correction.
// Immutable after construction; cheap to share between renderers.
class ColorMapper {
public:
    // colorCorrection: true = mimic the Game Boy Color LCD (Gambatte curve),
    // false = plain 5-to-8-bit expansion of each channel.
    explicit ColorMapper(bool colorCorrection);

    // The displayed color of one RGB555 pixel (bit 15 ignored).
    gfx::Rgb rgb(std::uint16_t rgb555) const { return lut_[rgb555 & 0x7FFF]; }

    // Converts a whole frame into `image` (resized to the frame's size; reuse
    // one Image per renderer to avoid allocations).
    void toImage(const gb::FrameBuffer& frame, gfx::Image& image) const;

private:
    std::vector<gfx::Rgb> lut_;  // 32768 entries, indexed by RGB555
};

}  // namespace term

#endif
