#ifndef TBOYEMU_MACHINEFACTORY_H
#define TBOYEMU_MACHINEFACTORY_H

#include <memory>
#include <string>

#include "core/Machine.h"
#include "util/RomLoader.h"

namespace emu {

// Which console a ROM targets, from its header (Game Boy CGB flag at 0x143,
// GBA fixed byte 0x96 at 0xB2 / .gba file name).
gb::System detectSystem(const util::RomImage& image);

// Cheap header-only inspection, used by the ROM list.
gb::System probeSystem(const std::string& path);

// Loads the ROM, builds the matching console and loads its battery save.
std::unique_ptr<gb::Machine> createMachine(const std::string& path);

}  // namespace emu

#endif
