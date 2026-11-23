// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Backup.cpp
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Implementation of the GBA save chips declared in Backup.h:
//   - Backup::create(): chip detection from the SDK library ID strings in the ROM;
//   - Backup::load()/save(): raw .sav file I/O shared by all chips;
//   - Flash: the JEDEC command state machine (chip ID, program byte, chip/sector erase,
//     bank switch on 128 KiB parts);
//   - Eeprom: the serial bit protocol (read / write requests of 64-bit blocks) and the
//     size detection from DMA lengths.
//
//  How it fits in
//  --------------
//  Gba creates the chip; Bus calls read8/write8 (0x0E region) or readSerial/writeSerial
//  (0x0D region); Dma calls hintDmaLength; the frontend saves through Gba::writeSave().
//
//  Hardware notes
//  --------------
//  - Flash IDs returned in ID mode (GBATEK "GBA Cart Backup Flash ROM"): Sanyo 128K = 0x62
//    0x13 (manufacturer, device); Panasonic 64K = 0x32 0x1B.
//  - Flash erase/program are instantaneous here (real chips are busy for milliseconds and
//    games poll for completion; reading back the final value satisfies that polling).
//  - EEPROM (GBATEK "GBA Cart Backup EEPROM"): the request starts with 2 bits, "11" = read,
//    "10" = write, then the block number (6 or 14 bits, MSB first), then for a write 64 data
//    bits, then one stop bit. A read reply is 4 ignored bits followed by 64 data bits. The
//    block number counts 8-byte blocks.
//
//  Contributing
//  ------------
//  - Detection order matters: "EEPROM_V" and "FLASH1M_V" are checked before the generic
//    "FLASH_V"/"FLASH512_V" strings. A game misdetected as SRAM will not save; add its case
//    carefully, the default must stay harmless.
//  - Test with the jsmolka gba-tests sram / flash64 / flash128 ROMs (README.md) and check that
//    a .sav written by another emulator loads.
// ============================================================================
#include "Backup.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace gba {

namespace {

// True if the NUL-terminated string `text` (without its terminator) appears anywhere in the
// ROM. Linear search over up to 32 MiB; only done once when the cartridge is loaded.
bool romContains(const std::vector<u8>& rom, const char* text) {
    size_t n = std::strlen(text);
    return std::search(rom.begin(), rom.end(), text, text + n) != rom.end();
}

}  // namespace

// Picks the save chip from the SDK ID strings (GBATEK "GBA Cart Backup IDs"):
//  "EEPROM_V"                -> EEPROM (size decided later),
//  "FLASH1M_V"               -> 128 KiB Flash,
//  "FLASH_V" / "FLASH512_V"  -> 64 KiB Flash,
//  anything else             -> 32 KiB SRAM.
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

// Loads a raw save file into data_. A shorter file fills only the beginning; a longer one
// is truncated to the chip size. For an EEPROM the file size decides the chip size
// (> 512 bytes means 8 KiB), which matters because games cannot tell us.
bool Backup::load(const std::string& path) {
    if (data_.empty()) return false;
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.empty()) return false;
    if (type() == Type::Eeprom) data_.resize(file.size() > 512 ? 8192 : 512, 0xFF);
    std::copy(file.begin(), file.begin() + static_cast<long>(std::min(file.size(), data_.size())), data_.begin());
    // 17 / 9 are the DMA lengths of a read request with a 14-bit / 6-bit address, so this
    // marks the EEPROM size as known. It is a no-op for SRAM and Flash.
    hintDmaLength(file.size() > 512 ? 17 : 9);  // the file settles the EEPROM size
    return true;
}

// Writes data_ to the file, replacing it. Returns false if there is nothing to save or the
// write failed.
bool Backup::save(const std::string& path) const {
    if (data_.empty()) return false;
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(&data_[0]), static_cast<std::streamsize>(data_.size()));
    return static_cast<bool>(out);
}

// ----------------------------------------------------------------- Flash ----

// Erased Flash reads 0xFF everywhere.
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

