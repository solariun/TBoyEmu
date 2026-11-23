#ifndef TBOYEMU_FRONTEND_ANSIART_H
#define TBOYEMU_FRONTEND_ANSIART_H

#include <string>

#include "../core/Ppu.h"
#include "ColorMapper.h"
#include "TerminalRenderer.h"

namespace term {

// Encodes a full 160x144 frame as terminal text: 72 lines of 160 "upper
// half block" cells with ANSI color escapes (xterm-256 or 24-bit).
// `cat file.txt` in a color terminal shows the picture again.
class AnsiArt {
public:
    static std::string encode(const gb::FrameBuffer& frame, const ColorMapper& colors, ColorMode mode);
    static bool save(const std::string& path, const gb::FrameBuffer& frame, const ColorMapper& colors,
                     ColorMode mode);
};

}  // namespace term

#endif
