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

RomOnly::RomOnly(const std::vector<u8>& rom, size_t ramSize) : Mbc(rom, ramSize) {
    ramEnabled_ = true;
}

void RomOnly::write(u16 addr, u8 value) {
    if (addr >= 0xA000 && addr < 0xC000) setRamByte(0, addr, value);
}

// --------------------------------------------------------------- MBC1 ----

Mbc1::Mbc1(const std::vector<u8>& rom, size_t ramSize)
    : Mbc(rom, ramSize), bank1_(1), bank2_(0), advancedMode_(false) {}

u8 Mbc1::read(u16 addr) const {
    if (addr < 0x4000) {
        unsigned bank = advancedMode_ ? (bank2_ << 5) : 0;
        return romByte(bank, addr);
    }
    if (addr < 0x8000) return romByte((bank2_ << 5) | bank1_, addr);
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!ramEnabled_) return 0xFF;
        return ramByte(advancedMode_ ? bank2_ : 0, addr);
    }
    return 0xFF;
}

void Mbc1::write(u16 addr, u8 value) {
    if (addr < 0x2000) {
        ramEnabled_ = (value & 0x0F) == 0x0A;
    } else if (addr < 0x4000) {
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

Mbc2::Mbc2(const std::vector<u8>& rom) : Mbc(rom, 512) {}

u8 Mbc2::read(u16 addr) const {
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!ramEnabled_) return 0xFF;
        return static_cast<u8>(ram_[addr & 0x1FF] | 0xF0);  // 4-bit RAM
    }
    return Mbc::read(addr);
}

void Mbc2::write(u16 addr, u8 value) {
    if (addr < 0x4000) {
        if (addr & 0x100) {
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

Mbc3::Mbc3(const std::vector<u8>& rom, size_t ramSize, bool hasRtc)
    : Mbc(rom, ramSize), hasRtc_(hasRtc), lastUpdate_(std::time(0)), latchState_(0xFF) {
    std::memset(&rtc_, 0, sizeof(rtc_));
    std::memset(&latched_, 0, sizeof(latched_));
}

void Mbc3::updateRtc() const {
    std::time_t now = std::time(0);
    if (now <= lastUpdate_) return;
    u64 elapsed = static_cast<u64>(now - lastUpdate_);
    lastUpdate_ = now;
    if (rtc_.halt) return;

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
        case 0x0B: return static_cast<u8>(rtc.days & 0xFF);
        case 0x0C:
            return static_cast<u8>(((rtc.days >> 8) & 1) | (rtc.halt ? 0x40 : 0) |
                                   (rtc.carry ? 0x80 : 0));
        default: return 0xFF;
    }
}

u8 Mbc3::read(u16 addr) const {
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!ramEnabled_) return 0xFF;
        if (ramBank_ <= 0x03) return ramByte(ramBank_, addr);
        if (hasRtc_) return rtcRegister(latched_, ramBank_);
        return 0xFF;
    }
    return Mbc::read(addr);
}

void Mbc3::write(u16 addr, u8 value) {
    if (addr < 0x2000) {
        ramEnabled_ = (value & 0x0F) == 0x0A;
    } else if (addr < 0x4000) {
        romBank_ = value & 0x7F;
        if (romBank_ == 0) romBank_ = 1;
    } else if (addr < 0x6000) {
        ramBank_ = value & 0x0F;
    } else if (addr < 0x8000) {
        if (latchState_ == 0x00 && value == 0x01 && hasRtc_) {
            updateRtc();
            latched_ = rtc_;
        }
        latchState_ = value;
    } else if (addr >= 0xA000 && addr < 0xC000 && ramEnabled_) {
        if (ramBank_ <= 0x03) {
            setRamByte(ramBank_, addr, value);
        } else if (hasRtc_ && ramBank_ >= 0x08 && ramBank_ <= 0x0C) {
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
            latched_ = rtc_;
        }
    }
}

namespace {
void putU32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
u32 getU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}
}  // namespace

// Same 48-byte trailer layout used by VBA-M / BGB / mGBA save files.
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

void Mbc3::loadExtra(const u8* data, size_t size) {
    if (!hasRtc_ || size < 44) return;
    Rtc* sets[2] = {&rtc_, &latched_};
    for (int s = 0; s < 2; ++s) {
        const u8* p = data + s * 20;
        Rtc& r = *sets[s];
        r.seconds = getU32(p) % 60;
        r.minutes = getU32(p + 4) % 60;
        r.hours = getU32(p + 8) % 24;
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

void Mbc5::write(u16 addr, u8 value) {
    if (addr < 0x2000) {
        ramEnabled_ = (value & 0x0F) == 0x0A;
    } else if (addr < 0x3000) {
        romBank_ = (romBank_ & 0x100) | value;
    } else if (addr < 0x4000) {
        romBank_ = (romBank_ & 0xFF) | ((value & 1u) << 8);
    } else if (addr < 0x6000) {
        ramBank_ = value & (rumble_ ? 0x07 : 0x0F);  // bit 3 drives the motor
    } else if (addr >= 0xA000 && addr < 0xC000 && ramEnabled_) {
        setRamByte(ramBank_, addr, value);
    }
}

}  // namespace gb
