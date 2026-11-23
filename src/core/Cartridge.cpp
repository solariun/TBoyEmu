#include "Cartridge.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace gb {

namespace {

size_t ramSizeFromCode(u8 code) {
    switch (code) {
        case 0x01: return 2 * 1024;
        case 0x02: return 8 * 1024;
        case 0x03: return 32 * 1024;
        case 0x04: return 128 * 1024;
        case 0x05: return 64 * 1024;
        default: return 0;
    }
}

std::unique_ptr<Mbc> createMbc(const CartridgeHeader& h, const std::vector<u8>& rom) {
    size_t ram = ramSizeFromCode(h.ramSizeCode);
    switch (h.type) {
        case 0x00:
        case 0x08:
        case 0x09: return makeUnique<RomOnly>(rom, ram);
        case 0x01:
        case 0x02:
        case 0x03:
        case 0xFF:  // HuC1 is close enough to MBC1 for ROM/RAM banking
            return makeUnique<Mbc1>(rom, ram);
        case 0x05:
        case 0x06: return makeUnique<Mbc2>(rom);
        case 0x0F:
        case 0x10: return makeUnique<Mbc3>(rom, ram, true);
        case 0x11:
        case 0x12:
        case 0x13: return makeUnique<Mbc3>(rom, ram, false);
        case 0x19:
        case 0x1A:
        case 0x1B: return makeUnique<Mbc5>(rom, ram, false);
        case 0x1C:
        case 0x1D:
        case 0x1E: return makeUnique<Mbc5>(rom, ram, true);
        default: break;
    }
    char msg[96];
    std::snprintf(msg, sizeof(msg), "unsupported cartridge type 0x%02X (%s)", h.type,
                  h.typeName().c_str());
    throw std::runtime_error(msg);
}

}  // namespace

bool CartridgeHeader::hasBattery() const {
    switch (type) {
        case 0x03: case 0x06: case 0x09: case 0x0D: case 0x0F: case 0x10:
        case 0x13: case 0x1B: case 0x1E: case 0x22: case 0xFE: case 0xFF:
            return true;
        default:
            return false;
    }
}

std::string CartridgeHeader::typeName() const {
    switch (type) {
        case 0x00: return "ROM";
        case 0x01: case 0x02: case 0x03: return "MBC1";
        case 0x05: case 0x06: return "MBC2";
        case 0x08: case 0x09: return "ROM+RAM";
        case 0x0B: case 0x0C: case 0x0D: return "MMM01";
        case 0x0F: case 0x10: return "MBC3+RTC";
        case 0x11: case 0x12: case 0x13: return "MBC3";
        case 0x19: case 0x1A: case 0x1B: return "MBC5";
        case 0x1C: case 0x1D: case 0x1E: return "MBC5+RUMBLE";
        case 0x20: return "MBC6";
        case 0x22: return "MBC7";
        case 0xFC: return "POCKET CAMERA";
        case 0xFE: return "HuC3";
        case 0xFF: return "HuC1";
        default: return "UNKNOWN";
    }
}

Cartridge::Cartridge(std::vector<u8> rom) : rom_(std::move(rom)) {
    if (rom_.size() < 0x8000) {
        // Pad tiny images so every bank access is in range.
        if (rom_.size() < 0x150) throw std::runtime_error("ROM image is too small");
        rom_.resize(0x8000, 0xFF);
    }
    // Round up to a whole number of 16 KiB banks.
    if (rom_.size() % 0x4000) rom_.resize((rom_.size() / 0x4000 + 1) * 0x4000, 0xFF);

    header_.cgbFlag = rom_[0x143];
    header_.type = rom_[0x147];
    header_.romSizeCode = rom_[0x148];
    header_.ramSizeCode = rom_[0x149];
    size_t titleLen = header_.supportsCgb() ? 15 : 16;
    for (size_t i = 0; i < titleLen; ++i) {
        char c = static_cast<char>(rom_[0x134 + i]);
        if (c == 0) break;
        header_.title.push_back((c >= 32 && c < 127) ? c : '?');
    }
    mbc_ = createMbc(header_, rom_);
}

bool Cartridge::loadSave(const std::string& path) {
    if (!header_.hasBattery()) return false;
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<u8>& ram = mbc_->ram();
    size_t n = std::min(ram.size(), data.size());
    std::copy(data.begin(), data.begin() + static_cast<long>(n), ram.begin());
    if (data.size() > ram.size()) mbc_->loadExtra(&data[ram.size()], data.size() - ram.size());
    return true;
}

bool Cartridge::writeSave(const std::string& path) const {
    if (!header_.hasBattery()) return false;
    std::vector<u8> data(mbc_->ram());
    mbc_->saveExtra(data);
    if (data.empty()) return false;
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(&data[0]), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

}  // namespace gb
