#ifndef TBOYEMU_FRONTEND_OPTIONS_H
#define TBOYEMU_FRONTEND_OPTIONS_H

#include <string>

namespace term {

// Command line configuration.
struct Options {
    Options()
        : trueColor(false),
          colorCorrection(true),
          maxScale(1.0),
          kittyKeyboard(true),
          nativeKeyboard(true),
          holdMs(150),
          repeatHoldMs(120),
          headless(false),
          frames(600),
          help(false) {}

    std::string path;  // ROM file or directory of ROMs
    bool trueColor;
    bool colorCorrection;
    double maxScale;
    bool kittyKeyboard;
    bool nativeKeyboard;  // read key state from the OS (macOS / Linux)
    int holdMs;
    int repeatHoldMs;

    // Headless mode (testing / benchmarking, no terminal UI).
    bool headless;
    int frames;
    std::string screenshot;
    std::string inputScript;  // "frame:button:length,..."

    bool help;

    // Returns an error message, or an empty string on success.
    std::string parse(int argc, char** argv);
    static std::string usage(const char* program);
};

}  // namespace term

#endif
