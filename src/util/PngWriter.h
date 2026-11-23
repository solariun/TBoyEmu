#ifndef TBOYEMU_UTIL_PNGWRITER_H
#define TBOYEMU_UTIL_PNGWRITER_H

#include <string>

#include "../core/Ppu.h"

namespace util {

// Minimal PNG encoder (zlib) used for screenshots / headless testing.
class PngWriter {
public:
    static bool write(const std::string& path, const gb::FrameBuffer& frame);
};

}  // namespace util

#endif
