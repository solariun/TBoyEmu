// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Mbc.cpp
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Implements the cartridge mappers declared in Mbc.h: bank selection for
//  MBC1, MBC2, MBC3 and MBC5, RAM enable/protection, the MBC3 real time
//  clock (kept in step with the host clock) and its save-file trailer.
//
//  How it fits in
//  --------------
//  Cartridge::read()/write() call read()/write() here for every access to
//  0x0000-0x7FFF and 0xA000-0xBFFF; Cartridge::loadSave()/writeSave() call
//  loadExtra()/saveExtra(). See Mbc.h for the class overview.
//
//  Hardware notes
//  --------------
//  Pan Docs, "MBC1", "MBC2", "MBC3", "MBC5". Key quirks implemented:
//  - MBC1/MBC2/MBC3 translate ROM bank 0 to 1 for the 0x4000 area (for MBC1
//    only the 5-bit BANK1 register is checked, as on hardware); MBC5 does not.
//  - MBC1 mode 1 lets BANK2 select the bank at 0x0000 and the RAM bank.
//  - MBC2 RAM is 4 bits wide; the upper nibble reads as 1.
//  - MBC3 RTC registers: 08 seconds, 09 minutes, 0A hours, 0B day low,
//    0C day high (bit 0 day bit 8, bit 6 halt, bit 7 day carry).
//  The RTC advances from the host clock (std::time), so in-game time keeps
//  passing while the emulator is closed, as with a real battery.
//
//  Contributing
//  ------------
//  The RTC trailer layout must stay compatible with VBA-M/BGB/mGBA saves.
//  Test mapper changes with cpu_instrs (MBC1) and games using MBC3/MBC5:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
// ============================================================================
#include "Mbc.h"

#include <cstring>

namespace gb {

// ---------------------------------------------------------------- Mbc ----

Mbc::Mbc(const std::vector<u8>& rom, size_t ramSize)
    : rom_(rom),
      ram_(ramSize, 0xFF),
      romBanks_(static_cast<unsigned>(rom.size() / 0x4000)),
      romBank_(1),
      ramBank_(0),
      ramEnabled_(false) {
    // Guard against a modulo by zero in romByte().
    if (romBanks_ == 0) romBanks_ = 1;
}

u8 Mbc::read(u16 addr) const {
    if (addr < 0x4000) return romByte(0, addr);
    if (addr < 0x8000) return romByte(romBank_, addr);
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!ramEnabled_) return 0xFF;
        return ramByte(ramBank_, addr);
    }
    return 0xFF;
}

// RAM index = bank * 8 KiB + offset within the bank, wrapped by the RAM size.
u8 Mbc::ramByte(unsigned bank, u16 offset) const {
    if (ram_.empty()) return 0xFF;
    size_t index = ((static_cast<size_t>(bank) << 13) | (offset & 0x1FFF)) % ram_.size();
    return ram_[index];
}

void Mbc::setRamByte(unsigned bank, u16 offset, u8 value) {
    if (ram_.empty()) return;
    size_t index = ((static_cast<size_t>(bank) << 13) | (offset & 0x1FFF)) % ram_.size();
    ram_[index] = value;
}

// ------------------------------------------------------------ RomOnly ----

// No RAM enable register exists, so RAM (if any) is always accessible.
RomOnly::RomOnly(const std::vector<u8>& rom, size_t ramSize) : Mbc(rom, ramSize) {
    ramEnabled_ = true;
}

// Writes to ROM are ignored; only the optional RAM is writable.
void RomOnly::write(u16 addr, u8 value) {
    if (addr >= 0xA000 && addr < 0xC000) setRamByte(0, addr, value);
}

// --------------------------------------------------------------- MBC1 ----

Mbc1::Mbc1(const std::vector<u8>& rom, size_t ramSize)
    : Mbc(rom, ramSize), bank1_(1), bank2_(0), advancedMode_(false) {}

u8 Mbc1::read(u16 addr) const {
    // 0x0000-0x3FFF: bank 0, or BANK2 << 5 (0x00/0x20/0x40/0x60) in mode 1.
    if (addr < 0x4000) {
        unsigned bank = advancedMode_ ? (bank2_ << 5) : 0;
        return romByte(bank, addr);
    }
    // 0x4000-0x7FFF: 7-bit bank number = BANK2 (bits 5-6) | BANK1 (bits 0-4).
    if (addr < 0x8000) return romByte((bank2_ << 5) | bank1_, addr);
    // 0xA000-0xBFFF: RAM bank 0, or BANK2 in mode 1.
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!ramEnabled_) return 0xFF;
        return ramByte(advancedMode_ ? bank2_ : 0, addr);
    }
    return 0xFF;
}

