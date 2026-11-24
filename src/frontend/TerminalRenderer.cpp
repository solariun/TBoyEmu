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
//  Implementation of TerminalRenderer (see TerminalRenderer.h): window
//  layout, the per-frame output block, status bar and indicator.  The
//  picture itself is drawn by gfx::Screen from the Graphicx library.
//
//  How it fits in
//  --------------
//  present() builds one output string per frame:
//      CSI ?2026h                      begin synchronized update
//      <gfx::Screen::draw output>      only the picture cells that changed
//      <status bar / indicator>        when they changed
//      CSI ?2026l                      end of update
//  and writes it to the terminal in one call.
//
//  Terminal concepts
//  -----------------
//  Layout: scale = min(columns / width, 2 * rows / height, maxScale); the
//  picture is centered horizontally and in the rows above the status row.
//  Pixel art is resized with gfx::Filter::Nearest so edges stay crisp.
//
//  Contributing
//  ------------
//  When something is drawn over the picture (the indicator without a status
//  row), tell gfx::Screen with invalidateCells() so those cells are
//  repainted once the overlay goes away.
// ============================================================================
#include "TerminalRenderer.h"

#include <algorithm>

namespace term {

namespace {

void moveTo(std::string& out, int row, int col) {
    out += "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}

// Indicator style: bold white on blue (xterm-256 color 21).
const char kIndicatorStyle[] = "\x1b[1;38;5;231;48;5;21m";

}  // namespace

TerminalRenderer::TerminalRenderer(Terminal& terminal, gfx::ColorMode colorMode, double maxScale, gfx::Style style)
    : terminal_(terminal),
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
    options_.style = style;
    options_.colorMode = colorMode;
    options_.filter = gfx::Filter::Nearest;  // pixel art: keep it crisp
    screen_.setSynchronized(false);          // present() wraps the whole frame
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

    double scale = std::min(static_cast<double>(cols_) / frameWidth_,
                            static_cast<double>(rows_ * 2) / frameHeight_);
    scale = std::min(scale, maxScale_);
    int pixW = static_cast<int>(frameWidth_ * scale);
    int pixH = static_cast<int>(frameHeight_ * scale);
    tooSmall_ = pixW < 32 || pixH < 28;

    outCols_ = std::max(1, pixW);
    outRows_ = std::max(1, (pixH + 1) / 2);
    hasStatusRow_ = outRows_ < rows_;
    int usableRows = hasStatusRow_ ? rows_ - 1 : rows_;
    originX_ = 1 + std::max(0, (cols_ - outCols_) / 2);
    originY_ = 1 + std::max(0, (usableRows - outRows_) / 2);

    // The grid size and position for Graphicx.
    options_.columns = outCols_;
    options_.rows = outRows_;
    screen_.setOptions(options_);
    screen_.setOrigin(originY_, originX_);
    invalidate();
}

void TerminalRenderer::setAdjustments(double contrast, double brightness) {
    options_.contrast = contrast;
    options_.brightness = brightness;
    screen_.setOptions(options_);  // new look for every cell: full repaint
    screen_.setOrigin(originY_, originX_);
}

void TerminalRenderer::invalidate() {
    screen_.invalidate();
    lastStatus_.clear();
    terminal_.write("\x1b[0m\x1b[2J");
}

void TerminalRenderer::drawStatus(std::string& out, const std::string& status) {
    if (!hasStatusRow_) {
        // No spare row: draw the indicator over the picture's bottom-left
        // cells, every frame (the picture may have just redrawn them), and
        // invalidate those cells so they are repainted once it disappears.
        if (indicator_.empty()) return;
        moveTo(out, rows_, 1);
        out += kIndicatorStyle + indicator_ + "\x1b[0m";
        int row = rows_ - originY_;  // picture row under the terminal's last row
        int covered = static_cast<int>(indicator_.size()) - (originX_ - 1);
        if (covered > 0) screen_.invalidateCells(row, 0, 1, covered);
        return;
    }

    // Only redraw when the text changed (it changes about once per second).
    std::string key = indicator_ + '\x02' + status;
    if (key == lastStatus_) return;
    lastStatus_ = key;
    moveTo(out, rows_, 1);
    out += "\x1b[0m\x1b[2K";  // reset and erase the whole line (CSI 2 K)
    size_t room = static_cast<size_t>(cols_);
    if (!indicator_.empty()) {
        out += kIndicatorStyle + indicator_ + "\x1b[0m ";
        room = room > indicator_.size() + 1 ? room - indicator_.size() - 1 : 0;
    }
    // Truncate to the space left (bytes = characters for ASCII text), in
    // reverse video (CSI 7 m).
    out += "\x1b[7m";
    out += status.substr(0, room);
    out += "\x1b[0m";
}

void TerminalRenderer::present(const gb::FrameBuffer& frame, const std::string& status) {
    if (!mapper_) return;
    if (frame.width() != frameWidth_ || frame.height() != frameHeight_) {
        frameWidth_ = frame.width();
        frameHeight_ = frame.height();
        relayout();
    }
    out_.clear();
    out_ += "\x1b[?2026h";  // begin synchronized update (ignored if unsupported)

    if (tooSmall_) {
        // "\x01" marks "message shown" so it is written only once.
        if (lastStatus_ != "\x01") {
            lastStatus_ = "\x01";
            out_ += "\x1b[0m\x1b[2J\x1b[1;1HTerminal too small - enlarge it";
        }
        out_ += "\x1b[?2026l";
        terminal_.write(out_);
        return;
    }

    mapper_->toImage(frame, image_);
    screen_.draw(image_, out_);  // changed cells only; ends with a color reset
    drawStatus(out_, status);
    out_ += "\x1b[?2026l";      // end synchronized update
    terminal_.write(out_);
}

}  // namespace term
