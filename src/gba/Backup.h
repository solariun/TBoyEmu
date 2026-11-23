// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Backup.h
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Declares the cartridge save memory ("backup" in GBATEK terms) of a GBA game. Backup is
//  an abstract base with one subclass per chip type:
//   - NoBackup: no save chip;
//   - Sram:     32 KiB battery-backed SRAM, plain byte reads/writes;
//   - Flash:    64 KiB or 128 KiB Flash driven by JEDEC-style command sequences;
//   - Eeprom:   512 byte or 8 KiB serial EEPROM, accessed one bit at a time.
//  Backup::create() picks the chip by searching the ROM for the library ID string that the
//  Nintendo SDK links into every game ("SRAM_V", "FLASH1M_V", "EEPROM_V", ...).
//  The base class also loads/saves the raw contents to the .sav file.
//
//  How it fits in
//  --------------
//  - Gba (Gba.h) creates the chip with Backup::create(rom) and owns it via unique_ptr.
//  - Bus routes 0x0E000000-0x0FFFFFFF to read8()/write8() (SRAM / Flash) and EEPROM
//    accesses in the 0x0D000000 region to readSerial()/writeSerial().
//  - Dma channel 3 calls hintDmaLength() when it targets 0x0D, which tells the EEPROM its
//    size (address width).
//  - The frontend calls Gba::loadSave()/writeSave(), which forward to load()/save().
//  Implementation: Backup.cpp.
//
//  Hardware notes
//  --------------
//  See GBATEK "GBA Cart Backup IDs", "GBA Cart Backup SRAM/FRAM", "GBA Cart Backup Flash
//  ROM" and "GBA Cart Backup EEPROM". Save files are raw dumps of the chip contents, the
//  same format other GBA emulators use (README.md).
//
//  Contributing
//  ------------
//  - data_ is the whole chip content and is what gets written to disk; keep its size equal
//    to the chip size (32 KiB, 64/128 KiB, 512 B/8 KiB) so .sav files stay compatible.
//  - Test: the jsmolka gba-tests "sram", "flash64" and "flash128" ROMs (README.md), and an
//    EEPROM game saving and reloading, e.g.  ./tboyemu --headless --frames 600 game.gba
// ============================================================================
#ifndef TBOYEMU_GBA_BACKUP_H
#define TBOYEMU_GBA_BACKUP_H

#include <string>
#include <vector>

#include "Common.h"

namespace gba {

// Cartridge save memory.  The chip type is detected from the library ID
// string the Nintendo SDK leaves in the ROM ("EEPROM_V", "FLASH1M_V", ...).
// Base class: the default implementations behave like an empty bus (reads return 0xFF or 1,
// writes are ignored), so subclasses only override the access path their chip uses.
// Owned by Gba through std::unique_ptr; single-threaded.
class Backup : private NonCopyable {
public:
    // Chip kinds; Flash64/Flash128 are the two Flash sizes.
    enum class Type { None, Sram, Flash64, Flash128, Eeprom };

    virtual ~Backup() {}
    // Factory: scans the ROM image for SDK ID strings and returns the matching chip
    // (never null; SRAM when nothing is recognised). See Backup.cpp for the order.
    static std::unique_ptr<Backup> create(const std::vector<u8>& rom);

    // Chip kind, and a short human readable name shown by the frontend (Gba::hardware()).
    virtual Type type() const = 0;
    virtual const char* name() const = 0;

    // 0x0E000000 region (SRAM / Flash); 8-bit bus.
    // addr is the full bus address; implementations mask the bits they decode.
    virtual u8 read8(u32 /*addr*/) { return 0xFF; }
    virtual void write8(u32 /*addr*/, u8 /*value*/) {}
    // 0x0D000000 region (EEPROM); serial, one bit per 16-bit access.
    // Only bit 0 of the value is meaningful. readSerial() returns 1 when idle ("ready").
    virtual u16 readSerial() { return 1; }
    virtual void writeSerial(u16 /*value*/) {}
    // DMA transfer length tells the EEPROM its address width.
    // units = number of halfwords of a DMA3 transfer to/from the EEPROM.
    virtual void hintDmaLength(int /*units*/) {}

