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
//  escape sequences in a .txt file (R key in a game). Every emulated frame
//  is appended as the cells that changed since the previous frame, drawn in
//  the session's display style (FULL half blocks, ASCII-COLOR or ASCII-BW).
//  Also declares MoviePlayer, which plays such a file back at the original
//  59.73 frames per second (./tboyemu --play movie.txt).
//
//  How it fits in
//  --------------
//  EmulatorSession owns a Recorder: R toggles start()/stop(), and after
//  every emulated frame it calls addFrame(). The live screen is drawn
//  independently by TerminalRenderer; the movie always uses the picture's
//  native size (160x72 cells for Game Boy, 240x80 for GBA), so it does not
//  depend on the size of the terminal it was recorded in. Cell encoding
//  reuses AnsiArt (colors) and AsciiShader (ASCII characters).
//  main.cpp runs MoviePlayer for --play.
//
//  Terminal concepts / file format
//  -------------------------------
//  The file is plain terminal output, so `cat movie.txt` also shows it
//  (as fast as the terminal can draw). Layout:
//    CSI 0m CSI 2J                      reset colors, clear the screen
//    then one block per frame:
//      CSI ?2026h                       begin synchronized update
//      CSI row;col H <colors> <chars>   only the cells that changed
//      CSI 0m CSI ?2026l                end of frame
//    CSI <rows+1>;1H                    leave the cursor below the picture
//  The first frame contains every cell. MoviePlayer splits the file at each
//  "CSI ?2026l" and writes one frame per 16.74 ms.
//
//  Contributing
//  ------------
//  * Keep every frame terminated by "\x1b[?2026l": the player relies on it.
//  * Test: start a game, press R, play a few seconds, press R again, then
//    ./tboyemu --play "<rom>-<date>-movie.txt" (or cat it).
// ============================================================================
#ifndef TBOYEMU_FRONTEND_RECORDER_H
#define TBOYEMU_FRONTEND_RECORDER_H

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "../core/FrameBuffer.h"
#include "AsciiShader.h"
#include "ColorMapper.h"
#include "Terminal.h"
#include "TerminalRenderer.h"

namespace term {

// Writes a movie file of escape sequences, one frame at a time.
// Single use per recording: start() ... addFrame() x N ... stop().
class Recorder {
public:
    Recorder();
    ~Recorder() { stop(); }

    // Opens `path` (overwritten) and starts recording with the given color
    // mode, display style and color mapping (not owned; must outlive the
    // recording). Returns false if the file cannot be created.
    bool start(const std::string& path, ColorMode mode, RenderStyle style, const ColorMapper* colors);
    // Finishes and closes the file. Safe to call when not recording.
    void stop();

    bool active() const { return file_.is_open(); }
    const std::string& path() const { return path_; }
    unsigned long frames() const { return frames_; }

    // Appends one frame: the cells that differ from the previous frame.
    void addFrame(const gb::FrameBuffer& frame);

private:
    // Packs what a cell shows (character + colors) for the frame diff.
    std::uint64_t cellKey(const std::uint16_t* top, const std::uint16_t* bottom, int x) const;

    std::ofstream file_;
    std::string path_;
    ColorMode mode_;
    RenderStyle style_;
    const ColorMapper* colors_;
    int width_, rows_;                    // picture size in cells
    std::vector<std::uint64_t> previous_;  // last written cell keys
    unsigned long frames_;
    std::string out_;                      // reused frame buffer
};

// Plays a movie file at 59.73 frames per second.
class MoviePlayer {
public:
    // Returns 0 on success; prints an error and returns 1 if the file
    // cannot be read. Esc, q or Ctrl+C stops; at the end it waits for a key.
    static int play(const std::string& path, bool kittyKeyboard);
};

}  // namespace term

#endif
