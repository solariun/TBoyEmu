#include "AnsiArt.h"

#include <fstream>

namespace term {

namespace {

const char kUpperHalf[] = "\xE2\x96\x80";  // U+2580

// Packs a color so equal colors compare equal in either mode.
std::uint32_t colorKey(std::uint16_t c, const ColorMapper& colors, ColorMode mode) {
    if (mode == ColorMode::TrueColor) {
        Rgb rgb = colors.rgb(c);
        return (static_cast<std::uint32_t>(rgb.r) << 16) | (static_cast<std::uint32_t>(rgb.g) << 8) | rgb.b;
    }
    return colors.xterm(c);
}

void appendColor(std::string& out, std::uint32_t key, bool foreground, ColorMode mode) {
    out += foreground ? "\x1b[38;" : "\x1b[48;";
    if (mode == ColorMode::TrueColor) {
        out += "2;" + std::to_string((key >> 16) & 0xFF) + ";" + std::to_string((key >> 8) & 0xFF) + ";" +
               std::to_string(key & 0xFF);
    } else {
        out += "5;" + std::to_string(key);
    }
    out += "m";
}

}  // namespace

std::string AnsiArt::encode(const gb::FrameBuffer& frame, const ColorMapper& colors, ColorMode mode) {
    std::string out;
    out.reserve(256 * 1024);
    for (int y = 0; y < gb::kScreenHeight; y += 2) {
        const std::uint16_t* top = frame.row(y);
        const std::uint16_t* bottom = frame.row(y + 1);
        bool haveFg = false, haveBg = false;
        std::uint32_t curFg = 0, curBg = 0;
        for (int x = 0; x < gb::kScreenWidth; ++x) {
            std::uint32_t fg = colorKey(top[x], colors, mode);
            std::uint32_t bg = colorKey(bottom[x], colors, mode);
            if (!haveBg || curBg != bg) {
                appendColor(out, bg, false, mode);
                curBg = bg;
                haveBg = true;
            }
            if (fg == bg) {
                out += ' ';  // solid cell: background only
                continue;
            }
            if (!haveFg || curFg != fg) {
                appendColor(out, fg, true, mode);
                curFg = fg;
                haveFg = true;
            }
            out += kUpperHalf;
        }
        out += "\x1b[0m\n";  // reset so the colors don't bleed past the line
    }
    return out;
}

bool AnsiArt::save(const std::string& path, const gb::FrameBuffer& frame, const ColorMapper& colors,
                   ColorMode mode) {
    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    std::string text = encode(frame, colors, mode);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

}  // namespace term
