// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/TerminalRenderer.cpp
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Implements TerminalRenderer (see TerminalRenderer.h): layout and scaling,
//  the per-cell diff against the previous frame, and the generation of the
//  escape-sequence stream for one frame (cursor moves, SGR color changes,
//  "▀" characters, status bar), wrapped in a synchronized-output block.
//
//  How it fits in
//  --------------
//  Called by EmulatorSession::run() once per drawn frame. Reads pixels from
//  gb::FrameBuffer (RGB555), converts them with ColorMapper, and writes the
//  result with a single Terminal::write().
//
//  Terminal concepts
//  -----------------
//  Escape sequences emitted (ESC = 0x1B, CSI = "ESC ["):
//    CSI row ; col H      move the cursor (1-based row and column)
//    CSI 38;5;n m         foreground = xterm palette index n
//    CSI 48;5;n m         background = xterm palette index n
//    CSI 38;2;r;g;b m     foreground = 24-bit color (truecolor)
//    CSI 48;2;r;g;b m     background = 24-bit color (truecolor)
//    CSI 49 m             background = terminal default
//    CSI 0 m              reset all attributes      CSI 7 m  reverse video
//    CSI 2 J              erase screen              CSI 2 K  erase line
//    CSI ? 2026 h / l     begin / end synchronized update
//  Each cell is written as "▀" (UTF-8 E2 96 80): top pixel = foreground,
//  bottom pixel = background. If both pixels have the same color a plain
//  space with only a background color is written instead (fewer bytes).
//  Writing a character advances the cursor one column, so consecutive
//  changed cells need no cursor move in between; auto-wrap is disabled by
//  Terminal, so writing the last column never scrolls.
//  SGR state persists between cells, so colors are only re-sent when they
//  differ from the last ones written (curFg / curBg).
//
//  Contributing
//  ------------
//  * Any code path that writes something without updating previous_ must
//    leave haveColors / cursorRow consistent, or later cells get the wrong
//    color or position.
//  * Performance: avoid std::to_string / ostringstream in the per-cell loop;
//    appendInt() exists for that reason.
//  * Test: play a game with lots of scrolling (e.g. a platformer) and watch
//    the "drawn" fps in the status bar; resize the window while playing.
// ============================================================================
#include "TerminalRenderer.h"

#include <algorithm>

