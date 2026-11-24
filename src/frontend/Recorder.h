// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/Recorder.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Declares Recorder, which records gameplay as a "movie" made of terminal
//  escape sequences in a .txt file (R key in a game).  Every emulated frame
//  is appended as the cells that changed since the previous frame, drawn in
//  the session's display style (FULL half blocks, ASCII-COLOR or ASCII-BW).
//  Also declares MoviePlayer, which plays such a file back at the original
//  59.73 frames per second (./tboyemu --play movie.txt).
//
//  How it fits in
//  --------------
//  EmulatorSession owns a Recorder: R toggles start()/stop(), and after
//  every emulated frame it calls addFrame().  The frames are encoded by a
//  gfx::Screen from the Graphicx library (external/Graphicx) placed at row
//  1, column 1, at the picture's native size (160x72 cells for Game Boy,
//  240x80 for GBA), so a movie does not depend on the size of the terminal
//  it was recorded in.  main.cpp runs MoviePlayer for --play.
//
//  Terminal concepts / file format
//  -------------------------------
//  The file is plain terminal output, so `cat movie.txt` also shows it (as
//  fast as the terminal can draw).  Layout:
//    CSI 0m CSI 2J                      reset colors, clear the screen
//    then one block per frame (exactly what gfx::Screen::draw returns):
//      CSI ?2026h                       begin synchronized update
//      CSI row;col H <colors> <chars>   only the cells that changed
//      CSI 0m CSI ?2026l                end of frame
//    CSI 0m CSI <rows+1>;1H \n          leave the cursor below the picture
//  The first frame contains every cell.  MoviePlayer splits the file at each
//  "CSI ?2026l" and writes one frame per 16.74 ms.
//
//  Contributing
//  ------------
//  * Every frame must end with "\x1b[?2026l": the player relies on it (keep
//    gfx::Screen's synchronized output on).
//  * Test: start a game, press R, play a few seconds, press R again, then
//    ./tboyemu --play "<rom>-<date>-movie.txt" (or cat it).
// ============================================================================
#ifndef TBOYEMU_FRONTEND_RECORDER_H
#define TBOYEMU_FRONTEND_RECORDER_H

#include <graphicx/graphicx.h>

#include <fstream>
#include <string>

#include "../core/FrameBuffer.h"
#include "ColorMapper.h"

namespace term {

// Writes a movie file of escape sequences, one frame at a time.
// Single use per recording: start() ... addFrame() x N ... stop().
class Recorder {
public:
    Recorder();
    ~Recorder() { stop(); }

    // Opens `path` (overwritten) and starts recording with the given color
    // mode, display style and color mapping (not owned; must outlive the
    // recording).  Returns false if the file cannot be created.
    bool start(const std::string& path, gfx::ColorMode colorMode, gfx::Style style, const ColorMapper* colors);
    // Finishes and closes the file.  Safe to call when not recording.
    void stop();
    // Picture adjustments for the following frames (0.10 = +10%).
    void setAdjustments(double contrast, double brightness);

    bool active() const { return file_.is_open(); }
    const std::string& path() const { return path_; }
    unsigned long frames() const { return frames_; }

    // Appends one frame: the cells that differ from the previous frame.
    void addFrame(const gb::FrameBuffer& frame);

private:
    std::ofstream file_;
    std::string path_;
    const ColorMapper* colors_;
    gfx::Screen screen_;     // encoder: native size, origin (1, 1), synchronized
    gfx::Image image_;       // the frame in 24-bit color (reused)
    unsigned long frames_;
    std::string out_;        // reused frame buffer
};

// Plays a movie file at 59.73 frames per second.
class MoviePlayer {
public:
    // Returns 0 on success; prints an error and returns 1 if the file
    // cannot be read.  Esc, q or Ctrl+C stops; at the end it waits for a key.
    static int play(const std::string& path, bool kittyKeyboard);
};

}  // namespace term

#endif
