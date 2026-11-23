// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/MachineFactory.h
//  Layer:  Application
//
//  What this file does
//  -------------------
//  The single place that decides which console a ROM belongs to and builds it. It turns a
//  ROM path into a ready-to-run gb::Machine (either a gb::GameBoy for Game Boy / Game Boy
//  Color games or a gba::Gba for Game Boy Advance games), with the battery save already
//  loaded. It also offers a cheap, header-only detection for the ROM list.
//
//  How it fits in
//  --------------
//  - The frontend (EmulatorSession for interactive play, HeadlessRunner for --headless)
//    calls createMachine() and then only talks to the gb::Machine interface.
//  - RomBrowser calls probeSystem() for every file under roms/ to show the GB / GB/GBC /
//    GBC / GBA tag.
//  - Uses util::RomLoader to read plain or zipped ROMs.
//  Implementation: MachineFactory.cpp.
//
//  Contributing
//  ------------
//  - Adding a new console means: a new gb::System value (core/Machine.h), a detection rule
//    in detectSystem(), and a branch in createMachine().
//  - Test detection by running ./tboyemu in a folder with roms/ and checking the tags, and
//    each console with headless mode (README.md "Testing").
// ============================================================================
#ifndef TBOYEMU_MACHINEFACTORY_H
#define TBOYEMU_MACHINEFACTORY_H

#include <memory>
#include <string>

#include "core/Machine.h"
#include "util/RomLoader.h"

namespace emu {

// Which console a ROM targets, from its header (Game Boy CGB flag at 0x143,
// GBA fixed byte 0x96 at 0xB2 / .gba file name).
// Works on a full image or on a header-only prefix (at least 0x150 bytes for a reliable
// answer). Returns GameBoy when nothing more specific is recognised.
gb::System detectSystem(const util::RomImage& image);

// Cheap header-only inspection, used by the ROM list.
// Reads only the first 0x150 bytes of the file (or zip entry). Throws std::runtime_error if
// the file cannot be read.
gb::System probeSystem(const std::string& path);

// Loads the ROM, builds the matching console and loads its battery save.
// The save path comes from util::RomLoader::savePathFor(path); a missing save is not an
// error. Throws std::runtime_error when the ROM cannot be loaded or is invalid.
std::unique_ptr<gb::Machine> createMachine(const std::string& path);

}  // namespace emu

#endif
