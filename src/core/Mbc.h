// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Mbc.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Declares the Memory Bank Controllers (MBCs), the mapper chips inside Game
//  Boy cartridges. The CPU can only see 32 KiB of ROM (0x0000-0x7FFF) and
//  8 KiB of cartridge RAM (0xA000-0xBFFF) at once; the MBC decides which
//  ROM/RAM bank appears there, based on values the game writes into the ROM
//  address range. Classes: Mbc (common base), RomOnly, Mbc1 (also used for
//  HuC1), Mbc2, Mbc3 (with optional real time clock) and Mbc5 (with
//  optional rumble).
//
//  How it fits in
//  --------------
//  Created by createMbc() in Cartridge.cpp from header byte 0x0147 and owned
//  by Cartridge. Cartridge::read()/write() forward every cartridge access
//  here. Cartridge::loadSave()/writeSave() use ram() and
//  saveExtra()/loadExtra() for battery saves. Implementation: Mbc.cpp.
//
//  Hardware notes
//  --------------
//  Pan Docs, "MBCs", "No MBC", "MBC1", "MBC2", "MBC3", "MBC5":
//    0x0000-0x3FFF ROM bank 0 (MBC1 mode 1 can map other banks here)
//    0x4000-0x7FFF switchable ROM bank
//    0xA000-0xBFFF external RAM bank (or MBC3 RTC register)
//  Common writes: 0x0000-0x1FFF RAM enable (low nibble 0x0A enables),
//  0x2000-0x3FFF ROM bank, 0x4000-0x5FFF RAM bank (or upper ROM bits),
//  0x6000-0x7FFF mode / RTC latch. Details are in each class.
//  ROM banks are 16 KiB, RAM banks are 8 KiB.
//
//  Contributing
//  ------------
//  Bank numbers are always reduced modulo the real ROM/RAM size (romByte,
//  ramByte), so out-of-range banks mirror instead of crashing. Not
//  supported: MBC1 multicarts (MBC1M), MMM01, MBC6, MBC7, HuC3, Pocket
//  Camera. Test with Blargg cpu_instrs (an MBC1 cartridge):
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
// ============================================================================
#ifndef TBOYEMU_CORE_MBC_H
#define TBOYEMU_CORE_MBC_H

#include <ctime>
#include <vector>

#include "Types.h"

namespace gb {

// Memory Bank Controller: maps the cartridge ROM/RAM into 0x0000-0x7FFF
// and 0xA000-0xBFFF.  One subclass per chip family.
// Owned by Cartridge; keeps a reference to the Cartridge's ROM vector (which
// must outlive it) and owns the external RAM. Single-threaded.
class Mbc : private NonCopyable {
public:
    // rom: the whole ROM image (a multiple of 16 KiB). ramSize: external RAM
    // size in bytes (0 = none); RAM starts filled with 0xFF.
    Mbc(const std::vector<u8>& rom, size_t ramSize);
    virtual ~Mbc() {}

    // Default read: bank 0 at 0x0000, romBank_ at 0x4000, ramBank_ at
    // 0xA000 (0xFF while RAM is disabled). Other addresses read 0xFF.
    virtual u8 read(u16 addr) const;
    // Every chip decodes writes differently, so each subclass implements it.
    virtual void write(u16 addr, u8 value) = 0;

    // External (battery-backed) RAM, used for .sav files.
    std::vector<u8>& ram() { return ram_; }
    const std::vector<u8>& ram() const { return ram_; }

    // Extra battery-backed state (e.g. the MBC3 real time clock).
    // saveExtra() appends bytes after the RAM in the save file; loadExtra()
    // receives the bytes found after the RAM. Default: nothing extra.
    virtual void saveExtra(std::vector<u8>& /*out*/) const {}
    virtual void loadExtra(const u8* /*data*/, size_t /*size*/) {}

protected:
    // Byte at offset (low 14 bits used) of 16 KiB ROM bank `bank`; the bank
    // number wraps around the real ROM size.
    u8 romByte(unsigned bank, u16 offset) const {
        return rom_[(static_cast<size_t>(bank % romBanks_) << 14) | (offset & 0x3FFF)];
    }
    // Byte at offset (low 13 bits used) of 8 KiB RAM bank `bank`; wraps
    // around the real RAM size (so a 2 KiB RAM is mirrored). 0xFF if no RAM.
    u8 ramByte(unsigned bank, u16 offset) const;
    // Write counterpart of ramByte(); ignored if there is no RAM.
    void setRamByte(unsigned bank, u16 offset, u8 value);

