// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/AnsiArt.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Declares term::AnsiArt, a stateless helper that turns one emulated frame
//  into "ANSI art": plain text made of "▀" (upper half block) characters and
//  SGR color escape sequences, and optionally saves it to a .txt file.
//  Printing the file in a color terminal (`cat shot.txt`) shows the picture.
//
//  How it fits in
//  --------------
//  EmulatorSession calls AnsiArt::save() when the player presses S (file
//  name "<rom name>-YYYYMMDD-HHMMSS.txt"); HeadlessRunner calls it for
//  --screenshot out.txt. It uses the same ColorMapper and ColorMode as the
//  live TerminalRenderer, so the file looks like the screen.
//
//  Terminal concepts
//  -----------------
//  Same half-block encoding as TerminalRenderer (one cell = top pixel in the
//  foreground color + bottom pixel in the background color), but always at
//  native size (160x144 -> 72 lines of 160 cells, 240x160 -> 80 lines of
//  240 cells), with no cursor movement and no diffing: lines are separated
//  by "\n" and each one ends with "CSI 0 m" (reset) so colors do not leak
//  into the rest of the terminal line.
//
//  Contributing
//  ------------
//  * The output must stay self-contained text: no cursor positioning, no
//    screen clearing, so it can be cat'ed, diffed or embedded anywhere.
//  * Test: ./tboyemu --headless --frames 600 --screenshot shot.txt game.zip
//    then `cat shot.txt` (also with --truecolor).
// ============================================================================
#ifndef TBOYEMU_FRONTEND_ANSIART_H
#define TBOYEMU_FRONTEND_ANSIART_H

#include <string>

#include "../core/FrameBuffer.h"
#include "ColorMapper.h"
#include "TerminalRenderer.h"

namespace term {

// Encodes a full frame as terminal text (160x144 -> 72 lines of 160 cells,
// 240x160 -> 80 lines of 240): "upper half block" cells with ANSI color escapes (xterm-256 or 24-bit).
// `cat file.txt` in a color terminal shows the picture again.
// Only static functions; there is no state.
class AnsiArt {
public:
    // Returns the escaped text for `frame` (UTF-8, lines ending in "\n").
    // colors converts the RGB555 pixels; mode picks palette or 24-bit codes.
    static std::string encode(const gb::FrameBuffer& frame, const ColorMapper& colors, ColorMode mode);
    // encode() + write to `path` (overwritten if it exists).
    // Returns false if the file cannot be opened or written.
    static bool save(const std::string& path, const gb::FrameBuffer& frame, const ColorMapper& colors,
                     ColorMode mode);
};

}  // namespace term

#endif
