#include "Backup.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace gba {

namespace {

bool romContains(const std::vector<u8>& rom, const char* text) {
    size_t n = std::strlen(text);
    return std::search(rom.begin(), rom.end(), text, text + n) != rom.end();
}

}  // namespace

std::unique_ptr<Backup> Backup::create(const std::vector<u8>& rom) {
    if (romContains(rom, "EEPROM_V")) return std::unique_ptr<Backup>(new Eeprom());
    if (romContains(rom, "FLASH1M_V")) return std::unique_ptr<Backup>(new Flash(true));
    if (romContains(rom, "FLASH_V") || romContains(rom, "FLASH512_V")) {
        return std::unique_ptr<Backup>(new Flash(false));
    }
    // "SRAM_V", "SRAM_F_V", or no ID string at all: plain 32 KiB SRAM is the
    // harmless default.
    return std::unique_ptr<Backup>(new Sram());
}

bool Backup::load(const std::string& path) {
    if (data_.empty()) return false;
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.empty()) return false;
    if (type() == Type::Eeprom) data_.resize(file.size() > 512 ? 8192 : 512, 0xFF);
    std::copy(file.begin(), file.begin() + static_cast<long>(std::min(file.size(), data_.size())), data_.begin());
    hintDmaLength(file.size() > 512 ? 17 : 9);  // the file settles the EEPROM size
    return true;
}

bool Backup::save(const std::string& path) const {
    if (data_.empty()) return false;
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(&data_[0]), static_cast<std::streamsize>(data_.size()));
    return static_cast<bool>(out);
}

// ----------------------------------------------------------------- Flash ----

Flash::Flash(bool large)
    : large_(large),
      state_(Ready),
      idMode_(false),
      eraseArmed_(false),
      writeArmed_(false),
      bankArmed_(false),
      bank_(0) {
    data_.assign(large ? 128 * 1024 : 64 * 1024, 0xFF);
}

u8 Flash::read8(u32 addr) {
    u32 a = addr & 0xFFFF;
    if (idMode_ && a < 2) {
        // Sanyo LE26FV10N1TS (128K) / Panasonic MN63F805MNP (64K)
        if (large_) return a == 0 ? 0x62 : 0x13;
        return a == 0 ? 0x32 : 0x1B;
    }
    return data_[(bank_ << 16) | a];
}

void Flash::write8(u32 addr, u8 value) {
    u32 a = addr & 0xFFFF;
    if (writeArmed_) {
        data_[(bank_ << 16) | a] = value;
        writeArmed_ = false;
        return;
    }
    if (bankArmed_ && a == 0) {
        bank_ = large_ ? (value & 1u) : 0;
        bankArmed_ = false;
        return;
    }
    switch (state_) {
        case Ready:
            if (a == 0x5555 && value == 0xAA) state_ = Cmd1;
            break;
        case Cmd1:
            state_ = (a == 0x2AAA && value == 0x55) ? Cmd2 : Ready;
            break;
        case Cmd2:
            state_ = Ready;
            if (eraseArmed_) {
                eraseArmed_ = false;
                if (a == 0x5555 && value == 0x10) {
                    std::fill(data_.begin(), data_.end(), 0xFF);
                } else if (value == 0x30) {
                    size_t start = (static_cast<size_t>(bank_) << 16) | (a & 0xF000);
                    std::fill(data_.begin() + static_cast<long>(start),
                              data_.begin() + static_cast<long>(start + 0x1000), 0xFF);
                }
                break;
            }
            if (a != 0x5555) break;
            switch (value) {
                case 0x90: idMode_ = true; break;
                case 0xF0: idMode_ = false; break;
                case 0x80: eraseArmed_ = true; break;
                case 0xA0: writeArmed_ = true; break;
                case 0xB0: bankArmed_ = large_; break;
                default: break;
            }
            break;
    }
}

// ---------------------------------------------------------------- EEPROM ----

Eeprom::Eeprom() : sizeKnown_(false), readAddress_(0), readPosition_(-1) {
    data_.assign(8192, 0xFF);
}

void Eeprom::setLarge(bool large) {
    data_.resize(large ? 8192 : 512, 0xFF);
    sizeKnown_ = true;
}

void Eeprom::hintDmaLength(int units) {
    if (sizeKnown_) return;
    if (units == 9 || units == 73) setLarge(false);
    if (units == 17 || units == 81) setLarge(true);
}

u16 Eeprom::readSerial() {
    if (readPosition_ < 0) return 1;  // ready
    int pos = readPosition_++;
    if (readPosition_ >= 68) readPosition_ = -1;
    if (pos < 4) return 0;  // four dummy bits
    int bit = pos - 4;
    u8 byte = data_[(readAddress_ + static_cast<u32>(bit / 8)) % data_.size()];
    return static_cast<u16>((byte >> (7 - bit % 8)) & 1);
}

void Eeprom::writeSerial(u16 value) {
    bits_.push_back(value & 1);
    if (bits_.size() < 2) return;
    const int abits = addressBits();
    const bool isRead = bits_[0] == 1 && bits_[1] == 1;
    const bool isWrite = bits_[0] == 1 && bits_[1] == 0;
    if (!isRead && !isWrite) {
        bits_.clear();
        return;
    }
    size_t needed = static_cast<size_t>(2 + abits + (isWrite ? 64 : 0) + 1);
    if (bits_.size() < needed) return;

    u32 block = 0;
    for (int i = 0; i < abits; ++i) block = (block << 1) | static_cast<u32>(bits_[static_cast<size_t>(2 + i)]);
    u32 address = (block * 8) % static_cast<u32>(data_.size());
    if (isRead) {
        readAddress_ = address;
        readPosition_ = 0;
    } else {
        for (int i = 0; i < 64; ++i) {
            u8& byte = data_[address + static_cast<u32>(i / 8)];
            u8 mask = static_cast<u8>(0x80 >> (i % 8));
            if (bits_[static_cast<size_t>(2 + abits + i)]) {
                byte |= mask;
            } else {
                byte &= static_cast<u8>(~mask);
            }
        }
        readPosition_ = -1;
    }
    bits_.clear();
}

}  // namespace gba
