// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/Options.cpp
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Implements Options::parse() (a simple hand-written argv loop, no getopt)
//  and Options::usage() (the --help text). See Options.h for the meaning of
//  every field.
//
//  How it fits in
//  --------------
//  Called from main() before anything touches the terminal, so errors are
//  printed on a normal (cooked-mode) terminal.
//
//  Terminal concepts
//  -----------------
//  None directly; the options only switch terminal features on or off (see
//  the table in Options.h).
//
//  Contributing
//  ------------
//  * Keep usage() in sync with parse() and with README.md.
//  * Numbers are parsed with atoi/atof, so garbage becomes 0; the range
//    checks after each value catch the cases that matter.
// ============================================================================
#include "Options.h"

#include <cstdlib>

namespace term {

std::string Options::parse(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        // True when another argument follows (for options that take a value).
        bool hasValue = i + 1 < argc;
        if (a == "-h" || a == "--help") {
            help = true;
        } else if (a == "--ascii") {
            style = RenderStyle::Ascii;
        } else if (a == "--ascii-color") {
            style = RenderStyle::AsciiColor;
        } else if (a == "--truecolor") {
            trueColor = true;
        } else if (a == "--no-color-correction") {
            colorCorrection = false;
        } else if (a == "--no-kitty") {
            kittyKeyboard = false;
        } else if (a == "--no-native") {
            nativeKeyboard = false;
        } else if (a == "--scale" && hasValue) {
            maxScale = std::atof(argv[++i]);
            if (maxScale <= 0) return "--scale must be positive";
        } else if (a == "--hold-ms" && hasValue) {
            // Minimum 16 ms, about one emulated frame (16.74 ms).
            holdMs = std::atoi(argv[++i]);
            if (holdMs < 16) return "--hold-ms must be >= 16";
        } else if (a == "--repeat-hold-ms" && hasValue) {
            repeatHoldMs = std::atoi(argv[++i]);
            if (repeatHoldMs < 16) return "--repeat-hold-ms must be >= 16";
        } else if (a == "--headless") {
            headless = true;
        } else if (a == "--frames" && hasValue) {
            frames = std::atoi(argv[++i]);
        } else if (a == "--screenshot" && hasValue) {
            screenshot = argv[++i];
        } else if (a == "--play" && hasValue) {
            playPath = argv[++i];
        } else if (a == "--input" && hasValue) {
            inputScript = argv[++i];
        } else if (!a.empty() && a[0] == '-') {
            return "unknown option: " + a;
        } else if (path.empty()) {
            // First non-option argument: the ROM file or a ROM directory.
            path = a;
        } else {
            return "only one ROM path may be given";
        }
    }
    return std::string();
}

std::string Options::usage(const char* program) {
    std::string p = program;
    return "TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for 256-color terminals\n"
           "\n"
           "usage: " + p + " [options] [rom.gb|rom.gbc|rom.gba|rom.zip|directory]\n"
           "\n"
           "  Without a ROM, every game under roms/ (and its subfolders) is listed,\n"
           "  tagged GB, GB/GBC, GBC or GBA.\n"
           "\n"
           "controls:  arrows = D-pad   X = A   Z = B   C = Select   Space/Enter = Start\n"
           "           A = L   D = R (Game Boy Advance)\n"
           "           P = pause   S = save screen to <rom>-<date>.txt   Esc = back / quit\n"
           "           R = start / stop recording a movie to <rom>-<date>-movie.txt\n"
           "           Tab (in the ROM list) = display style: FULL / ASCII-COLOR / ASCII-BW\n"
           "\n"
           "options:\n"
           "  --ascii                 draw with plain ASCII characters (no colors, no background)\n"
           "  --ascii-color           ASCII characters in the game's colors (no background)\n"
           "                          default: colored half blocks (foreground + background)\n"
           "  --truecolor             24-bit color instead of the xterm 256 palette\n"
           "  --no-color-correction   raw CGB colors (more saturated)\n"
           "  --scale N               maximum scale (1 = 160x72 cells, default 1)\n"
           "  --no-kitty              don't use the kitty keyboard protocol\n"
           "  --no-native             don't read the OS keyboard state (macOS/Linux)\n"
           "  --hold-ms N             timed keys: hold time of a key press\n"
           "                          (default: the keyboard repeat delay + 80 ms)\n"
           "  --repeat-hold-ms N      timed keys: hold time per auto-repeat\n"
           "                          (default: twice the repeat interval + 40 ms)\n"
           "\n"
           "movies:\n"
           "  --play FILE             play a recorded movie at 59.7 fps (Esc or q stops)\n"
           "\n"
           "headless (testing):\n"
           "  --headless --frames N [--screenshot out.png|out.txt] [--input 60:start:5,120:a:5]\n";
}

}  // namespace term
