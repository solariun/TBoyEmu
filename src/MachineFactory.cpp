// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/MachineFactory.cpp
//  Layer:  Application
//
//  What this file does
//  -------------------
//  Implementation of the console factory (see MachineFactory.h): ROM header inspection
//  (detectSystem / probeSystem) and construction of the right emulator core
//  (createMachine), followed by loading the battery save next to the ROM.
//
//  How it fits in
//  --------------
//  This is the only file that includes both cores (core/GameBoy.h and gba/Gba.h); the rest
//  of the application depends only on the gb::Machine interface.
//
//  Contributing
//  ------------
//  Detection rules, in order:
//   1. a ".gba" file / zip entry name, or no Game Boy logo at 0x104 but the GBA fixed byte
//      0x96 at 0xB2 -> Game Boy Advance (the logo check stops a Game Boy ROM whose byte
//      0xB2 happens to be 0x96 from being taken for a GBA game);
//   2. CGB flag (0x143) == 0xC0 -> Game Boy Color only;
//   3. CGB flag bit 7 set (0x80) -> Game Boy game with Color enhancements;
//   4. otherwise -> original Game Boy.
//  Header facts: Pan Docs "The Cartridge Header" (0x104 logo, 0x143 CGB flag) and GBATEK
//  "GBA Cartridge Header" (0xB2 fixed value 0x96).
// ============================================================================
#include "MachineFactory.h"

#include "core/GameBoy.h"
#include "gba/Gba.h"

namespace emu {

gb::System detectSystem(const util::RomImage& image) {
    const std::vector<gb::u8>& d = image.data;
    bool gbaName = util::endsWith(util::toLower(image.name), ".gba");
    // Game Boy carts carry the Nintendo logo at 0x104 (starts CE ED 66 66).
    bool gbLogo = d.size() > 0x107 && d[0x104] == 0xCE && d[0x105] == 0xED && d[0x106] == 0x66 && d[0x107] == 0x66;
    if (gbaName || (!gbLogo && d.size() > 0xB2 && d[0xB2] == 0x96)) return gb::System::GameBoyAdvance;
    // CGB flag: 0xC0 = works only on Game Boy Color, 0x80 = also runs on the original Game
    // Boy (dual-mode). Short images default to 0 = original Game Boy.
    gb::u8 cgbFlag = d.size() > 0x143 ? d[0x143] : 0;
    if (cgbFlag == 0xC0) return gb::System::GameBoyColor;
    if (cgbFlag & 0x80) return gb::System::GameBoyDual;
    return gb::System::GameBoy;
}

// 0x150 bytes cover the whole Game Boy header (0x100-0x14F) and the GBA header (0x00-0xBF).
gb::System probeSystem(const std::string& path) {
    return detectSystem(util::RomLoader::loadHeader(path, 0x150));
}

// Loads the full ROM, then hands its bytes (moved, not copied) to the matching core:
//  - GBA: gba::Gba takes the raw image;
//  - Game Boy / Color: the image becomes a gb::Cartridge (which picks the MBC from the
//    header) owned by gb::GameBoy.
// The battery save is loaded last; its return value is ignored because a game played for
// the first time has no .sav file yet.
std::unique_ptr<gb::Machine> createMachine(const std::string& path) {
    util::RomImage image = util::RomLoader::load(path);
    std::unique_ptr<gb::Machine> machine;
    if (detectSystem(image) == gb::System::GameBoyAdvance) {
        machine.reset(new gba::Gba(std::move(image.data)));
    } else {
        std::unique_ptr<gb::Cartridge> cart = gb::makeUnique<gb::Cartridge>(std::move(image.data));
        machine.reset(new gb::GameBoy(std::move(cart)));
    }
    machine->loadSave(util::RomLoader::savePathFor(path));
    return machine;
}

}  // namespace emu
