#include "TerminalRenderer.h"

#include <algorithm>

namespace term {

namespace {

const char kUpperHalf[] = "\xE2\x96\x80";  // U+2580
const std::uint64_t kInvalid = ~0ull;

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

void moveTo(std::string& out, int row, int col) {
    out += "\x1b[";
    appendInt(out, row);
    out.push_back(';');
    appendInt(out, col);
    out.push_back('H');
}

}  // namespace

TerminalRenderer::TerminalRenderer(Terminal& terminal, ColorMode mode, double maxScale)
    : terminal_(terminal),
      mode_(mode),
      maxScale_(maxScale > 0 ? maxScale : 1.0),
      mapper_(0),
      cols_(0),
      rows_(0),
      outCols_(0),
      outRows_(0),
      originX_(1),
      originY_(1),
      hasStatusRow_(false),
      tooSmall_(false) {
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

    double scale = std::min(static_cast<double>(cols_) / gb::kScreenWidth,
                            static_cast<double>(rows_ * 2) / gb::kScreenHeight);
    scale = std::min(scale, maxScale_);
    int pixW = static_cast<int>(gb::kScreenWidth * scale);
    int pixH = static_cast<int>(gb::kScreenHeight * scale);
    tooSmall_ = pixW < 32 || pixH < 28;

    outCols_ = std::max(1, pixW);
    outRows_ = std::max(1, (pixH + 1) / 2);
    hasStatusRow_ = outRows_ < rows_;
    int usableRows = hasStatusRow_ ? rows_ - 1 : rows_;
    originX_ = 1 + std::max(0, (cols_ - outCols_) / 2);
    originY_ = 1 + std::max(0, (usableRows - outRows_) / 2);

    srcX_.resize(static_cast<size_t>(outCols_));
    for (int x = 0; x < outCols_; ++x) {
        srcX_[static_cast<size_t>(x)] = std::min(gb::kScreenWidth - 1, static_cast<int>(x / scale));
    }
    srcTop_.resize(static_cast<size_t>(outRows_));
    srcBottom_.resize(static_cast<size_t>(outRows_));
    for (int y = 0; y < outRows_; ++y) {
        int top = 2 * y;
        int bottom = 2 * y + 1;
        srcTop_[static_cast<size_t>(y)] = std::min(gb::kScreenHeight - 1, static_cast<int>(top / scale));
        srcBottom_[static_cast<size_t>(y)] =
            bottom < pixH ? std::min(gb::kScreenHeight - 1, static_cast<int>(bottom / scale)) : -1;
    }
    invalidate();
}

void TerminalRenderer::invalidate() {
    previous_.assign(static_cast<size_t>(outCols_ * outRows_), kInvalid);
    lastStatus_.clear();
    terminal_.write("\x1b[0m\x1b[2J");
}

std::uint32_t TerminalRenderer::colorKey(std::uint16_t c) const {
    if (mode_ == ColorMode::TrueColor) {
        Rgb rgb = mapper_->rgb(c);
        return (static_cast<std::uint32_t>(rgb.r) << 16) | (static_cast<std::uint32_t>(rgb.g) << 8) | rgb.b;
    }
    return mapper_->xterm(c);
}

void TerminalRenderer::appendColor(std::string& out, std::uint32_t key, bool foreground) const {
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
    if (!hasStatusRow_ || status == lastStatus_) return;
    lastStatus_ = status;
    std::string line = status.substr(0, static_cast<size_t>(cols_));
    moveTo(out, rows_, 1);
    out += "\x1b[0m\x1b[2K\x1b[7m";
    out += line;
    out += "\x1b[0m";
}

void TerminalRenderer::present(const gb::FrameBuffer& frame, const std::string& status) {
    if (!mapper_) return;
    out_.clear();
    out_ += "\x1b[?2026h";  // begin synchronized update (ignored if unsupported)

    if (tooSmall_) {
        if (lastStatus_ != "\x01") {
            lastStatus_ = "\x01";
            out_ += "\x1b[0m\x1b[2J\x1b[1;1HTerminal too small - enlarge it (160x72 for full size)";
        }
        out_ += "\x1b[?2026l";
        terminal_.write(out_);
        return;
    }

    bool haveColors = false;
    std::uint32_t curFg = 0, curBg = 0;
    int cursorRow = -1, cursorCol = -1;

    for (int y = 0; y < outRows_; ++y) {
        const std::uint16_t* top = frame.row(srcTop_[static_cast<size_t>(y)]);
        int bottomRow = srcBottom_[static_cast<size_t>(y)];
        const std::uint16_t* bottom = bottomRow >= 0 ? frame.row(bottomRow) : 0;

        for (int x = 0; x < outCols_; ++x) {
            int sx = srcX_[static_cast<size_t>(x)];
            std::uint32_t fg = colorKey(top[sx]);
            std::uint32_t bg = bottom ? colorKey(bottom[sx]) : 0;
            // Terminal default background below the last half row.
            std::uint64_t cell = (static_cast<std::uint64_t>(fg) << 32) | bg | (bottom ? 0 : (1ull << 31));
            std::uint64_t& prev = previous_[static_cast<size_t>(y * outCols_ + x)];
            if (prev == cell) continue;
            prev = cell;

            int row = originY_ + y;
            int col = originX_ + x;
            if (row != cursorRow || col != cursorCol) moveTo(out_, row, col);

            if (!bottom) {
                out_ += "\x1b[49m";
                appendFg(out_, fg);
                out_ += kUpperHalf;
                haveColors = false;
            } else if (fg == bg) {
                // Solid cell: a space only needs the background color.
                if (!haveColors || curBg != bg) {
                    appendBg(out_, bg);
                    curBg = bg;
                    if (!haveColors) curFg = ~0u;
                    haveColors = true;
                }
                out_.push_back(' ');
            } else {
                if (!haveColors || curFg != fg) appendFg(out_, fg);
                if (!haveColors || curBg != bg) appendBg(out_, bg);
                curFg = fg;
                curBg = bg;
                haveColors = true;
                out_ += kUpperHalf;
            }
            cursorRow = row;
            cursorCol = col + 1;
        }
    }

    out_ += "\x1b[0m";
    drawStatus(out_, status);
    out_ += "\x1b[?2026l";  // end synchronized update
    terminal_.write(out_);
}

}  // namespace term
