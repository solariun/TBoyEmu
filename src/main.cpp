// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/main.cpp
//  Layer:  Application
//
//  What this file does
//  -------------------
//  Program entry point. It parses the command line (term::Options), then runs one of three
//  modes:
//   - headless (--headless): emulate a single ROM without a terminal UI, for tests and
//     screenshots (term::HeadlessRunner);
//   - single ROM: a ROM file path was given, play it and exit;
//   - ROM browser: a directory was given (default "roms" in the current directory), list
//     its games, play the chosen one, and come back to the list when the game is left.
//  Exit codes: 0 success, 1 runtime error (exception), 2 usage error (bad options, missing
//  roms/ folder, --headless with a directory).
//
//  How it fits in
//  --------------
//  Source tree overview (where everything lives):
//    core/      Machine interface + Game Boy (Color) hardware: Cpu, Bus, Ppu,
//               Timer, Joypad, Serial, Apu, Dma, Cartridge + Mbc*, GameBoy
//    gba/       Game Boy Advance: Cpu (ARM7TDMI), Bus + Dma, Ppu, Timers,
//               Keypad, Backup (SRAM/Flash/EEPROM), Bios (HLE), Gba
//    MachineFactory  picks the console from the ROM header
//    frontend/  Terminal (raw mode), Keyboard, GameInput, TerminalRenderer,
//               ColorMapper, AnsiArt, Recorder (movies, --play), RomBrowser,
//               EmulatorSession
//    util/      RomLoader (.gb/.gbc/.gba/.zip), PngWriter
//    ../external/Graphicx  (git submodule) the Graphicx library that draws
//               pictures as half blocks / ASCII; used by the frontend
//  main() only wires the frontend objects together: Terminal (raw mode, restored by RAII
//  when it goes out of scope), Keyboard (reads keys from that terminal), EmulatorSession
//  (runs a game, using emu::createMachine() from MachineFactory) and RomBrowser.
//
//  Contributing
//  ------------
//  - Keep main() thin: options live in frontend/Options, the game loop in
//    frontend/EmulatorSession.
//  - Exceptions thrown while loading a ROM chosen in the browser are shown as a message in
//    the list instead of ending the program; other exceptions end it with exit code 1.
//  - Quick checks: ./tboyemu --help ;  ./tboyemu --headless --frames 3600 cpu_instrs.gb ;
//    ./tboyemu --headless --frames 600 --screenshot out.png rom.gba  (README.md "Testing").
// ============================================================================

#include <sys/stat.h>

#include <cstdio>
#include <exception>
#include <iostream>

#include "frontend/EmulatorSession.h"
#include "frontend/Keyboard.h"
#include "frontend/Options.h"
#include "frontend/Recorder.h"
#include "frontend/RomBrowser.h"
#include "frontend/Terminal.h"

namespace {

// True if path exists and is a directory (POSIX stat).
bool isDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// All games live under roms/ (any subfolder layout).
// Relative to the current working directory; used when no path is given.
const char kRomDirectory[] = "roms";

// Interactive (terminal) mode. Creates the terminal in raw mode, the keyboard reader and
// the session, then either plays the single ROM given or loops on the ROM browser until
// the user quits (or a termination signal is received). Returns the process exit code.
int runInteractive(term::Options& options) {
    term::Terminal terminal(options.kittyKeyboard);
    term::Keyboard keyboard(terminal);
    term::EmulatorSession session(terminal, keyboard, options);

    // Single ROM given on the command line: play it and exit.
    if (!isDirectory(options.path)) {
        session.run(options.path);
        return 0;
    }

    // Browser loop: choose() returns false when the user leaves the list. `message` is
    // shown in the browser (e.g. an empty folder or the error from the last ROM).
    term::RomBrowser browser(terminal, keyboard, options.path);
    std::string message = browser.empty() ? "No ROMs found in " + options.path : std::string();
    std::string rom;
    while (!term::Terminal::terminateRequested() && browser.choose(rom, message, options.style)) {
        try {
            session.run(rom);
        } catch (const std::exception& e) {
            message = std::string("Cannot run ROM: ") + e.what();
        }
    }
    return 0;
}

}  // namespace

// Entry point: parse options, handle --help, pick the ROM folder default, then dispatch to
// headless or interactive mode. Errors are printed to stderr.
int main(int argc, char** argv) {
    term::Options options;
    // parse() returns an error message, or an empty string on success.
    std::string error = options.parse(argc, argv);
    if (!error.empty()) {
        std::cerr << "error: " << error << "\n\n" << term::Options::usage(argv[0]);
        return 2;
    }
    if (options.help) {
        std::cout << term::Options::usage(argv[0]);
        return 0;
    }
    // No path argument: browse ./roms, which must exist.
    // --play: show a recorded movie (R key in a game) and exit; no roms/ needed.
    if (!options.playPath.empty()) {
        try {
            return term::MoviePlayer::play(options.playPath, options.kittyKeyboard);
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }
    }

    if (options.path.empty()) {
        options.path = kRomDirectory;
        if (!isDirectory(options.path)) {
            std::cerr << "error: no \"" << kRomDirectory << "\" folder here - put your games in " << kRomDirectory
                      << "/ (subfolders are fine)\n";
            return 2;
        }
    }

    try {
        if (options.headless) {
            // Headless mode runs exactly one ROM for a fixed number of frames; the runner's
            // return value becomes the exit code.
            if (isDirectory(options.path)) {
                std::cerr << "error: --headless needs a ROM file\n";
                return 2;
            }
            term::HeadlessRunner runner(options);
            return runner.run();
        }
        return runInteractive(options);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
