#ifndef TBOYEMU_CORE_CARTRIDGE_H
#define TBOYEMU_CORE_CARTRIDGE_H

#include <string>
#include <vector>

#include "Mbc.h"
#include "Types.h"

namespace gb {

// Parsed cartridge header (0x0100 - 0x014F).
struct CartridgeHeader {
    std::string title;
    u8 cgbFlag;
    u8 type;
    u8 romSizeCode;
    u8 ramSizeCode;

    bool supportsCgb() const { return (cgbFlag & 0x80) != 0; }
    bool hasBattery() const;
    std::string typeName() const;
};

// A cartridge = ROM image + mapper + battery-backed RAM.
class Cartridge : private NonCopyable {
public:
    // Throws std::runtime_error for unusable images.
    explicit Cartridge(std::vector<u8> rom);

    const CartridgeHeader& header() const { return header_; }

    u8 read(u16 addr) const { return mbc_->read(addr); }
    void write(u16 addr, u8 value) { mbc_->write(addr, value); }

    // Battery save (.sav) persistence; no-ops for carts without battery.
    bool loadSave(const std::string& path);
    bool writeSave(const std::string& path) const;

private:
    std::vector<u8> rom_;
    CartridgeHeader header_;
    std::unique_ptr<Mbc> mbc_;
};

}  // namespace gb

#endif
