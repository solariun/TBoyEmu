// TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
// 256-color terminals.
//
//   core/      Machine interface + Game Boy (Color) hardware: Cpu, Bus, Ppu,
//              Timer, Joypad, Serial, Apu, Dma, Cartridge + Mbc*, GameBoy
//   gba/       Game Boy Advance: Cpu (ARM7TDMI), Bus + Dma, Ppu, Timers,
//              Keypad, Backup (SRAM/Flash/EEPROM), Bios (HLE), Gba
//   MachineFactory  picks the console from the ROM header
//   frontend/  Terminal (raw mode), Keyboard, GameInput, TerminalRenderer,
//              ColorMapper, RomBrowser, EmulatorSession
//   util/      RomLoader (.gb/.gbc/.gba/.zip), PngWriter

#include <sys/stat.h>

#include <cstdio>
#include <exception>
#include <iostream>

#include "frontend/EmulatorSession.h"
#include "frontend/Keyboard.h"
#include "frontend/Options.h"
#include "frontend/RomBrowser.h"
#include "frontend/Terminal.h"

namespace {

bool isDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// All games live under roms/ (any subfolder layout).
const char kRomDirectory[] = "roms";

int runInteractive(const term::Options& options) {
    term::Terminal terminal(options.kittyKeyboard);
    term::Keyboard keyboard(terminal);
    term::EmulatorSession session(terminal, keyboard, options);

    // Single ROM given on the command line: play it and exit.
    if (!isDirectory(options.path)) {
        session.run(options.path);
        return 0;
    }

    term::RomBrowser browser(terminal, keyboard, options.path);
    std::string message = browser.empty() ? "No ROMs found in " + options.path : std::string();
    std::string rom;
    while (!term::Terminal::terminateRequested() && browser.choose(rom, message)) {
        try {
            session.run(rom);
        } catch (const std::exception& e) {
            message = std::string("Cannot run ROM: ") + e.what();
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    term::Options options;
    std::string error = options.parse(argc, argv);
    if (!error.empty()) {
        std::cerr << "error: " << error << "\n\n" << term::Options::usage(argv[0]);
        return 2;
    }
    if (options.help) {
        std::cout << term::Options::usage(argv[0]);
        return 0;
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
