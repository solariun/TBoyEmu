// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/AnsiArt.cpp
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Implements AnsiArt (see AnsiArt.h): walks the frame two pixel rows at a
//  time and emits one text line per pair, with "▀" cells and SGR color codes
//  (CSI 38;5;n m / 48;5;n m for xterm-256, CSI 38;2;r;g;b m / 48;2;r;g;b m
//  for truecolor). Colors are only re-sent when they change along a line.
//
//  How it fits in
//  --------------
//  Used by EmulatorSession (S key) and HeadlessRunner (--screenshot *.txt).
//  It deliberately mirrors the color logic of TerminalRenderer but does not
//  share code with it, because it needs no cursor moves, scaling or diffing.
//
//  Terminal concepts
//  -----------------
//  * "▀" U+2580 is written as the UTF-8 bytes E2 96 80.
//  * A cell whose two pixels have the same color is written as a space with
//    only the background set: shorter, and it looks identical.
//  * "CSI 0 m" at the end of each line resets the colors, so a following
//    shell prompt is not painted in the last background color.
//
//  Contributing
//  ------------
//  * A frame with an odd height would lose its last row (the loop needs a
//    full pair); both consoles have even heights (144 and 160).
//  * Test: ./tboyemu --headless --frames 600 --screenshot shot.txt game.zip
//    and `cat shot.txt`.
// ============================================================================
#include "AnsiArt.h"

#include <fstream>

namespace term {

namespace {

const char kUpperHalf[] = "\xE2\x96\x80";  // U+2580

// Packs a color so equal colors compare equal in either mode.
// TrueColor: 0x00RRGGBB; Xterm256: palette index 16-255.
std::uint32_t colorKey(std::uint16_t c, const ColorMapper& colors, ColorMode mode) {
    if (mode == ColorMode::TrueColor) {
        Rgb rgb = colors.rgb(c);
        return (static_cast<std::uint32_t>(rgb.r) << 16) | (static_cast<std::uint32_t>(rgb.g) << 8) | rgb.b;
    }
    return colors.xterm(c);
}

// Appends the SGR sequence for a color key: "CSI 38;..." for the foreground
// or "CSI 48;..." for the background, then "2;r;g;b" (truecolor) or "5;n"
// (palette index), then the final 'm'.
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

std::string AnsiArt::encode(const gb::FrameBuffer& frame, const ColorMapper& colors, ColorMode mode,
                            RenderStyle style) {
    std::string out;
    // A busy frame can need hundreds of KB (up to ~40 bytes per cell in
    // truecolor); reserve a good chunk up front to avoid reallocations.
    out.reserve(256 * 1024);
    if (style != RenderStyle::Blocks) {
        // ASCII styles: one character per 1x2 pixels, never a background.
        const bool colored = style == RenderStyle::AsciiColor;
        for (int y = 0; y + 1 < frame.height(); y += 2) {
            const std::uint16_t* top = frame.row(y);
            const std::uint16_t* bottom = frame.row(y + 1);
            bool haveFg = false;
            std::uint32_t curFg = 0;
            for (int x = 0; x < frame.width(); ++x) {
                AsciiCell c = AsciiShader::shade(top[x], bottom[x], colors);
                if (colored && c.glyph != ' ') {
                    std::uint32_t fg = colorKey(c.color, colors, mode);
                    if (!haveFg || curFg != fg) {
                        appendColor(out, fg, true, mode);
                        curFg = fg;
                        haveFg = true;
                    }
                }
                out += c.glyph;
            }
            // Plain ASCII files contain no escape codes at all.
            out += colored ? "\x1b[0m\n" : "\n";
        }
        return out;
    }
    // One text line per pair of pixel rows (y = top, y + 1 = bottom).
    for (int y = 0; y + 1 < frame.height(); y += 2) {
        const std::uint16_t* top = frame.row(y);
        const std::uint16_t* bottom = frame.row(y + 1);
        // Colors currently set on this line; each line starts after a reset,
        // so nothing is known yet.
        bool haveFg = false, haveBg = false;
        std::uint32_t curFg = 0, curBg = 0;
        for (int x = 0; x < frame.width(); ++x) {
            std::uint32_t fg = colorKey(top[x], colors, mode);
            std::uint32_t bg = colorKey(bottom[x], colors, mode);
            // The background is needed by every cell (space or half block).
            if (!haveBg || curBg != bg) {
                appendColor(out, bg, false, mode);
                curBg = bg;
                haveBg = true;
            }
            if (fg == bg) {
                out += ' ';  // solid cell: background only
                continue;
            }
            // Two different colors: top pixel as the "▀" foreground.
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
                   ColorMode mode, RenderStyle style) {
    // Binary mode: write the bytes exactly as encoded (no newline conversion).
    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    std::string text = encode(frame, colors, mode, style);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    // The stream's state tells whether every byte was written.
    return static_cast<bool>(file);
}

}  // namespace term
