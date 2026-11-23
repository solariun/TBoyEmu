#ifndef TBOYEMU_CORE_FRAMEBUFFER_H
#define TBOYEMU_CORE_FRAMEBUFFER_H

#include <algorithm>
#include <vector>

#include "Types.h"

namespace gb {

// A video frame, one RGB555 color (0bbbbbgggggrrrrr) per pixel.  Both the
// Game Boy (160x144) and the Game Boy Advance (240x160) produce these.
class FrameBuffer {
public:
    FrameBuffer(int width, int height)
        : width_(width), height_(height), pixels_(static_cast<size_t>(width * height), 0x7FFF) {}

    int width() const { return width_; }
    int height() const { return height_; }
    u16 at(int x, int y) const { return pixels_[static_cast<size_t>(y * width_ + x)]; }
    u16* row(int y) { return &pixels_[static_cast<size_t>(y * width_)]; }
    const u16* row(int y) const { return &pixels_[static_cast<size_t>(y * width_)]; }
    void fill(u16 color) { std::fill(pixels_.begin(), pixels_.end(), color); }

private:
    int width_;
    int height_;
    std::vector<u16> pixels_;
};

}  // namespace gb

#endif
