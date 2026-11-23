// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/util/RomLoader.h
//  Layer:  Utilities
//
//  What this file does
//  -------------------
//  Declares the ROM file loader: it reads a cartridge image either from a plain ROM file
//  (.gb, .gbc, .sgb, .gba) or from the first ROM entry inside a .zip archive, and returns
//  the bytes together with the file/entry name. It can also read only the first N bytes
//  (enough for the cartridge header) cheaply, and it knows the naming rules for ROM files
//  and their .sav battery files. Two small string helpers (toLower, endsWith) are exported
//  for reuse.
//
//  How it fits in
//  --------------
//  - MachineFactory (src/MachineFactory.cpp) uses load() to build a console and
//    loadHeader() + the entry name to detect which console a ROM targets.
//  - RomBrowser (src/frontend) uses isRomFile() while scanning roms/ and, through
//    emu::probeSystem(), loadHeader() to tag each game.
//  - EmulatorSession uses savePathFor() to know where the battery save lives.
//  Implementation: RomLoader.cpp (zip parsing and zlib inflate).
//
//  Contributing
//  ------------
//  - All functions are static / free and stateless; errors are reported with
//    std::runtime_error, which the frontend shows to the user.
//  - Test both a plain ROM and a zipped one, e.g.
//      make && ./tboyemu --headless --frames 60 game.zip
// ============================================================================
#ifndef TBOYEMU_UTIL_ROMLOADER_H
#define TBOYEMU_UTIL_ROMLOADER_H

#include <cstddef>
#include <string>
#include <vector>

#include "../core/Types.h"

namespace util {

// A ROM image and the name of the file it came from (the entry name when
// it was inside a zip archive).
// data: raw cartridge bytes (possibly only a prefix, see RomLoader::loadHeader()).
// name: file name without directories, or the zip entry name as stored in the archive
// (which may include folders); used to recognise ".gba" images.
struct RomImage {
    std::vector<gb::u8> data;
    std::string name;
};

// Loads ROM images from plain .gb/.gbc/.gba files or from inside .zip
// archives (stored or deflated entries, via zlib).  The zip reader seeks
// instead of loading the whole archive, so peeking at a header is cheap.
// Static-only utility class; no state, thread-agnostic.
class RomLoader {
public:
    // Throws std::runtime_error on failure.
    // Reads the whole ROM (for a zip: the first entry whose name passes isRomName()).
    static RomImage load(const std::string& path);
    // Only the first `bytes` bytes (header inspection for the ROM list).
    // May return fewer bytes if the file is shorter. Throws like load().
    static RomImage loadHeader(const std::string& path, size_t bytes);

    static bool isRomFile(const std::string& path);  // .gb .gbc .sgb .gba .zip
    static bool isRomName(const std::string& name);  // .gb .gbc .sgb .gba
    // Battery save path for a ROM: the ROM path with its extension replaced by ".sav"
    // (".sav" is appended when the file name has no extension).
    // E.g. "roms/Game.zip" -> "roms/Game.sav".
    static std::string savePathFor(const std::string& romPath);

private:
    // Shared implementation of load()/loadHeader(): reads at most `limit` bytes.
    static RomImage read(const std::string& path, size_t limit);
};

// ASCII lower-case copy of s (used for case-insensitive extension checks).
std::string toLower(std::string s);
// True if s ends with suffix (case-sensitive).
bool endsWith(const std::string& s, const std::string& suffix);

}  // namespace util

#endif