    // Raw chip contents (what is stored in the .sav file).
    std::vector<u8>& data() { return data_; }
    const std::vector<u8>& data() const { return data_; }
    // Reads/writes the raw save file. Both return false (and do nothing) for a chip with no
    // storage or when the file cannot be opened; load() also fails on an empty file.
    bool load(const std::string& path);
    bool save(const std::string& path) const;

protected:
    // Chip contents; erased state is 0xFF. Empty for NoBackup.
    std::vector<u8> data_;
};

// No save chip: every access hits the base class defaults; data_ stays empty so nothing
// is ever loaded or written.
class NoBackup : public Backup {
public:
    Type type() const { return Type::None; }
    const char* name() const { return "no save"; }
};

// 32 KiB SRAM at 0x0E000000-0x0E007FFF (mirrored above: only the low 15 address bits are
// decoded). Plain byte-wide memory, starts as 0xFF.
class Sram : public Backup {
public:
    Sram() { data_.assign(32 * 1024, 0xFF); }
    Type type() const { return Type::Sram; }
    const char* name() const { return "SRAM"; }
    u8 read8(u32 addr) { return data_[addr & 0x7FFF]; }
    void write8(u32 addr, u8 value) { data_[addr & 0x7FFF] = value; }
};

// Flash with the standard JEDEC command protocol, 64 KiB or 128 KiB (two
// banks).
// Commands are written as the unlock sequence 0x5555=0xAA, 0x2AAA=0x55 followed by a
// command byte at 0x5555 (all addresses relative to 0x0E000000). The 128 KiB chip maps one
// 64 KiB bank at a time, selected with command 0xB0. Erase and program complete instantly.
class Flash : public Backup {
public:
    // large = true for 128 KiB (two banks), false for 64 KiB.
    explicit Flash(bool large);
    Type type() const { return large_ ? Type::Flash128 : Type::Flash64; }
    const char* name() const { return large_ ? "FLASH 128K" : "FLASH 64K"; }
    // Reads a byte of the current bank (or the chip ID bytes while in ID mode).
    u8 read8(u32 addr);
    // Feeds one byte write into the command state machine.
    void write8(u32 addr, u8 value);

private:
    // Progress through the two unlock writes: Ready -> Cmd1 (after AA) -> Cmd2 (after 55).
    enum State { Ready, Cmd1, Cmd2 };
    // large_: 128 KiB chip. state_: position in the unlock sequence.
    bool large_;
    State state_;
    // idMode_: command 0x90 entered "chip identification" mode (0xF0 leaves it).
    // eraseArmed_: command 0x80 was received; the next unlocked command is an erase.
    // writeArmed_: command 0xA0 was received; the next write programs one byte.
    // bankArmed_: command 0xB0 was received; the next write to address 0 selects the bank.
    bool idMode_;
    bool eraseArmed_;
    bool writeArmed_;
    bool bankArmed_;
    // Selected 64 KiB bank: 0, or 0/1 on the 128 KiB chip.
    unsigned bank_;
};

// Serial EEPROM, 512 bytes (6-bit address) or 8 KiB (14-bit address).
// The game talks to it by DMA-ing bit streams (one bit per halfword) to/from 0x0D000000.
// The size cannot be read from the ROM, so it is inferred from the first DMA length
// (hintDmaLength) or from the size of an existing save file.
class Eeprom : public Backup {
public:
    // Starts as 8 KiB until the real size is known.
    Eeprom();
    Type type() const { return Type::Eeprom; }
    const char* name() const { return data_.size() > 512 ? "EEPROM 8K" : "EEPROM 512"; }
    // Returns the next bit of a pending read (after 4 dummy zero bits), or 1 when idle.
    u16 readSerial();
    // Collects one command bit; executes the command once all its bits have arrived.
    void writeSerial(u16 value);
    // Fixes the chip size from a DMA length the first time a known length is seen.
    void hintDmaLength(int units);

private:
    // Address width in bits for the current size.
    int addressBits() const { return data_.size() > 512 ? 14 : 6; }
    // Resizes data_ to 8 KiB (true) or 512 bytes (false) and marks the size as known.
    void setLarge(bool large);

    // True once the size has been decided (later hints are ignored).
    bool sizeKnown_;
    std::vector<int> bits_;  // incoming command bits
    // Byte address of the 8-byte block being read out.
    u32 readAddress_;
    // Bit position 0-67 of the 68-bit read reply (4 dummy + 64 data bits).
    int readPosition_;  // -1 when not reading
};

}  // namespace gba

#endif
