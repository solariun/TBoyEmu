// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/Recorder.cpp
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Implementation of Recorder (movie recording) and MoviePlayer (--play),
//  see Recorder.h for the file format.
//
//  How it fits in
//  --------------
//  Recorder::addFrame() is called by EmulatorSession once per emulated
//  frame while recording: the frame is converted with the game's
//  ColorMapper and handed to a gfx::Screen (Graphicx), whose output - the
//  changed cells wrapped in a synchronized-update block - is appended to the
//  file as is.  MoviePlayer::play() is called by main.cpp.
//
//  Terminal concepts
//  -----------------
//  gfx::Screen re-sends colors at the start of every frame and ends each
//  frame with a reset (CSI 0m), so frames are independent of each other's
//  color state and can be played (or skipped) one by one.
//
//  Contributing
//  ------------
//  Recording runs once per emulated frame (59.73 Hz): keep addFrame()
//  cheap (one conversion, one Screen::draw, one file write).
// ============================================================================
#include "Recorder.h"

#include <chrono>
#include <cstdio>
#include <iterator>
#include <thread>
#include <vector>

#include "../core/Types.h"
#include "Keyboard.h"
#include "Terminal.h"

namespace term {

// --------------------------------------------------------------- Recorder ----

Recorder::Recorder() : colors_(0), frames_(0) {}

bool Recorder::start(const std::string& path, gfx::ColorMode colorMode, gfx::Style style, const ColorMapper* colors) {
    stop();
    file_.open(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file_) return false;
    path_ = path;
    colors_ = colors;
    frames_ = 0;

    gfx::Options options;  // columns = rows = 0: native size (1x2 pixels per cell)
    options.style = style;
    options.colorMode = colorMode;
    options.filter = gfx::Filter::Nearest;
    screen_.setOptions(options);  // the first frame is written in full
    screen_.setOrigin(1, 1);
    screen_.setSynchronized(true);  // each frame = one ?2026h ... ?2026l block

    file_ << "\x1b[0m\x1b[2J";
    return true;
}

void Recorder::stop() {
    if (!file_.is_open()) return;
    // Leave the cursor below the picture so a shell prompt after `cat`
    // does not overwrite it.
    file_ << "\x1b[0m\x1b[" << (screen_.rows() + 1) << ";1H\n";
    file_.close();
}

void Recorder::addFrame(const gb::FrameBuffer& frame) {
    if (!active() || !colors_) return;
    colors_->toImage(frame, image_);
    out_.clear();
    screen_.draw(image_, out_);
    file_.write(out_.data(), static_cast<std::streamsize>(out_.size()));
    ++frames_;
}

// ------------------------------------------------------------ MoviePlayer ----

int MoviePlayer::play(const std::string& path, bool kittyKeyboard) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot open %s\n", path.c_str());
        return 1;
    }
    std::string movie((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Split into frames: each one ends right after "CSI ?2026l".
    std::vector<std::string> frames;
    const std::string marker = "\x1b[?2026l";
    size_t start = 0;
    for (size_t at = movie.find(marker); at != std::string::npos; at = movie.find(marker, start)) {
        frames.push_back(movie.substr(start, at + marker.size() - start));
        start = at + marker.size();
    }
    if (start < movie.size()) frames.push_back(movie.substr(start));  // trailer

    typedef std::chrono::steady_clock Clock;
    const Clock::duration frameTime = std::chrono::duration_cast<Clock::duration>(
        std::chrono::nanoseconds(1000000000LL * gb::kDotsPerFrame / gb::kCpuClockHz));

    Terminal terminal(kittyKeyboard);
    Keyboard keyboard(terminal);
    std::vector<KeyEvent> events;
    Clock::time_point next = Clock::now();
    for (size_t i = 0; i < frames.size(); ++i) {
        terminal.write(frames[i]);
        events.clear();
        keyboard.poll(events, 0);
        for (size_t k = 0; k < events.size(); ++k) {
            const KeyEvent& e = events[k];
            if (e.action != KeyAction::Press) continue;
            if (e.key == Key::Escape || e.key == Key::CtrlC || (e.key == Key::Char && e.codepoint == 'q')) {
                return 0;
            }
        }
        if (Terminal::terminateRequested()) return 0;
        next += frameTime;
        std::this_thread::sleep_until(next);
    }

    terminal.write("\x1b[0m\r\n\x1b[7m End of movie - press any key \x1b[0m");
    for (;;) {
        events.clear();
        keyboard.poll(events, 200);
        if (Terminal::terminateRequested()) break;
        bool done = false;
        for (size_t k = 0; k < events.size(); ++k) {
            if (events[k].action == KeyAction::Press && events[k].key != Key::FocusIn &&
                events[k].key != Key::FocusOut) {
                done = true;
            }
        }
        if (done) break;
    }
    return 0;
}

}  // namespace term
