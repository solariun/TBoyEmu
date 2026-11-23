#include "Options.h"

#include <cstdlib>

namespace term {

std::string Options::parse(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        bool hasValue = i + 1 < argc;
        if (a == "-h" || a == "--help") {
            help = true;
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
        } else if (a == "--input" && hasValue) {
            inputScript = argv[++i];
        } else if (!a.empty() && a[0] == '-') {
            return "unknown option: " + a;
        } else if (path.empty()) {
            path = a;
        } else {
            return "only one ROM path may be given";
        }
    }
    return std::string();
}

std::string Options::usage(const char* program) {
    std::string p = program;
    return "TBoyEmu - Game Boy / Game Boy Color emulator for 256-color terminals\n"
           "\n"
           "usage: " + p + " [options] [rom.gb|rom.gbc|rom.zip|directory]\n"
           "\n"
           "  With a directory (default: \"Nintendo - Game Boy Color\" if present,\n"
           "  otherwise the current directory) a ROM browser is shown.\n"
           "\n"
           "controls:  arrows = D-pad   X = A   Z = B   C = Select   Space/Enter = Start\n"
           "           P = pause   S = save screen to <rom>-<date>.txt   Esc = back / quit\n"
           "\n"
           "options:\n"
           "  --truecolor             24-bit color instead of the xterm 256 palette\n"
           "  --no-color-correction   raw CGB colors (more saturated)\n"
           "  --scale N               maximum scale (1 = 160x72 cells, default 1)\n"
           "  --no-kitty              don't use the kitty keyboard protocol\n"
           "  --no-native             don't read the OS keyboard state (macOS/Linux)\n"
           "  --hold-ms N             timed keys: hold time of a key press (150)\n"
           "  --repeat-hold-ms N      timed keys: hold time per auto-repeat (120)\n"
           "\n"
           "headless (testing):\n"
           "  --headless --frames N [--screenshot out.png|out.txt] [--input 60:start:5,120:a:5]\n";
}

}  // namespace term
