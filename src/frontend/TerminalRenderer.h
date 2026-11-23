#ifndef TBOYEMU_FRONTEND_TERMINALRENDERER_H
#define TBOYEMU_FRONTEND_TERMINALRENDERER_H

#include <cstdint>
#include <string>
#include <vector>

#include "../core/Ppu.h"
#include "ColorMapper.h"
#include "Terminal.h"

namespace term {

enum class ColorMode { Xterm256, TrueColor };

// Draws Game Boy frames with "upper half block" characters: each terminal
// cell shows two vertical pixels (foreground = top, background = bottom), so
// the native 160x144 picture needs 160 columns x 72 rows.  Smaller terminals
// get a nearest-neighbour downscale.  Only cells that changed since the last
// frame are sent, inside a synchronized-update block, to keep the byte rate
// low enough for 60 fps.
class TerminalRenderer {
public:
    TerminalRenderer(Terminal& terminal, ColorMode mode, double maxScale);

    void setColorMapper(const ColorMapper* mapper);
    void relayout();  // call after a terminal resize
    void invalidate();
    void present(const gb::FrameBuffer& frame, const std::string& status);

private:
    std::uint32_t colorKey(std::uint16_t rgb555) const;
    void appendFg(std::string& out, std::uint32_t key) const;
    void appendBg(std::string& out, std::uint32_t key) const;
    void appendColor(std::string& out, std::uint32_t key, bool foreground) const;
    void drawStatus(std::string& out, const std::string& status);

    Terminal& terminal_;
    ColorMode mode_;
    double maxScale_;
    const ColorMapper* mapper_;

    int cols_, rows_;
    int outCols_, outRows_;  // picture size in cells
    int originX_, originY_;  // 1-based terminal position of the picture
    bool hasStatusRow_;
    bool tooSmall_;
    std::vector<int> srcX_;       // per output column
    std::vector<int> srcTop_;     // per output row (top half)
    std::vector<int> srcBottom_;  // per output row (bottom half, -1 = none)
    std::vector<std::uint64_t> previous_;  // last drawn cell (fg/bg keys)
    std::string lastStatus_;
    std::string out_;
};

}  // namespace term

#endif
