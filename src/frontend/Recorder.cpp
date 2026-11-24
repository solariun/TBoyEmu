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
//  frame while recording; MoviePlayer::play() is called by main.cpp.
//
//  Terminal concepts
//  -----------------
//  Each recorded frame is a diff: only changed cells are written, each
//  preceded by a cursor move (CSI row;col H) when it does not directly
//  follow the previous cell. Colors are re-sent at the start of every frame
//  because every frame ends with a reset (CSI 0m), which also makes the
//  frames independent of each other's color state.
//
//  Contributing
//  ------------
//  Recording runs once per emulated frame (59.73 Hz): keep addFrame() cheap
//  (one pass over the cells, a single file write).
// ============================================================================
#include "Recorder.h"

#include <chrono>
#include <cstdio>
#include <iterator>
#include <thread>

#include "../core/Types.h"
#include "AnsiArt.h"
#include "Keyboard.h"

namespace term {

namespace {

const char kFrameEnd[] = "\x1b[0m\x1b[?2026l";
const std::uint64_t kInvalid = ~0ull;

void appendMove(std::string& out, int row, int col) {
    out += "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}

}  // namespace

// --------------------------------------------------------------- Recorder ----

Recorder::Recorder()
    : mode_(ColorMode::Xterm256), style_(RenderStyle::Blocks), colors_(0), width_(0), rows_(0), frames_(0) {}

bool Recorder::start(const std::string& path, ColorMode mode, RenderStyle style, const ColorMapper* colors) {
    stop();
    file_.open(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file_) return false;
    path_ = path;
    mode_ = mode;
    style_ = style;
    colors_ = colors;
    frames_ = 0;
    previous_.clear();  // the first frame is written in full
    width_ = rows_ = 0;
    file_ << "\x1b[0m\x1b[2J";
    return true;
}

void Recorder::stop() {
    if (!file_.is_open()) return;
    // Leave the cursor below the picture so a shell prompt after `cat`
    // does not overwrite it.
    std::string tail = "\x1b[0m";
    appendMove(tail, rows_ + 1, 1);
    tail += "\n";
    file_ << tail;
    file_.close();
}

std::uint64_t Recorder::cellKey(const std::uint16_t* top, const std::uint16_t* bottom, int x) const {
    if (style_ == RenderStyle::Blocks) {
        std::uint64_t fg = AnsiArt::colorKey(top[x], *colors_, mode_);
        std::uint64_t bg = AnsiArt::colorKey(bottom[x], *colors_, mode_);
        return (fg << 24) | bg;  // keys are at most 24 bits
    }
    AsciiCell c = AsciiShader::shade(top[x], bottom[x], *colors_);
    std::uint64_t fg = style_ == RenderStyle::AsciiColor ? AnsiArt::colorKey(c.color, *colors_, mode_) : 0;
    return (1ull << 62) | (fg << 8) | static_cast<unsigned char>(c.glyph);
}

void Recorder::addFrame(const gb::FrameBuffer& frame) {
    if (!active() || !colors_) return;
    const int width = frame.width();
    const int rows = frame.height() / 2;
    if (width != width_ || rows != rows_) {
        width_ = width;
        rows_ = rows;
        previous_.assign(static_cast<size_t>(width * rows), kInvalid);
    }

    out_.clear();
    out_ += "\x1b[?2026h";
    bool haveFg = false, haveBg = false;
    std::uint32_t curFg = 0, curBg = 0;
    int cursorRow = -1, cursorCol = -1;

    for (int y = 0; y < rows; ++y) {
        const std::uint16_t* top = frame.row(y * 2);
        const std::uint16_t* bottom = frame.row(y * 2 + 1);
        for (int x = 0; x < width; ++x) {
            std::uint64_t key = cellKey(top, bottom, x);
            std::uint64_t& prev = previous_[static_cast<size_t>(y * width + x)];
            if (prev == key) continue;
            prev = key;

            if (y + 1 != cursorRow || x + 1 != cursorCol) appendMove(out_, y + 1, x + 1);
            cursorRow = y + 1;
            cursorCol = x + 2;

            if (style_ == RenderStyle::Blocks) {
                std::uint32_t fg = AnsiArt::colorKey(top[x], *colors_, mode_);
                std::uint32_t bg = AnsiArt::colorKey(bottom[x], *colors_, mode_);
                if (!haveBg || curBg != bg) {
                    AnsiArt::appendColor(out_, bg, false, mode_);
                    curBg = bg;
                    haveBg = true;
                }
                if (fg == bg) {
                    out_ += ' ';  // solid cell: background only
                    continue;
                }
                if (!haveFg || curFg != fg) {
                    AnsiArt::appendColor(out_, fg, true, mode_);
                    curFg = fg;
                    haveFg = true;
                }
                out_ += AnsiArt::upperHalf();
            } else {
                AsciiCell c = AsciiShader::shade(top[x], bottom[x], *colors_);
                if (style_ == RenderStyle::AsciiColor && c.glyph != ' ') {
                    std::uint32_t fg = AnsiArt::colorKey(c.color, *colors_, mode_);
                    if (!haveFg || curFg != fg) {
                        AnsiArt::appendColor(out_, fg, true, mode_);
                        curFg = fg;
                        haveFg = true;
                    }
                }
                out_ += c.glyph;
            }
        }
    }
    out_ += kFrameEnd;
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
