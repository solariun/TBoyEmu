#ifndef TBOYEMU_CORE_MBC_H
#define TBOYEMU_CORE_MBC_H

#include <ctime>
#include <vector>

#include "Types.h"

namespace gb {

// Memory Bank Controller: maps the cartridge ROM/RAM into 0x0000-0x7FFF
// and 0xA000-0xBFFF.  One subclass per chip family.
class Mbc : private NonCopyable {
public:
    Mbc(const std::vector<u8>& rom, size_t ramSize);
    virtual ~Mbc() {}

    virtual u8 read(u16 addr) const;
    virtual void write(u16 addr, u8 value) = 0;

    std::vector<u8>& ram() { return ram_; }
    const std::vector<u8>& ram() const { return ram_; }

    // Extra battery-backed state (e.g. the MBC3 real time clock).
    virtual void saveExtra(std::vector<u8>& /*out*/) const {}
    virtual void loadExtra(const u8* /*data*/, size_t /*size*/) {}

protected:
    u8 romByte(unsigned bank, u16 offset) const {
        return rom_[(static_cast<size_t>(bank % romBanks_) << 14) | (offset & 0x3FFF)];
    }
    u8 ramByte(unsigned bank, u16 offset) const;
    void setRamByte(unsigned bank, u16 offset, u8 value);

    const std::vector<u8>& rom_;
    std::vector<u8> ram_;
    unsigned romBanks_;
    unsigned romBank_;  // bank mapped at 0x4000
    unsigned ramBank_;
    bool ramEnabled_;
};

// Cartridge without a mapper (32 KiB ROM, optional 8 KiB RAM).
class RomOnly : public Mbc {
public:
    RomOnly(const std::vector<u8>& rom, size_t ramSize);
    void write(u16 addr, u8 value);
};

class Mbc1 : public Mbc {
public:
    Mbc1(const std::vector<u8>& rom, size_t ramSize);
    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);

private:
    u8 bank1_;  // 5-bit register
    u8 bank2_;  // 2-bit register
    bool advancedMode_;
};

class Mbc2 : public Mbc {
public:
    explicit Mbc2(const std::vector<u8>& rom);
    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);
};

class Mbc3 : public Mbc {
public:
    Mbc3(const std::vector<u8>& rom, size_t ramSize, bool hasRtc);
    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);
    void saveExtra(std::vector<u8>& out) const;
    void loadExtra(const u8* data, size_t size);

private:
    // Real time clock driven by the host wall clock.
    struct Rtc {
        u32 seconds, minutes, hours, days;  // days: 9 bits
        bool halt, carry;
    };
    void updateRtc() const;
    u8 rtcRegister(const Rtc& rtc, unsigned index) const;

    bool hasRtc_;
    mutable Rtc rtc_;
    Rtc latched_;
    mutable std::time_t lastUpdate_;
    u8 latchState_;
};

class Mbc5 : public Mbc {
public:
    Mbc5(const std::vector<u8>& rom, size_t ramSize, bool rumble);
    void write(u16 addr, u8 value);

private:
    bool rumble_;
};

}  // namespace gb

#endif