namespace term {

namespace {

const char kUpperHalf[] = "\xE2\x96\x80";  // U+2580
// "Upper half block" in UTF-8. Foreground paints the top half of the cell.
// Marker for "cell content unknown"; no real cell key can be all ones
// because bits 24-30 of the low word are always zero.
const std::uint64_t kInvalid = ~0ull;

// Appends a non-negative decimal integer to `out` without allocating
// (faster than std::to_string in the per-cell loop).
void appendInt(std::string& out, int v) {
    char buf[12];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0) {
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    while (n > 0) out.push_back(buf[--n]);
}

// Appends "CSI row ; col H" (cursor position, both 1-based).
void moveTo(std::string& out, int row, int col) {
    out += "\x1b[";
    appendInt(out, row);
    out.push_back(';');
    appendInt(out, col);
    out.push_back('H');
}

}  // namespace

TerminalRenderer::TerminalRenderer(Terminal& terminal, ColorMode mode, double maxScale, RenderStyle style)
    : terminal_(terminal),
      mode_(mode),
      style_(style),
      maxScale_(maxScale > 0 ? maxScale : 1.0),
      mapper_(0),
      cols_(0),
      rows_(0),
      frameWidth_(gb::kScreenWidth),
      frameHeight_(gb::kScreenHeight),
      outCols_(0),
      outRows_(0),
      originX_(1),
      originY_(1),
      hasStatusRow_(false),
      tooSmall_(false) {
    // A full truecolor frame can be a few hundred KB; reserving 1 MB once
    // avoids reallocations in present().
    out_.reserve(1 << 20);
    relayout();
}

void TerminalRenderer::setColorMapper(const ColorMapper* mapper) {
    mapper_ = mapper;
    invalidate();
}

void TerminalRenderer::relayout() {
    Size s = terminal_.size();
    cols_ = s.cols;
    rows_ = s.rows;

    // Scale factor = largest that fits: horizontally 1 pixel per column,
    // vertically 2 pixels per row (half blocks), capped by --scale.
    // Example: 160x144 in a 200x50 terminal -> min(1.25, 0.69) = 0.69.
    double scale = std::min(static_cast<double>(cols_) / frameWidth_,
                            static_cast<double>(rows_ * 2) / frameHeight_);
    scale = std::min(scale, maxScale_);
    // Picture size in pixels after scaling.
    int pixW = static_cast<int>(frameWidth_ * scale);
    int pixH = static_cast<int>(frameHeight_ * scale);
    // Below 32x28 pixels the picture is useless; show a message instead.
    tooSmall_ = pixW < 32 || pixH < 28;

    // Picture size in cells: one column per pixel, one row per 2 pixels
    // (rounded up; an odd height leaves the last row half empty).
    outCols_ = std::max(1, pixW);
    outRows_ = std::max(1, (pixH + 1) / 2);
    // The status bar uses the last terminal row, but only if the picture
    // leaves it free; at exact native size (e.g. 72 rows) it is hidden.
    hasStatusRow_ = outRows_ < rows_;
    int usableRows = hasStatusRow_ ? rows_ - 1 : rows_;
    // Center the picture (terminal coordinates are 1-based).
    originX_ = 1 + std::max(0, (cols_ - outCols_) / 2);
    originY_ = 1 + std::max(0, (usableRows - outRows_) / 2);

    // Nearest-neighbour sampling: for every output column / half row, the
    // source pixel it shows. Computed once here, not per frame.
    srcX_.resize(static_cast<size_t>(outCols_));
    for (int x = 0; x < outCols_; ++x) {
        srcX_[static_cast<size_t>(x)] = std::min(frameWidth_ - 1, static_cast<int>(x / scale));
    }
    srcTop_.resize(static_cast<size_t>(outRows_));
    srcBottom_.resize(static_cast<size_t>(outRows_));
    for (int y = 0; y < outRows_; ++y) {
        int top = 2 * y;
        int bottom = 2 * y + 1;
        srcTop_[static_cast<size_t>(y)] = std::min(frameHeight_ - 1, static_cast<int>(top / scale));
        // -1: the bottom half of the last row is outside the picture.
        srcBottom_[static_cast<size_t>(y)] =
            bottom < pixH ? std::min(frameHeight_ - 1, static_cast<int>(bottom / scale)) : -1;
    }
    invalidate();
}

void TerminalRenderer::invalidate() {
    // Mark every cell unknown so the next present() redraws all of them, and
    // forget the status bar; then reset colors and erase the screen.
    previous_.assign(static_cast<size_t>(outCols_ * outRows_), kInvalid);
    lastStatus_.clear();
    terminal_.write("\x1b[0m\x1b[2J");
}

std::uint32_t TerminalRenderer::colorKey(std::uint16_t c) const {
    if (mode_ == ColorMode::TrueColor) {
        // 0x00RRGGBB
        Rgb rgb = mapper_->rgb(c);
        return (static_cast<std::uint32_t>(rgb.r) << 16) | (static_cast<std::uint32_t>(rgb.g) << 8) | rgb.b;
    }
    // Palette index 16-255
    return mapper_->xterm(c);
}

void TerminalRenderer::appendColor(std::string& out, std::uint32_t key, bool foreground) const {
    // SGR 38 = set foreground, 48 = set background; sub-mode 2 = RGB,
    // 5 = palette index. Example: "\x1b[48;5;231m" = white background.
    out += foreground ? "\x1b[38;" : "\x1b[48;";
    if (mode_ == ColorMode::TrueColor) {
        out += "2;";
        appendInt(out, static_cast<int>((key >> 16) & 0xFF));
        out.push_back(';');
        appendInt(out, static_cast<int>((key >> 8) & 0xFF));
        out.push_back(';');
        appendInt(out, static_cast<int>(key & 0xFF));
    } else {
        out += "5;";
        appendInt(out, static_cast<int>(key));
    }
    out.push_back('m');
}

void TerminalRenderer::appendFg(std::string& out, std::uint32_t key) const { appendColor(out, key, true); }
void TerminalRenderer::appendBg(std::string& out, std::uint32_t key) const { appendColor(out, key, false); }

void TerminalRenderer::drawStatus(std::string& out, const std::string& status) {
    // Only redraw when the text changed (it changes about once per second).
    if (!hasStatusRow_ || status == lastStatus_) return;
    lastStatus_ = status;
    // Truncate to the terminal width. This counts bytes, not characters,
    // which is exact for ASCII text (a non-ASCII file name in a flash message
    // could be cut in the middle of a UTF-8 character).
    std::string line = status.substr(0, static_cast<size_t>(cols_));
    moveTo(out, rows_, 1);
    // Reset, erase the whole line (CSI 2 K), switch to reverse video (CSI 7 m).
    out += "\x1b[0m\x1b[2K\x1b[7m";
    out += line;
    out += "\x1b[0m";
}

void TerminalRenderer::present(const gb::FrameBuffer& frame, const std::string& status) {
    if (!mapper_) return;
    // GB frames are 160x144 and GBA frames 240x160; a change means a different
    // console, so the layout must be recomputed.
    if (frame.width() != frameWidth_ || frame.height() != frameHeight_) {
        frameWidth_ = frame.width();
        frameHeight_ = frame.height();
        relayout();
    }
    out_.clear();
    out_ += "\x1b[?2026h";  // begin synchronized update (ignored if unsupported)

    if (tooSmall_) {
        // lastStatus_ = "\x01" is a sentinel meaning "the too-small message is
        // already on screen", so the message is written only once.
        if (lastStatus_ != "\x01") {
            lastStatus_ = "\x01";
            out_ += "\x1b[0m\x1b[2J\x1b[1;1HTerminal too small - enlarge it";
        }
        out_ += "\x1b[?2026l";
        terminal_.write(out_);
        return;
    }

    // Current SGR / cursor state of the terminal as far as this frame knows.
    // haveColors = false: the colors on the terminal are unknown, so the next
    // cell must send both. cursorRow/Col = -1: the cursor position is unknown.
    bool haveColors = false;
    std::uint32_t curFg = 0, curBg = 0;
    int cursorRow = -1, cursorCol = -1;

    if (style_ != RenderStyle::Blocks) {
        presentAscii(frame);
        out_ += "\x1b[0m";
        drawStatus(out_, status);
        out_ += "\x1b[?2026l";
        terminal_.write(out_);
        return;
    }

    for (int y = 0; y < outRows_; ++y) {
        // Source rows for the top and bottom half of this cell row.
        const std::uint16_t* top = frame.row(srcTop_[static_cast<size_t>(y)]);
        int bottomRow = srcBottom_[static_cast<size_t>(y)];
        const std::uint16_t* bottom = bottomRow >= 0 ? frame.row(bottomRow) : 0;

        for (int x = 0; x < outCols_; ++x) {
            int sx = srcX_[static_cast<size_t>(x)];
            std::uint32_t fg = colorKey(top[sx]);
            std::uint32_t bg = bottom ? colorKey(bottom[sx]) : 0;
            // Terminal default background below the last half row.
            // Cell key: fg in the high 32 bits, bg in the low ones, and bit 31
            // set for a cell without a bottom pixel (no color key uses bit 31).
            std::uint64_t cell = (static_cast<std::uint64_t>(fg) << 32) | bg | (bottom ? 0 : (1ull << 31));
            // The diff: skip cells that already show exactly this.
            std::uint64_t& prev = previous_[static_cast<size_t>(y * outCols_ + x)];
            if (prev == cell) continue;
            prev = cell;

            // Move the cursor only if it is not already right here (it is,
            // when the previous cell on this row was also written).
            int row = originY_ + y;
            int col = originX_ + x;
            if (row != cursorRow || col != cursorCol) moveTo(out_, row, col);

            if (!bottom) {
                // Half-empty last row: default background (CSI 49 m) under a
                // top half block. This changes the background state, so the
                // next cell re-sends its colors.
                out_ += "\x1b[49m";
                appendFg(out_, fg);
                out_ += kUpperHalf;
                haveColors = false;
            } else if (fg == bg) {
                // Solid cell: a space only needs the background color.
                if (!haveColors || curBg != bg) {
                    appendBg(out_, bg);
                    curBg = bg;
                    // The foreground was not set: mark it unknown (~0 never
                    // matches a real key), so the next "▀" cell sends it.
                    if (!haveColors) curFg = ~0u;
                    haveColors = true;
                }
                out_.push_back(' ');
            } else {
                // Two different pixels: "▀" with fg = top, bg = bottom,
                // sending only the color(s) that changed.
                if (!haveColors || curFg != fg) appendFg(out_, fg);
                if (!haveColors || curBg != bg) appendBg(out_, bg);
                curFg = fg;
                curBg = bg;
                haveColors = true;
                out_ += kUpperHalf;
            }
            // Writing one character advanced the cursor by one column.
            cursorRow = row;
            cursorCol = col + 1;
        }
    }

    // Reset colors so the status bar / later output starts clean.
    out_ += "\x1b[0m";
    drawStatus(out_, status);
    out_ += "\x1b[?2026l";  // end synchronized update
    // One write per frame: fewer syscalls and the terminal gets the whole
    // synchronized block at once.
    terminal_.write(out_);
}

// ASCII styles (--ascii / --ascii-color): same layout and diff as the half
// blocks, but each cell becomes one character from AsciiShader.  No
// background color is ever sent, so the terminal's own background shows.
// Cell keys in previous_ get bit 62 set so they never equal a half-block key.
void TerminalRenderer::presentAscii(const gb::FrameBuffer& frame) {
    const bool colored = style_ == RenderStyle::AsciiColor;
    out_ += "\x1b[0m";  // start from the terminal's default colors
    bool haveFg = false;
    std::uint32_t curFg = 0;
    int cursorRow = -1, cursorCol = -1;

    for (int y = 0; y < outRows_; ++y) {
        const std::uint16_t* top = frame.row(srcTop_[static_cast<size_t>(y)]);
        int bottomRow = srcBottom_[static_cast<size_t>(y)];
        const std::uint16_t* bottom = bottomRow >= 0 ? frame.row(bottomRow) : top;

        for (int x = 0; x < outCols_; ++x) {
            int sx = srcX_[static_cast<size_t>(x)];
            AsciiCell c = AsciiShader::shade(top[sx], bottom[sx], *mapper_);
            std::uint32_t fg = colored ? colorKey(c.color) : 0;
            std::uint64_t cell = (1ull << 62) | (static_cast<std::uint64_t>(fg) << 8) |
                                 static_cast<unsigned char>(c.glyph);
            std::uint64_t& prev = previous_[static_cast<size_t>(y * outCols_ + x)];
            if (prev == cell) continue;
            prev = cell;

            int row = originY_ + y;
            int col = originX_ + x;
            if (row != cursorRow || col != cursorCol) moveTo(out_, row, col);
            // A space shows nothing, so its color does not matter.
            if (colored && c.glyph != ' ' && (!haveFg || curFg != fg)) {
                appendFg(out_, fg);
                curFg = fg;
                haveFg = true;
            }
            out_.push_back(c.glyph);
            cursorRow = row;
            cursorCol = col + 1;
        }
    }
}

}  // namespace term
