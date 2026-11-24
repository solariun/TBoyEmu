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
//  Declares term::TerminalRenderer, which shows the emulated screen in the
//  terminal at 60 frames per second: it sizes and centers the picture for
//  the current window, draws it through the Graphicx library
//  (external/Graphicx) in the chosen style, and adds the status bar and an
//  optional indicator tag (e.g. "[RECORDING]").
//
//  How it fits in
//  --------------
//  EmulatorSession owns one per game and calls present() for every frame it
//  decides to draw.  The work is split:
//    TerminalRenderer  layout: scale (never above --scale), centering, the
//                      status row, "terminal too small", the indicator; and
//                      the synchronized-update wrapper around each frame.
//    ColorMapper       RGB555 frame -> gfx::Image (with CGB correction).
//    gfx::Screen       the picture itself: resizing (Nearest, crisp pixels),
//                      half blocks / ASCII / colored ASCII, xterm-256 or
//                      truecolor, and sending only the cells that changed.
//
//  Terminal concepts
//  -----------------
//  * Each cell shows 1x2 pixels, so a full-size picture needs 160x72 cells
//    (Game Boy) or 240x80 (GBA); smaller windows get a scaled-down picture.
//  * A frame is written as one block between "CSI ?2026h" and "CSI ?2026l"
//    (synchronized output), so terminals that support it never show a
//    half-drawn frame.
//  * The status bar uses reverse video (CSI 7 m) on the last terminal row
//    when the picture leaves that row free.
//
//  Contributing
//  ------------
//  * Picture drawing belongs in Graphicx; keep this class about layout and
//    TBoyEmu-specific overlays.
//  * Test all styles: ./tboyemu, ./tboyemu --ascii, ./tboyemu --ascii-color,
//    and resize the window while a game runs.
// ============================================================================
#ifndef TBOYEMU_FRONTEND_TERMINALRENDERER_H
#define TBOYEMU_FRONTEND_TERMINALRENDERER_H

#include <graphicx/graphicx.h>

#include <string>

#include "../core/FrameBuffer.h"
#include "ColorMapper.h"
#include "Terminal.h"

namespace term {

// Draws frames of the running game in the terminal (see the file header).
// One per EmulatorSession::run(); holds a reference to the Terminal and a
// non-owning pointer to a ColorMapper (owned by the session).
class TerminalRenderer {
public:
    // colorMode: xterm-256 palette or 24-bit truecolor.
    // maxScale: upper limit of the scale factor (1.0 = native size at most;
    // values <= 0 are treated as 1.0).
    // style: half blocks (default), colored ASCII or plain ASCII.
    TerminalRenderer(Terminal& terminal, gfx::ColorMode colorMode, double maxScale,
                     gfx::Style style = gfx::Style::Blocks);

    // Selects the color conversion (not owned; must outlive the renderer).
    // Forces a full redraw.  present() draws nothing until it is set.
    void setColorMapper(const ColorMapper* mapper);
    // Re-reads the terminal size and recomputes the layout (call after a
    // resize); clears the screen and repaints everything on the next frame.
    void relayout();
    // Clears the screen; the next present() repaints everything.
    void invalidate();
    // Draws one frame plus the status bar text.
    void present(const gb::FrameBuffer& frame, const std::string& status);
    // Short tag shown in blue at the bottom left (e.g. "[RECORDING]"), in
    // front of the status text; empty = none.  When the terminal has no spare
    // status row it is drawn over the picture's last row instead.
    void setIndicator(const std::string& text) { indicator_ = text; }
    // Picture adjustments (fractions: 0.10 = +10%), see gfx::Options; the
    // whole picture is redrawn with them on the next frame.
    void setAdjustments(double contrast, double brightness);

private:
    void drawStatus(std::string& out, const std::string& status);

    Terminal& terminal_;
    gfx::Options options_;   // style, colors, Nearest filter, grid size
    gfx::Screen screen_;     // draws the picture, remembers what is shown
    gfx::Image image_;       // the current frame in 24-bit color (reused)
    double maxScale_;
    const ColorMapper* mapper_;

    int cols_, rows_;                // terminal size
    int frameWidth_, frameHeight_;   // source picture size (GB or GBA)
    int outCols_, outRows_;          // picture size in cells
    int originX_, originY_;          // 1-based terminal position of the picture
    bool hasStatusRow_;              // a free row below the picture exists
    bool tooSmall_;                  // picture would be unrecognizably small
    std::string lastStatus_;         // status line last drawn (skip if same)
    std::string indicator_;          // see setIndicator()
    std::string out_;                // output buffer, reused every frame
};

}  // namespace term

#endif
