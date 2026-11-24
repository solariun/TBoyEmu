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
//  into terminal text in the session's display style and optionally saves
//  it to a .txt file.  Printing the file in a terminal (`cat shot.txt`)
//  shows the picture.
//
//  How it fits in
//  --------------
//  EmulatorSession calls AnsiArt::save() when the player presses S (file
//  name "<rom name>-YYYYMMDD-HHMMSS.txt"); HeadlessRunner calls it for
//  --screenshot out.txt.  The frame goes through the same ColorMapper as the
//  live screen and is rendered by gfx::render() from the Graphicx library
//  (external/Graphicx), so the file looks like the screen.
//
//  Terminal concepts
//  -----------------
//  Always at native size (160x144 -> 72 lines of 160 cells, 240x160 -> 80
//  lines of 240 cells), with no cursor movement: lines end with "\n" and,
//  in the colored styles, with "CSI 0 m" (reset) so colors do not leak.
//  The plain ASCII style contains no escape sequences at all.
//
//  Contributing
//  ------------
//  * The output must stay self-contained text (no cursor positioning, no
//    screen clearing), so it can be cat'ed, diffed or embedded anywhere.
//  * Test: ./tboyemu --headless --frames 600 --screenshot shot.txt game.zip
//    then `cat shot.txt` (also with --ascii, --ascii-color, --truecolor).
// ============================================================================
#ifndef TBOYEMU_FRONTEND_ANSIART_H
#define TBOYEMU_FRONTEND_ANSIART_H

#include <graphicx/graphicx.h>

#include <string>

#include "../core/FrameBuffer.h"
#include "ColorMapper.h"

namespace term {

// Frame -> terminal text (and file).  Only static functions; no state.
class AnsiArt {
public:
    // Returns the text for `frame` (UTF-8, lines ending in "\n").
    // colors converts the RGB555 pixels; colorMode picks palette or 24-bit
    // codes; style: half blocks (default), colored ASCII or plain ASCII.
    // contrast / brightness: picture adjustments (0.10 = +10%), see gfx::Options.
    static std::string encode(const gb::FrameBuffer& frame, const ColorMapper& colors, gfx::ColorMode colorMode,
                              gfx::Style style = gfx::Style::Blocks, double contrast = 0, double brightness = 0);
    // encode() + write to `path` (overwritten if it exists).
    // Returns false if the file cannot be opened or written.
    static bool save(const std::string& path, const gb::FrameBuffer& frame, const ColorMapper& colors,
                     gfx::ColorMode colorMode, gfx::Style style = gfx::Style::Blocks, double contrast = 0,
                     double brightness = 0);
};

}  // namespace term

#endif
