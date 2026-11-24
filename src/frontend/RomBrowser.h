// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/RomBrowser.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Declares term::RomBrowser, the start screen. On construction it scans a
//  directory (normally roms/) and its subfolders for ROM files, reads each
//  ROM header to tag the game with its system (GB, GB/GBC, GBC, GBA), and
//  sorts the list by name. choose() then shows an interactive, scrollable
//  list with a type-to-search box until the user picks a game or quits.
//
//  How it fits in
//  --------------
//  main.cpp creates one RomBrowser when the path is a directory and loops:
//  choose() -> EmulatorSession::run(path) -> back to choose() (with an
//  error message if the ROM failed to load). The system tag comes from
//  emu::probeSystem() (MachineFactory), file filtering from
//  util::RomLoader::isRomFile().
//
//  Terminal concepts
//  -----------------
//  The screen is redrawn completely on every change (it is small), inside a
//  synchronized-output block (CSI ? 2026 h ... l) to avoid flicker. Colors
//  use xterm-256 SGR codes (CSI 38;5;n m foreground, CSI 48;5;n m
//  background, CSI 1 m bold); rows are placed with CSI row;1 H. Key release
//  and focus events (kitty protocol / focus reporting) are ignored here.
//
//  Contributing
//  ------------
//  * Scanning reads the header (first 0x150 bytes, via
//    util::RomLoader::loadHeader) of every ROM, opening .zip archives too,
//    so big collections take a moment; a progress line is drawn meanwhile.
//  * Test: put a few .gb/.gbc/.gba/.zip files in roms/ (in subfolders too),
//    run ./tboyemu, type "gba" or part of a name, and use PgUp/PgDn/Home/End.
// ============================================================================
#ifndef TBOYEMU_FRONTEND_ROMBROWSER_H
#define TBOYEMU_FRONTEND_ROMBROWSER_H

#include <string>
#include <vector>

#include "../core/Machine.h"
#include <graphicx/graphicx.h>
#include "Keyboard.h"
#include "Terminal.h"

namespace term {

// Start screen: every ROM found under a folder (and its subfolders), each
// tagged with the console it is for (read from the ROM header), with
// type-to-filter search over names and system tags.
//
// Lifetime: one instance for the whole interactive session (owned by
// main.cpp); the list is scanned once, in the constructor. The search text
// and selection persist between games.
class RomBrowser {
public:
    // Scans `directory` (up to 4 levels of subfolders below it) and reads
    // every ROM header, drawing progress on the terminal while doing so.
    RomBrowser(Terminal& terminal, Keyboard& keyboard, const std::string& directory);

    // True when no ROM file was found.
    bool empty() const { return entries_.empty(); }
    // Blocks until the user picks a ROM (true) or quits (false).
    // path: receives the chosen ROM's file path.
    // message: shown in the bottom line (e.g. a load error from the previous
    // game) until the next key press, which clears it.
    // Quitting = Esc with an empty search, Ctrl+C, or a termination signal.
    // style: the display style games will start in; Tab cycles it
    // (FULL -> ASCII-COLOR -> ASCII-BW) and the change is written back here.
    bool choose(std::string& path, std::string& message, gfx::Style& style);

private:
    // One ROM file in the list.
    struct Entry {
        std::string path;    // full path, e.g. "roms/GBA/Game.zip"
        std::string name;    // file name without directory and extension
        gb::System system;   // console the ROM is for (GameBoy if unknown)
        bool known;  // header could be read
    };

    // Recursively collects ROM file paths below `directory` into `files`.
    // depth = current nesting level (stops below level 4); hidden entries
    // (names starting with '.') are skipped.
    void scan(const std::string& directory, int depth, std::vector<std::string>& files);
    // Rebuilds visible_ from filter_ (case-insensitive name substring, or a
    // system tag prefix such as "gb", "gbc", "gba").
    void applyFilter();
    // Redraws the whole screen: header with counts, search box, list, help.
    void draw(const std::string& message, gfx::Style style);
    // Shows "Reading ROM headers ... done/total" during the constructor.
    void drawProgress(size_t done, size_t total);

    Terminal& terminal_;
    Keyboard& keyboard_;
    std::string directory_;          // scanned folder (shown in the progress line)
    std::vector<Entry> entries_;     // all ROMs, sorted by name (case-insensitive)
    std::vector<size_t> visible_;  // indexes into entries_
    std::string filter_;             // search text typed by the user (max 40 chars)
    size_t selected_;  // index into visible_
    size_t scroll_;    // index into visible_ of the first row on screen
    int counts_[4];    // ROMs per gb::System
};

}  // namespace term

#endif
