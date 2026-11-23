// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/Options.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Declares term::Options, the plain struct that holds the command line
//  configuration (ROM path, color and scaling choices, keyboard source
//  switches, timed-key tuning, headless test settings), together with the
//  parser and the --help text.
//
//  How it fits in
//  --------------
//  main.cpp creates one Options, calls parse(argc, argv), prints usage() on
//  error or --help, and fills in the default "roms" directory when no path
//  is given. The struct is then passed by const reference to
//  EmulatorSession and HeadlessRunner.
//
//  Terminal concepts
//  -----------------
//  Each option maps to a terminal feature documented elsewhere:
//    --truecolor            24-bit SGR colors instead of xterm-256 (ColorMapper)
//    --scale N              size limit of the half-block picture (TerminalRenderer)
//    --no-kitty             do not probe/enable the kitty keyboard protocol (Terminal)
//    --no-native            do not read the OS keyboard state (NativeKeyboard)
//    --hold-ms / --repeat-hold-ms  timed key mode tuning (GameInput)
//
//  Contributing
//  ------------
//  * A new option needs: a member with a default in the constructor, a
//    branch in parse(), a line in usage(), and an entry in README.md.
//  * Test: ./tboyemu --help, and an invalid option to see the error path.
// ============================================================================
#ifndef TBOYEMU_FRONTEND_OPTIONS_H
#define TBOYEMU_FRONTEND_OPTIONS_H

#include <string>

namespace term {

// Command line configuration.
// A plain value type; filled once at start-up and read-only afterwards.
struct Options {
    Options()
        : trueColor(false),
          colorCorrection(true),
          maxScale(1.0),
          kittyKeyboard(true),
          nativeKeyboard(true),
          holdMs(0),
          repeatHoldMs(0),
          headless(false),
          frames(600),
          help(false) {}

    std::string path;  // ROM file or directory of ROMs
    bool trueColor;        // --truecolor: 24-bit colors instead of xterm-256
    bool colorCorrection;  // false with --no-color-correction (raw CGB colors)
    double maxScale;       // --scale N: maximum scale factor, > 0 (default 1)
    bool kittyKeyboard;    // false with --no-kitty
    bool nativeKeyboard;  // read key state from the OS (macOS / Linux)
                          // (false with --no-native)
    int holdMs;        // 0 = from the keyboard auto-repeat settings
                       // (--hold-ms, >= 16 when given)
    int repeatHoldMs;  // 0 = from the keyboard auto-repeat settings
                       // (--repeat-hold-ms, >= 16 when given)

    // Headless mode (testing / benchmarking, no terminal UI).
    bool headless;            // --headless
    int frames;               // --frames N: number of frames to run (default 600)
    std::string screenshot;   // --screenshot FILE: .png, or .txt for ANSI art
    std::string inputScript;  // "frame:button:length,..."
                              // (--input; e.g. "300:start:5")

    bool help;  // -h / --help

    // Returns an error message, or an empty string on success.
    // Options needing a value (--scale, --hold-ms, ...) that appear last
    // without one fall through to "unknown option".
    std::string parse(int argc, char** argv);
    // The --help text; `program` is argv[0].
    static std::string usage(const char* program);
};

}  // namespace term

#endif
