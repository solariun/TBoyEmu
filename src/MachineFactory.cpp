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
    gb::u8 cgbFlag = d.size() > 0x143 ? d[0x143] : 0;
    if (cgbFlag == 0xC0) return gb::System::GameBoyColor;
    if (cgbFlag & 0x80) return gb::System::GameBoyDual;
    return gb::System::GameBoy;
}

gb::System probeSystem(const std::string& path) {
    return detectSystem(util::RomLoader::loadHeader(path, 0x150));
}

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