// Only the low 16 address bits are decoded (64 KiB window); the bank supplies bit 16.
u8 Flash::read8(u32 addr) {
    u32 a = addr & 0xFFFF;
    if (idMode_ && a < 2) {
        // Sanyo LE26FV10N1TS (128K) / Panasonic MN63F805MNP (64K)
        // Address 0 = manufacturer ID, address 1 = device ID.
        if (large_) return a == 0 ? 0x62 : 0x13;
        return a == 0 ? 0x32 : 0x1B;
    }
    return data_[(bank_ << 16) | a];
}

// Command state machine. Pending one-shot operations (program byte, bank select) are served
// first, because their data write does not use the unlock sequence. Otherwise the write
// advances the AA/55 unlock sequence and, at the third step, executes a command:
//  0x90 enter ID mode, 0xF0 leave ID mode, 0x80 arm erase, 0xA0 arm byte program,
//  0xB0 arm bank switch (128 KiB only).
// After 0x80, a second unlock sequence followed by 0x10 at 0x5555 erases the whole chip,
// or 0x30 at a sector address erases that 4 KiB sector.
// Note: programming stores the byte as is (real Flash can only clear bits), which is what
// games expect since they erase first.
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
            // First unlock write.
            if (a == 0x5555 && value == 0xAA) state_ = Cmd1;
            break;
        case Cmd1:
            // Second unlock write; anything else aborts the sequence.
            state_ = (a == 0x2AAA && value == 0x55) ? Cmd2 : Ready;
            break;
        case Cmd2:
            // Command byte. The machine always returns to Ready afterwards.
            state_ = Ready;
            if (eraseArmed_) {
                eraseArmed_ = false;
                if (a == 0x5555 && value == 0x10) {
                    // Chip erase: both banks.
                    std::fill(data_.begin(), data_.end(), 0xFF);
                } else if (value == 0x30) {
                    // Sector erase: the 4 KiB sector containing `a` in the current bank.
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

// Defaults to 8 KiB (erased) until hintDmaLength() or load() decides the size.
Eeprom::Eeprom() : sizeKnown_(false), readAddress_(0), readPosition_(-1) {
    data_.assign(8192, 0xFF);
}

void Eeprom::setLarge(bool large) {
    data_.resize(large ? 8192 : 512, 0xFF);
    sizeKnown_ = true;
}

// DMA lengths in halfwords (one bit each) of the two request types:
//   read request  = 2 + address bits + 1 stop  ->  9 (6-bit) or 17 (14-bit),
//   write request = 2 + address bits + 64 + 1  -> 73 (6-bit) or 81 (14-bit).
// Any other length (e.g. the 68-halfword read of the reply) says nothing about the size.
void Eeprom::hintDmaLength(int units) {
    if (sizeKnown_) return;
    if (units == 9 || units == 73) setLarge(false);
    if (units == 17 || units == 81) setLarge(true);
}

// Read reply: 68 bits, the first 4 are 0 and then the 64 data bits of the block, MSB of
// each byte first. When no read is in progress the chip reports 1 = ready (a write is
// completed immediately, so the game's "wait for ready" polling ends at once).
u16 Eeprom::readSerial() {
    if (readPosition_ < 0) return 1;  // ready
    int pos = readPosition_++;
    if (readPosition_ >= 68) readPosition_ = -1;
    if (pos < 4) return 0;  // four dummy bits
    int bit = pos - 4;
    u8 byte = data_[(readAddress_ + static_cast<u32>(bit / 8)) % data_.size()];
    return static_cast<u16>((byte >> (7 - bit % 8)) & 1);
}

// Accumulates request bits. Once the first two bits are known the request type and its
// total length are known; when all bits (including the trailing stop bit) have arrived the
// request is executed and the buffer cleared. Unknown 2-bit prefixes are discarded.
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

    // Block number, MSB first, then converted to a byte address (8 bytes per block). The
    // modulo keeps out-of-range 14-bit addresses inside the chip.
    u32 block = 0;
    for (int i = 0; i < abits; ++i) block = (block << 1) | static_cast<u32>(bits_[static_cast<size_t>(2 + i)]);
    u32 address = (block * 8) % static_cast<u32>(data_.size());
    if (isRead) {
        // Arm the reply; readSerial() streams it out.
        readAddress_ = address;
        readPosition_ = 0;
    } else {
        // Store the 64 data bits, MSB of each byte first.
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