    // ROM image (owned by Cartridge).
    const std::vector<u8>& rom_;
    // External RAM contents.
    std::vector<u8> ram_;
    // Number of 16 KiB ROM banks (at least 1).
    unsigned romBanks_;
    unsigned romBank_;  // bank mapped at 0x4000
    // RAM bank (or MBC3 RTC register select) mapped at 0xA000.
    unsigned ramBank_;
    // External RAM enable (written to 0x0000-0x1FFF).
    bool ramEnabled_;
};

// Cartridge without a mapper (32 KiB ROM, optional 8 KiB RAM).
// ROM writes are ignored; RAM is always enabled.
class RomOnly : public Mbc {
public:
    RomOnly(const std::vector<u8>& rom, size_t ramSize);
    void write(u16 addr, u8 value);
};

// MBC1: up to 2 MiB ROM (125 usable banks) and 32 KiB RAM.
// Writes: 0000-1FFF RAM enable, 2000-3FFF BANK1 (5 bits, 0 -> 1),
// 4000-5FFF BANK2 (2 bits: ROM bits 5-6 or RAM bank), 6000-7FFF mode.
// Also used for HuC1 cartridges (type 0xFF), whose banking is similar.
class Mbc1 : public Mbc {
public:
    Mbc1(const std::vector<u8>& rom, size_t ramSize);
    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);

private:
    u8 bank1_;  // 5-bit register
    u8 bank2_;  // 2-bit register
    // Banking mode (0x6000-0x7FFF): when true, BANK2 also applies to the
    // 0x0000-0x3FFF ROM area and selects the RAM bank.
    bool advancedMode_;
};

// MBC2: up to 256 KiB ROM and 512 x 4-bit RAM built into the chip.
// Writes to 0000-3FFF: address bit 8 clear = RAM enable, set = ROM bank
// (4 bits, 0 -> 1). RAM is mirrored over A000-BFFF; upper nibble reads 1.
class Mbc2 : public Mbc {
public:
    explicit Mbc2(const std::vector<u8>& rom);
    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);
};

// MBC3: up to 2 MiB ROM (7-bit bank, 0 -> 1), 32 KiB RAM (banks 0-3) and an
// optional real time clock whose registers 0x08-0x0C replace the RAM bank
// at A000-BFFF. Writing 0x00 then 0x01 to 6000-7FFF latches the clock.
class Mbc3 : public Mbc {
public:
    // hasRtc: cartridge type 0x0F/0x10 (MBC3+TIMER).
    Mbc3(const std::vector<u8>& rom, size_t ramSize, bool hasRtc);
    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);
    // RTC state in the VBA-M / BGB save trailer format (see Mbc.cpp).
    void saveExtra(std::vector<u8>& out) const;
    void loadExtra(const u8* data, size_t size);

private:
    // Real time clock driven by the host wall clock.
    // seconds 0-59, minutes 0-59, hours 0-23, days 0-511; halt = DH bit 6
    // (clock stopped), carry = DH bit 7 (day counter overflowed).
    struct Rtc {
        u32 seconds, minutes, hours, days;  // days: 9 bits
        bool halt, carry;
    };
    // Adds the host seconds elapsed since lastUpdate_ to rtc_ (unless
    // halted). const because it is called from const read/save paths.
    void updateRtc() const;
    // Value of RTC register index (0x08 S, 0x09 M, 0x0A H, 0x0B DL, 0x0C DH).
    u8 rtcRegister(const Rtc& rtc, unsigned index) const;

    // Cartridge has the clock chip.
    bool hasRtc_;
    // Live clock (advanced lazily from the host clock).
    mutable Rtc rtc_;
    // Snapshot taken by the latch sequence; this is what the game reads.
    Rtc latched_;
    // Host time (seconds since the epoch) rtc_ was last brought up to date.
    mutable std::time_t lastUpdate_;
    // Last value written to 6000-7FFF (to detect the 0x00 -> 0x01 latch).
    u8 latchState_;
};

// MBC5: up to 8 MiB ROM (9-bit bank; bank 0 can be mapped at 0x4000) and
// 128 KiB RAM (16 banks). Writes: 0000-1FFF RAM enable, 2000-2FFF ROM bank
// low 8 bits, 3000-3FFF ROM bank bit 8, 4000-5FFF RAM bank.
class Mbc5 : public Mbc {
public:
    // rumble: cartridge types 0x1C-0x1E (RAM bank bit 3 controls the motor).
    Mbc5(const std::vector<u8>& rom, size_t ramSize, bool rumble);
    void write(u16 addr, u8 value);

private:
    // Rumble cartridge: only 3 RAM bank bits are used.
    bool rumble_;
};

}  // namespace gb

#endif
