// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/util/PngWriter.h
//  Layer:  Utilities
//
//  What this file does
//  -------------------
//  Declares a tiny PNG encoder that saves an emulator frame (gb::FrameBuffer, RGB555
//  pixels) as a 24-bit RGB PNG image. It exists so headless runs can produce screenshots
//  that can be compared or inspected without a terminal.
//
//  How it fits in
//  --------------
//  Called by term::HeadlessRunner (src/frontend/EmulatorSession.cpp) for
//  `--screenshot out.png`. Works for any frame size (160x144 Game Boy, 240x160 GBA).
//  Implementation: PngWriter.cpp (uses zlib for compression and CRC32).
//
//  Contributing
//  ------------
//  - The output is a plain, unfiltered, non-interlaced PNG; keep it dependency-light
//    (zlib only).
//  - Test: ./tboyemu --headless --frames 600 --screenshot shot.png game.zip, then open
//    shot.png in any image viewer.
// ============================================================================
#ifndef TBOYEMU_UTIL_PNGWRITER_H
#define TBOYEMU_UTIL_PNGWRITER_H

#include <string>

#include "../core/FrameBuffer.h"

namespace util {

// Minimal PNG encoder (zlib) used for screenshots / headless testing.
// Static-only, stateless.
class PngWriter {
public:
    // Writes `frame` to `path` (overwriting it). Returns false if compression fails or the
    // file cannot be written.
    static bool write(const std::string& path, const gb::FrameBuffer& frame);
};

}  // namespace util

#endif