void Mbc1::write(u16 addr, u8 value) {
    if (addr < 0x2000) {
        // RAM enable: 0x0A in the low nibble enables, anything else disables.
        ramEnabled_ = (value & 0x0F) == 0x0A;
    } else if (addr < 0x4000) {
        // BANK1: 5 bits; 0 is turned into 1 (so banks 0x20/0x40/0x60
        // cannot be selected at 0x4000, as on hardware).
        bank1_ = value & 0x1F;
        if (bank1_ == 0) bank1_ = 1;
    } else if (addr < 0x6000) {
        bank2_ = value & 0x03;
    } else if (addr < 0x8000) {
        advancedMode_ = value & 1;
    } else if (addr >= 0xA000 && addr < 0xC000 && ramEnabled_) {
        setRamByte(advancedMode_ ? bank2_ : 0, addr, value);
    }
}

// --------------------------------------------------------------- MBC2 ----

// The 512 half-bytes of built-in RAM, whatever the header says.
Mbc2::Mbc2(const std::vector<u8>& rom) : Mbc(rom, 512) {}

u8 Mbc2::read(u16 addr) const {
    // RAM: only address bits 0-8 are decoded, so 512 bytes repeat across
    // A000-BFFF; each cell is 4 bits and the upper nibble reads as 1.
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!ramEnabled_) return 0xFF;
        return static_cast<u8>(ram_[addr & 0x1FF] | 0xF0);  // 4-bit RAM
    }
    return Mbc::read(addr);
}

void Mbc2::write(u16 addr, u8 value) {
    // 0x0000-0x3FFF: address bit 8 selects which register is written.
    if (addr < 0x4000) {
        if (addr & 0x100) {
            // ROM bank (4 bits, 0 -> 1).
            romBank_ = value & 0x0F;
            if (romBank_ == 0) romBank_ = 1;
        } else {
            ramEnabled_ = (value & 0x0F) == 0x0A;
        }
    } else if (addr >= 0xA000 && addr < 0xC000 && ramEnabled_) {
        ram_[addr & 0x1FF] = value & 0x0F;
    }
}

// --------------------------------------------------------------- MBC3 ----

// The clock starts at 0 days 00:00:00 and counts from "now" until a save
// file provides the real state (loadExtra()).
Mbc3::Mbc3(const std::vector<u8>& rom, size_t ramSize, bool hasRtc)
    : Mbc(rom, ramSize), hasRtc_(hasRtc), lastUpdate_(std::time(0)), latchState_(0xFF) {
    std::memset(&rtc_, 0, sizeof(rtc_));
    std::memset(&latched_, 0, sizeof(latched_));
}

void Mbc3::updateRtc() const {
    std::time_t now = std::time(0);
    // Ignore host clocks that went backwards.
    if (now <= lastUpdate_) return;
    u64 elapsed = static_cast<u64>(now - lastUpdate_);
    lastUpdate_ = now;
    // A halted clock does not count, but lastUpdate_ still moves forward.
    if (rtc_.halt) return;

    // Convert to total seconds, add the elapsed time and split again.
    // The day counter is 9 bits: past 511 it wraps and sets the carry flag.
    u64 total = rtc_.seconds + 60ull * rtc_.minutes + 3600ull * rtc_.hours +
                86400ull * rtc_.days + elapsed;
    rtc_.seconds = static_cast<u32>(total % 60);
    total /= 60;
    rtc_.minutes = static_cast<u32>(total % 60);
    total /= 60;
    rtc_.hours = static_cast<u32>(total % 24);
    total /= 24;
    if (total > 511) rtc_.carry = true;
    rtc_.days = static_cast<u32>(total % 512);
}

u8 Mbc3::rtcRegister(const Rtc& rtc, unsigned index) const {
    switch (index) {
        case 0x08: return static_cast<u8>(rtc.seconds);
        case 0x09: return static_cast<u8>(rtc.minutes);
        case 0x0A: return static_cast<u8>(rtc.hours);
        // DL: day counter bits 0-7.
        case 0x0B: return static_cast<u8>(rtc.days & 0xFF);
        // DH: bit 0 day counter bit 8, bit 6 halt, bit 7 day carry.
        case 0x0C:
            return static_cast<u8>(((rtc.days >> 8) & 1) | (rtc.halt ? 0x40 : 0) |
                                   (rtc.carry ? 0x80 : 0));
        default: return 0xFF;
    }
}

u8 Mbc3::read(u16 addr) const {
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!ramEnabled_) return 0xFF;
        // 0x00-0x03: RAM bank; 0x08-0x0C: latched RTC register.
        if (ramBank_ <= 0x03) return ramByte(ramBank_, addr);
        if (hasRtc_) return rtcRegister(latched_, ramBank_);
        return 0xFF;
    }
    return Mbc::read(addr);
}

