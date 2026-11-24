// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/TerminalRenderer.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Declares term::TerminalRenderer, which draws emulated frames (160x144 for
//  Game Boy / Game Boy Color, 240x160 for Game Boy Advance) into the terminal
//  using colored "upper half block" characters, plus a one-line status bar
//  at the bottom. Also declares ColorMode (xterm-256 or truecolor).
//
//  How it fits in
//  --------------
//  EmulatorSession creates one TerminalRenderer per game, gives it the right
//  ColorMapper, and calls present() for every frame it decides to draw. The
//  renderer writes its output through Terminal::write() in one call per
//  frame. AnsiArt reuses the same encoding idea to save frames to files.
//
//  Terminal concepts
//  -----------------
//  * Half-block rendering: a terminal cell is roughly twice as tall as it is
//    wide. The character U+2580 "▀" (UPPER HALF BLOCK) paints the top half of
//    the cell in the foreground color; the bottom half shows the background
//    color. So one cell = two vertically stacked, nearly square pixels, and
//    a 160x144 picture fits in 160 columns x 72 rows.
//  * Diffing: sending every cell every frame costs several hundred KB per
//    second. The renderer remembers what each cell shows and only sends the
//    cells that changed, with a cursor move (CSI row;col H) when the changed
//    cells are not adjacent.
//  * Synchronized output (DEC private mode 2026): "CSI ? 2026 h" asks the
//    terminal to buffer what follows and "CSI ? 2026 l" to show it all at
//    once, so a frame never appears half-drawn (tearing). Terminals that do
//    not know the mode ignore it.
//  * Scaling: if the terminal is smaller than the native size, the picture is
//    downscaled with nearest-neighbour sampling (srcX_/srcTop_/srcBottom_
//    tables); with --scale > 1 it may also grow on large terminals.
//
//  Contributing
//  ------------
//  * Anything that may change what the terminal shows outside present()
//    (resize, clear, another screen drawn over us) must call invalidate()
//    or relayout(), otherwise the diff skips cells that need redrawing.
//  * Keep the output small: bytes written per frame are the main
//    performance limit at 59.73 fps. The status bar shows "drawn" fps.
//  * Test by resizing the window while a game runs, with and without
//    --truecolor, and with a terminal smaller than 160x72.
// ============================================================================
#ifndef TBOYEMU_FRONTEND_TERMINALRENDERER_H
#define TBOYEMU_FRONTEND_TERMINALRENDERER_H

#include <cstdint>
#include <string>
#include <vector>

#include "../core/FrameBuffer.h"
#include "AsciiShader.h"
#include "ColorMapper.h"
#include "Terminal.h"

namespace term {

// How colors are sent to the terminal:
//   Xterm256   SGR 38;5;n / 48;5;n with a palette index (works almost everywhere)
//   TrueColor  SGR 38;2;r;g;b / 48;2;r;g;b with exact 24-bit RGB (--truecolor)
enum class ColorMode { Xterm256, TrueColor };

// Draws Game Boy / Game Boy Advance frames with "upper half block" characters: each terminal
// cell shows two vertical pixels (foreground = top, background = bottom), so
// the native picture needs 160x72 cells (Game Boy) or 240x80 (GBA).  Smaller terminals
// get a nearest-neighbour downscale.  Only cells that changed since the last
// frame are sent, inside a synchronized-update block, to keep the byte rate
// low enough for 60 fps.
//
// Lifetime: one per EmulatorSession::run() call; holds a reference to the
// Terminal and a non-owning pointer to a ColorMapper (owned by the session).
class TerminalRenderer {
public:
    // maxScale: upper limit of the scale factor (1.0 = native size at most;
    // values <= 0 are treated as 1.0). Computes the layout immediately.
    // style: half blocks (default) or one of the ASCII styles (AsciiShader.h);
    // the layout is the same for all styles (one cell = 1x2 source pixels).
    TerminalRenderer(Terminal& terminal, ColorMode mode, double maxScale,
                     RenderStyle style = RenderStyle::Blocks);

    // Selects the color conversion table (not owned; must outlive the
    // renderer). Forces a full redraw. present() draws nothing until set.
    void setColorMapper(const ColorMapper* mapper);
    void relayout();  // call after a terminal resize
                      // (re-reads the size, recomputes scale, position and
                      //  sampling tables, then invalidate()s)
    // Forgets what is on screen and clears it, so the next present() redraws
    // every cell and the status bar.
    void invalidate();
    // Draws one frame (only the changed cells) and, if there is a spare row,
    // the status text in reverse video on the last line. If the frame size
    // differs from the previous one (GB vs GBA), the layout is recomputed.
    void present(const gb::FrameBuffer& frame, const std::string& status);
    // Short tag shown in blue at the bottom left (e.g. "[RECORDING]"), in
    // front of the status text; empty = none.  When the terminal has no spare
    // status row it is drawn over the picture's last row instead.
    void setIndicator(const std::string& text) { indicator_ = text; }

private:
    // Packs a pixel's terminal color into 32 bits so that equal colors compare
    // equal: the palette index (Xterm256) or 0xRRGGBB (TrueColor).
    std::uint32_t colorKey(std::uint16_t rgb555) const;
    // Append the SGR sequence that sets the foreground / background color.
    void appendFg(std::string& out, std::uint32_t key) const;
    void appendBg(std::string& out, std::uint32_t key) const;
    void appendColor(std::string& out, std::uint32_t key, bool foreground) const;
    // Appends the status bar to `out` when it changed and a row is free.
    void drawStatus(std::string& out, const std::string& status);
    // Draws the changed cells for the ASCII styles into out_.
    void presentAscii(const gb::FrameBuffer& frame);

    Terminal& terminal_;          // output target
    ColorMode mode_;              // palette index or truecolor
    RenderStyle style_;           // half blocks, plain ASCII or colored ASCII
    double maxScale_;             // from --scale, > 0
    const ColorMapper* mapper_;   // not owned; null until setColorMapper()

    int cols_, rows_;  // terminal size in cells
    int frameWidth_, frameHeight_;  // source picture size (GB or GBA)
    int outCols_, outRows_;  // picture size in cells
    int originX_, originY_;  // 1-based terminal position of the picture
    bool hasStatusRow_;      // a row is left below the picture for the status bar
    bool tooSmall_;          // picture would be < 32x28 pixels: show a message
    // Nearest-neighbour sampling tables, rebuilt by relayout():
    std::vector<int> srcX_;       // per output column
    std::vector<int> srcTop_;     // per output row (top half)
    std::vector<int> srcBottom_;  // per output row (bottom half, -1 = none)
    // Diff state: one entry per cell (outCols_ * outRows_), row-major.
    // Layout: fg key << 32 | bg key, plus bit 31 when the cell has no bottom
    // pixel; ~0 (kInvalid in the .cpp) means "unknown, must redraw".
    std::vector<std::uint64_t> previous_;  // last drawn cell (fg/bg keys)
    std::string lastStatus_;
    std::string indicator_;  // see setIndicator()  // status text currently on screen ("" = redraw)
    std::string out_;         // output buffer reused across frames (1 MB reserved)
};

}  // namespace term

#endif
