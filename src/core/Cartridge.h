// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Cartridge.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Declares CartridgeHeader (the parsed fields of the ROM header that the
//  emulator needs) and Cartridge, which owns the ROM image, picks and owns
//  the right Memory Bank Controller (Mbc.h) and loads/writes the battery
//  save file (.sav).
//
//  How it fits in
//  --------------
//  MachineFactory loads the ROM bytes (src/util/RomLoader) and builds a
//  Cartridge, which is then moved into GameBoy. The Bus forwards CPU
//  accesses to 0x0000-0x7FFF and 0xA000-0xBFFF to read()/write(), which go
//  straight to the Mbc. GameBoy::loadSave()/writeSave() call the save
//  functions. Implementation: Cartridge.cpp.
//
//  Hardware notes
//  --------------
//  Pan Docs, "The Cartridge Header":
//    0x0134-0x0143 title (15 chars on CGB carts, the last byte is the CGB flag)
//    0x0143 CGB flag: 0x80 = CGB enhanced (also runs on DMG), 0xC0 = CGB only
//    0x0147 cartridge type (mapper + RAM + battery + timer/rumble)
//    0x0148 ROM size code, 0x0149 RAM size code
//
//  Contributing
//  ------------
//  The Mbc keeps a reference to rom_, so the Cartridge must not be copied
//  and rom_ must never be resized after construction. Save format: raw RAM
//  followed by the MBC's extra state (MBC3 RTC: the VBA/BGB 48-byte trailer).
//  Test saves by playing a battery-backed game, quitting and reloading.
// ============================================================================
#ifndef TBOYEMU_CORE_CARTRIDGE_H
#define TBOYEMU_CORE_CARTRIDGE_H

#include <string>
#include <vector>

#include "Mbc.h"
#include "Types.h"

namespace gb {

// Parsed cartridge header (0x0100 - 0x014F).
// Plain value type, filled by the Cartridge constructor.
struct CartridgeHeader {
    // Printable title (non-ASCII bytes replaced with '?', stops at the first 0).
    std::string title;
    // Byte 0x0143 (0x80 = CGB enhanced, 0xC0 = CGB only).
    u8 cgbFlag;
    // Byte 0x0147: cartridge type / mapper.
    u8 type;
    // Bytes 0x0148 / 0x0149: ROM and RAM size codes.
    u8 romSizeCode;
    u8 ramSizeCode;

    // true if the game can use Game Boy Color features (CGB flag bit 7).
    bool supportsCgb() const { return (cgbFlag & 0x80) != 0; }
    // true if the cartridge type includes a battery (save file needed).
    bool hasBattery() const;
    // Mapper name for display, e.g. "MBC3+RTC".
    std::string typeName() const;
};

// A cartridge = ROM image + mapper + battery-backed RAM.
// Owned by GameBoy (std::unique_ptr); owns the ROM bytes and the Mbc.
class Cartridge : private NonCopyable {
public:
    // Throws std::runtime_error for unusable images.
    // (Too small to hold a header, or an unsupported mapper type.)
    explicit Cartridge(std::vector<u8> rom);

    const CartridgeHeader& header() const { return header_; }

    // CPU access to 0x0000-0x7FFF (ROM / MBC control) and 0xA000-0xBFFF
    // (external RAM / RTC), delegated to the mapper.
    u8 read(u16 addr) const { return mbc_->read(addr); }
    void write(u16 addr, u8 value) { mbc_->write(addr, value); }

    // Battery save (.sav) persistence; no-ops for carts without battery.
    // Both return true on success, false if there is no battery, the file
    // cannot be opened, or (writeSave) there is nothing to save.
    bool loadSave(const std::string& path);
    bool writeSave(const std::string& path) const;

private:
    // ROM image, padded to at least 32 KiB and to a multiple of 16 KiB.
    std::vector<u8> rom_;
    CartridgeHeader header_;
    // Mapper chosen from header_.type; references rom_.
    std::unique_ptr<Mbc> mbc_;
};

}  // namespace gb

#endif