void Mbc3::write(u16 addr, u8 value) {
    if (addr < 0x2000) {
        // RAM and RTC enable.
        ramEnabled_ = (value & 0x0F) == 0x0A;
    } else if (addr < 0x4000) {
        // ROM bank, 7 bits, 0 -> 1.
        romBank_ = value & 0x7F;
        if (romBank_ == 0) romBank_ = 1;
    } else if (addr < 0x6000) {
        // RAM bank (0x00-0x03) or RTC register select (0x08-0x0C).
        ramBank_ = value & 0x0F;
    } else if (addr < 0x8000) {
        // Latch clock data: writing 0x00 then 0x01 copies the live clock
        // into the registers the game reads.
        if (latchState_ == 0x00 && value == 0x01 && hasRtc_) {
            updateRtc();
            latched_ = rtc_;
        }
        latchState_ = value;
    } else if (addr >= 0xA000 && addr < 0xC000 && ramEnabled_) {
        if (ramBank_ <= 0x03) {
            setRamByte(ramBank_, addr, value);
        } else if (hasRtc_ && ramBank_ >= 0x08 && ramBank_ <= 0x0C) {
            // Writing an RTC register: bring the live clock up to date first,
            // then set the field (values are wrapped into their valid range).
            updateRtc();
            switch (ramBank_) {
                case 0x08: rtc_.seconds = value % 60; break;
                case 0x09: rtc_.minutes = value % 60; break;
                case 0x0A: rtc_.hours = value % 24; break;
                case 0x0B: rtc_.days = (rtc_.days & 0x100) | value; break;
                case 0x0C:
                    rtc_.days = (rtc_.days & 0xFF) | ((value & 1u) << 8);
                    rtc_.halt = (value & 0x40) != 0;
                    rtc_.carry = (value & 0x80) != 0;
                    break;
            }
            // Make the written value visible to reads right away.
            latched_ = rtc_;
        }
    }
}

namespace {
// Little-endian 32-bit helpers for the save trailer.
void putU32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
u32 getU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}
}  // namespace

// Same 48-byte trailer layout used by VBA-M / BGB / mGBA save files.
// Layout (all little-endian 32-bit words):
//   bytes  0-19  live clock:    seconds, minutes, hours, DL, DH
//   bytes 20-39  latched clock: seconds, minutes, hours, DL, DH
//   bytes 40-47  64-bit UNIX timestamp of the moment the clock was saved
void Mbc3::saveExtra(std::vector<u8>& out) const {
    if (!hasRtc_) return;
    updateRtc();
    const Rtc* sets[2] = {&rtc_, &latched_};
    for (int s = 0; s < 2; ++s) {
        for (unsigned r = 0x08; r <= 0x0C; ++r) putU32(out, rtcRegister(*sets[s], r));
    }
    u64 ts = static_cast<u64>(lastUpdate_);
    putU32(out, static_cast<u32>(ts));
    putU32(out, static_cast<u32>(ts >> 32));
}

// Accepts the 48-byte trailer and the older 44-byte variant (32-bit
// timestamp). Fields are wrapped into their valid ranges.
void Mbc3::loadExtra(const u8* data, size_t size) {
    if (!hasRtc_ || size < 44) return;
    Rtc* sets[2] = {&rtc_, &latched_};
    for (int s = 0; s < 2; ++s) {
        const u8* p = data + s * 20;
        Rtc& r = *sets[s];
        r.seconds = getU32(p) % 60;
        r.minutes = getU32(p + 4) % 60;
        r.hours = getU32(p + 8) % 24;
        // DH word: bit 0 day bit 8, bit 6 halt, bit 7 carry.
        u32 dh = getU32(p + 16);
        r.days = (getU32(p + 12) & 0xFF) | ((dh & 1) << 8);
        r.halt = (dh & 0x40) != 0;
        r.carry = (dh & 0x80) != 0;
    }
    u64 ts = getU32(data + 40);
    if (size >= 48) ts |= static_cast<u64>(getU32(data + 44)) << 32;
    lastUpdate_ = static_cast<std::time_t>(ts);
    updateRtc();  // catch up with the time spent switched off
}

// --------------------------------------------------------------- MBC5 ----

Mbc5::Mbc5(const std::vector<u8>& rom, size_t ramSize, bool rumble)
    : Mbc(rom, ramSize), rumble_(rumble) {}

// Reads use the base Mbc::read(); romBank_ holds the full 9-bit bank number.
void Mbc5::write(u16 addr, u8 value) {
    if (addr < 0x2000) {
        ramEnabled_ = (value & 0x0F) == 0x0A;
    } else if (addr < 0x3000) {
        // ROM bank bits 0-7 (bank 0 is allowed on MBC5).
        romBank_ = (romBank_ & 0x100) | value;
    } else if (addr < 0x4000) {
        // ROM bank bit 8.
        romBank_ = (romBank_ & 0xFF) | ((value & 1u) << 8);
    } else if (addr < 0x6000) {
        ramBank_ = value & (rumble_ ? 0x07 : 0x0F);  // bit 3 drives the motor
    } else if (addr >= 0xA000 && addr < 0xC000 && ramEnabled_) {
        setRamByte(ramBank_, addr, value);
    }
}

}  // namespace gb
