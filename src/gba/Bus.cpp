#include "Bus.h"

#include <cstring>

namespace gba {

namespace {

// High-level-emulated BIOS: only the IRQ dispatcher is real ARM code (at the
// same addresses as in the original BIOS), so games' interrupt handlers run
// exactly as on hardware.  SWI calls are intercepted by the CPU.
const u32 kBiosCode[][2] = {
    {0x0018, 0xEA000042},  // b     0x128              (IRQ vector)
    {0x0128, 0xE92D500F},  // stmfd sp!, {r0-r3, r12, lr}
    {0x012C, 0xE3A00301},  // mov   r0, #0x04000000
    {0x0130, 0xE28FE000},  // add   lr, pc, #0
    {0x0134, 0xE510F004},  // ldr   pc, [r0, #-4]       (user handler at 0x03007FFC)
    {0x0138, 0xE8BD500F},  // ldmfd sp!, {r0-r3, r12, lr}
    {0x013C, 0xE25EF004},  // subs  pc, lr, #4
    {0x0144, 0xE55EC002},  // (prefetched after the IRQ return: read-protection value)
};

const int kWaitN[4] = {4, 3, 2, 8};

inline u16 load16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline u32 load32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
inline void store16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}
inline void store32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}

inline u32 vramOffset(u32 addr) {
    u32 off = addr & 0x1FFFF;
    return off >= 0x18000 ? off - 0x8000 : off;
}

}  // namespace

// =================================================================== Bus ====

Bus::Bus(const std::vector<u8>& rom, Backup& backup, Gpio* gpio, Interrupts& interrupts, Ppu& ppu, Timers& timers,
         Keypad& keypad)
    : rom_(rom),
      backup_(backup),
      gpio_(gpio),
      interrupts_(interrupts),
      ppu_(ppu),
      timers_(timers),
      keypad_(keypad),
      dma_(*this, interrupts),
      bios_(0x4000, 0),
      ewram_(0x40000, 0),
      iwram_(0x8000, 0) {
    for (size_t i = 0; i < sizeof(kBiosCode) / sizeof(kBiosCode[0]); ++i) {
        store32(&bios_[kBiosCode[i][0]], kBiosCode[i][1]);
    }
    romMask_ = 1;
    while (romMask_ < rom_.size()) romMask_ <<= 1;
    romMask_ -= 1;
    eepromLarge_ = rom_.size() > 16 * 1024 * 1024;
    reset();
}

void Bus::reset() {
    std::fill(ewram_.begin(), ewram_.end(), 0);
    std::fill(iwram_.begin(), iwram_.end(), 0);
    std::memset(soundRegs_, 0, sizeof(soundRegs_));
    waitcnt_ = 0;
    rcnt_ = 0;
    siocnt_ = 0;
    postflg_ = 1;
    cycles_ = 0;
    pc_ = 0x08000000;
    biosLatch_ = 0xE129F000;  // value seen after the boot sequence
    haltRequested_ = false;
    dma_.reset();
    updateWaitStates();
}

void Bus::updateWaitStates() {
    for (int r = 0; r < 16; ++r) {
        cost16N_[r] = cost16S_[r] = cost32N_[r] = 1;
    }
    // EWRAM: 2 wait states, 16-bit bus.
    cost16N_[2] = cost16S_[2] = 3;
    cost32N_[2] = 6;
    // Palette and VRAM: 16-bit bus.
    cost32N_[5] = cost32N_[6] = 2;
    // Cartridge ROM wait states 0/1/2 (mirrors at 08, 0A, 0C).
    const int n[3] = {kWaitN[(waitcnt_ >> 2) & 3], kWaitN[(waitcnt_ >> 5) & 3], kWaitN[(waitcnt_ >> 8) & 3]};
    const int s[3] = {(waitcnt_ & 0x10) ? 1 : 2, (waitcnt_ & 0x80) ? 1 : 4, (waitcnt_ & 0x400) ? 1 : 8};
    for (int ws = 0; ws < 3; ++ws) {
        for (int k = 0; k < 2; ++k) {
            int r = 8 + ws * 2 + k;
            cost16N_[r] = 1 + n[ws];
            cost16S_[r] = 1 + s[ws];
            cost32N_[r] = cost16N_[r] + cost16S_[r];
        }
    }
    // SRAM / Flash: 8-bit bus.
    int sram = 1 + kWaitN[waitcnt_ & 3];
    cost16N_[14] = cost16S_[14] = cost32N_[14] = sram;
    cost16N_[15] = cost16S_[15] = cost32N_[15] = sram;
    prefetch_ = (waitcnt_ & 0x4000) != 0;
}

u32 Bus::openBus(u32 /*addr*/) const { return 0; }

u32 Bus::readBios32(u32 addr) const {
    if (pc_ < 0x4000) return load32(&bios_[addr & 0x3FFC]);
    return biosLatch_;  // BIOS is read-protected when not executing from it
}

// ----------------------------------------------------------- fetching ----

u16 Bus::fetch16(u32 addr, bool sequential) {
    unsigned r = (addr >> 24) & 0xF;
    if (r >= 8 && r <= 0xD) {
        cycles_ += sequential ? (prefetch_ ? 1 : cost16S_[r]) : cost16N_[r];
        u32 off = addr & 0x01FFFFFE;
        if (off + 1 < rom_.size()) return load16(&rom_[off]);
        return static_cast<u16>(addr >> 1);
    }
    if (r == 3) {
        cycles_ += 1;
        return load16(&iwram_[addr & 0x7FFE]);
    }
    if (r == 0 && addr < 0x4000) {
        cycles_ += 1;
        return load16(&bios_[addr & 0x3FFE]);
    }
    int before = cycles_;
    u16 v = read16(addr);
    if (sequential) cycles_ = before + cost16S_[r];
    return v;
}

u32 Bus::fetch32(u32 addr, bool sequential) {
    unsigned r = (addr >> 24) & 0xF;
    if (r >= 8 && r <= 0xD) {
        cycles_ += sequential ? (prefetch_ ? 2 : 2 * cost16S_[r]) : cost32N_[r];
        u32 off = addr & 0x01FFFFFC;
        if (off + 3 < rom_.size()) return load32(&rom_[off]);
        return static_cast<u32>((addr >> 1) & 0xFFFF) | (static_cast<u32>(((addr + 2) >> 1) & 0xFFFF) << 16);
    }
    if (r == 3) {
        cycles_ += 1;
        return load32(&iwram_[addr & 0x7FFC]);
    }
    if (r == 0 && addr < 0x4000) {
        cycles_ += 1;
        return load32(&bios_[addr & 0x3FFC]);
    }
    return read32(addr);
}

// -------------------------------------------------------------- reads ----

u8 Bus::read8(u32 addr) {
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) r = 1;
    cycles_ += cost16N_[r];
    switch (r) {
        case 0x0: return addr < 0x4000 ? static_cast<u8>(readBios32(addr) >> ((addr & 3) * 8)) : 0;
        case 0x2: return ewram_[addr & 0x3FFFF];
        case 0x3: return iwram_[addr & 0x7FFF];
        case 0x4: {
            u32 off = addr & 0x00FFFFFF;
            if (off >= 0x400) return 0;
            return static_cast<u8>(readIo16(off & ~1u) >> ((off & 1) * 8));
        }
        case 0x5: return ppu_.palette()[addr & 0x3FF];
        case 0x6: return ppu_.vram()[vramOffset(addr)];
        case 0x7: return ppu_.oam()[addr & 0x3FF];
        case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
            if (r == 0xD && backup_.type() == Backup::Type::Eeprom && (!eepromLarge_ || addr >= 0x0DFFFF00)) {
                return static_cast<u8>(backup_.readSerial());
            }
            u32 off = addr & 0x01FFFFFF;
            if (gpioAt(off) && gpio_->readable()) return static_cast<u8>(gpio_->read(off & ~1u) >> ((off & 1) * 8));
            if (off < rom_.size()) return rom_[off];
            return static_cast<u8>((addr >> 1) >> ((addr & 1) * 8));
        }
        case 0xE: case 0xF: return backup_.read8(addr);
        default: return static_cast<u8>(openBus(addr));
    }
}

u16 Bus::read16(u32 addr) {
    addr &= ~1u;
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) r = 1;
    cycles_ += cost16N_[r];
    switch (r) {
        case 0x0: return addr < 0x4000 ? static_cast<u16>(readBios32(addr) >> ((addr & 2) * 8)) : 0;
        case 0x2: return load16(&ewram_[addr & 0x3FFFE]);
        case 0x3: return load16(&iwram_[addr & 0x7FFE]);
        case 0x4: {
            u32 off = addr & 0x00FFFFFF;
            return off < 0x400 ? readIo16(off) : 0;
        }
        case 0x5: return load16(&ppu_.palette()[addr & 0x3FE]);
        case 0x6: return load16(&ppu_.vram()[vramOffset(addr)]);
        case 0x7: return load16(&ppu_.oam()[addr & 0x3FE]);
        case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
            if (r == 0xD && backup_.type() == Backup::Type::Eeprom && (!eepromLarge_ || addr >= 0x0DFFFF00)) {
                return backup_.readSerial();
            }
            u32 off = addr & 0x01FFFFFE;
            if (gpioAt(off) && gpio_->readable()) return gpio_->read(off);
            if (off + 1 < rom_.size()) return load16(&rom_[off]);
            return static_cast<u16>(addr >> 1);
        }
        case 0xE: case 0xF: return static_cast<u16>(backup_.read8(addr) * 0x0101u);
        default: return static_cast<u16>(openBus(addr));
    }
}

u32 Bus::read32(u32 addr) {
    addr &= ~3u;
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) r = 1;
    switch (r) {
        case 0x0: cycles_ += 1; return addr < 0x4000 ? readBios32(addr) : 0;
        case 0x2: cycles_ += cost32N_[2]; return load32(&ewram_[addr & 0x3FFFC]);
        case 0x3: cycles_ += 1; return load32(&iwram_[addr & 0x7FFC]);
        case 0x5: cycles_ += 2; return load32(&ppu_.palette()[addr & 0x3FC]);
        case 0x6: cycles_ += 2; return load32(&ppu_.vram()[vramOffset(addr)]);
        case 0x7: cycles_ += 1; return load32(&ppu_.oam()[addr & 0x3FC]);
        case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
            cycles_ += cost32N_[r];
            u32 off = addr & 0x01FFFFFC;
            if (off + 3 < rom_.size()) return load32(&rom_[off]);
            return static_cast<u32>((addr >> 1) & 0xFFFF) | (static_cast<u32>(((addr + 2) >> 1) & 0xFFFF) << 16);
        }
        case 0xE: case 0xF: cycles_ += cost32N_[r]; return backup_.read8(addr) * 0x01010101u;
        default: {
            u32 lo = read16(addr);
            u32 hi = read16(addr + 2);
            return lo | (hi << 16);
        }
    }
}

// ------------------------------------------------------------- writes ----

void Bus::write8(u32 addr, u8 value) {
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) return;
    cycles_ += cost16N_[r];
    switch (r) {
        case 0x2: ewram_[addr & 0x3FFFF] = value; break;
        case 0x3: iwram_[addr & 0x7FFF] = value; break;
        case 0x4: {
            u32 off = addr & 0x00FFFFFF;
            if (off < 0x400) writeIo8(off, value);
            break;
        }
        case 0x5:  // 8-bit writes to palette fill the whole halfword
            store16(&ppu_.palette()[addr & 0x3FE], static_cast<u16>(value * 0x0101u));
            break;
        case 0x6: {
            u32 off = vramOffset(addr);
            u32 bgLimit = ppu_.bitmapMode() ? 0x14000 : 0x10000;
            if (off < bgLimit) store16(&ppu_.vram()[off & ~1u], static_cast<u16>(value * 0x0101u));
            break;  // 8-bit writes to sprite VRAM are ignored
        }
        case 0x7: break;  // 8-bit writes to OAM are ignored
        case 0xE: case 0xF: backup_.write8(addr, value); break;
        default: break;
    }
}

void Bus::write16(u32 addr, u16 value) {
    if ((addr >> 25) == 7 && addr < 0x10000000) {  // 0x0E / 0x0F: 8-bit save memory
        cycles_ += cost16N_[14];
        backup_.write8(addr, static_cast<u8>(value >> ((addr & 1) * 8)));
        return;
    }
    addr &= ~1u;
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) return;
    cycles_ += cost16N_[r];
    switch (r) {
        case 0x2: store16(&ewram_[addr & 0x3FFFE], value); break;
        case 0x3: store16(&iwram_[addr & 0x7FFE], value); break;
        case 0x4: {
            u32 off = addr & 0x00FFFFFF;
            if (off < 0x400) writeIo16(off, value);
            break;
        }
        case 0x5: store16(&ppu_.palette()[addr & 0x3FE], value); break;
        case 0x6: store16(&ppu_.vram()[vramOffset(addr)], value); break;
        case 0x7: store16(&ppu_.oam()[addr & 0x3FE], value); break;
        case 0x8:
            if (gpioAt(addr & 0x01FFFFFE)) gpio_->write(addr & 0x01FFFFFE, value);
            break;
        case 0xD:
            if (backup_.type() == Backup::Type::Eeprom && (!eepromLarge_ || addr >= 0x0DFFFF00)) {
                backup_.writeSerial(value);
            }
            break;
        default: break;
    }
}

void Bus::write32(u32 addr, u32 value) {
    if ((addr >> 25) == 7 && addr < 0x10000000) {  // 0x0E / 0x0F: 8-bit save memory
        cycles_ += cost16N_[14];
        backup_.write8(addr, static_cast<u8>(value >> ((addr & 3) * 8)));
        return;
    }
    addr &= ~3u;
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) return;
    switch (r) {
        case 0x2: cycles_ += cost32N_[2]; store32(&ewram_[addr & 0x3FFFC], value); break;
        case 0x3: cycles_ += 1; store32(&iwram_[addr & 0x7FFC], value); break;
        case 0x5: cycles_ += 2; store32(&ppu_.palette()[addr & 0x3FC], value); break;
        case 0x6: cycles_ += 2; store32(&ppu_.vram()[vramOffset(addr)], value); break;
        case 0x7: cycles_ += 1; store32(&ppu_.oam()[addr & 0x3FC], value); break;
        default:
            write16(addr, static_cast<u16>(value));
            write16(addr + 2, static_cast<u16>(value >> 16));
            break;
    }
}

// ---------------------------------------------------------------- I/O ----

u16 Bus::readIo16(u32 off) {
    if (off < 0x60) return ppu_.readIo(off);
    if (off < 0xB0) {
        if (off == 0x84) return soundRegs_[(0x84 - 0x60) / 2] & 0x0080;  // no channel ever plays
        if (off == 0x88) return soundRegs_[(0x88 - 0x60) / 2] ? soundRegs_[(0x88 - 0x60) / 2] : 0x0200;
        return soundRegs_[(off - 0x60) / 2];
    }
    if (off < 0xE0) return dma_.read(off - 0xB0);
    if (off >= 0x100 && off < 0x110) return timers_.read(off - 0x100);
    switch (off) {
        case 0x128: return siocnt_;
        case 0x130: return keypad_.keyInput();
        case 0x132: return keypad_.control();
        case 0x134: return rcnt_;
        case 0x200: return interrupts_.ie();
        case 0x202: return interrupts_.flags();
        case 0x204: return waitcnt_;
        case 0x208: return interrupts_.ime() ? 1 : 0;
        case 0x300: return postflg_;
        default: return 0;
    }
}

void Bus::writeIo16(u32 off, u16 v) {
    if (off < 0x60) {
        ppu_.writeIo(off, v);
        return;
    }
    if (off < 0xB0) {
        soundRegs_[(off - 0x60) / 2] = v;
        return;
    }
    if (off < 0xE0) {
        dma_.write(off - 0xB0, v);
        return;
    }
    if (off >= 0x100 && off < 0x110) {
        timers_.write(off - 0x100, v);
        return;
    }
    switch (off) {
        case 0x128:
            // No link cable: a started transfer completes at once.
            siocnt_ = static_cast<u16>(v & ~0x80);
            if ((v & 0x80) && (v & 0x4000)) interrupts_.request(irq::Serial);
            break;
        case 0x132: keypad_.setControl(v); break;
        case 0x134: rcnt_ = v; break;
        case 0x200: interrupts_.setIe(v); break;
        case 0x202: interrupts_.acknowledge(v); break;
        case 0x204:
            waitcnt_ = v & 0x5FFF;
            updateWaitStates();
            break;
        case 0x208: interrupts_.setIme(v & 1); break;
        case 0x300:
            postflg_ = v & 1;
            haltRequested_ = true;  // HALTCNT is the high byte
            break;
        default: break;
    }
}

void Bus::writeIo8(u32 off, u8 value) {
    if (off == 0x202 || off == 0x203) {
        interrupts_.acknowledge(static_cast<u16>(value << ((off & 1) * 8)));
        return;
    }
    if (off == 0x300) {
        postflg_ = value & 1;
        return;
    }
    if (off == 0x301) {
        haltRequested_ = true;  // HALTCNT (bit 7 = STOP, treated as HALT)
        return;
    }
    // Read-modify-write the surrounding halfword.
    u32 aligned = off & ~1u;
    u16 current = readIo16(aligned);
    u16 merged = (off & 1) ? static_cast<u16>((current & 0x00FF) | (value << 8))
                           : static_cast<u16>((current & 0xFF00) | value);
    writeIo16(aligned, merged);
}

// =================================================================== DMA ====

Dma::Dma(Bus& bus, Interrupts& interrupts) : bus_(bus), interrupts_(interrupts) { reset(); }

void Dma::reset() { std::memset(channels_, 0, sizeof(channels_)); }

u16 Dma::read(u32 offset) const {
    int ch = static_cast<int>(offset / 12);
    if (ch > 3) return 0;
    return (offset % 12) == 10 ? channels_[ch].control : 0;
}

void Dma::write(u32 offset, u16 v) {
    int ch = static_cast<int>(offset / 12);
    if (ch > 3) return;
    Channel& c = channels_[ch];
    switch (offset % 12) {
        case 0: c.sourceReg = (c.sourceReg & 0xFFFF0000u) | v; break;
        case 2: c.sourceReg = (c.sourceReg & 0x0000FFFFu) | (static_cast<u32>(v) << 16); break;
        case 4: c.destReg = (c.destReg & 0xFFFF0000u) | v; break;
        case 6: c.destReg = (c.destReg & 0x0000FFFFu) | (static_cast<u32>(v) << 16); break;
        case 8: c.countReg = v; break;
        case 10: {
            bool wasEnabled = (c.control & 0x8000) != 0;
            c.control = v;
            if (!wasEnabled && (v & 0x8000)) start(ch);
            break;
        }
    }
}

void Dma::start(int ch) {
    Channel& c = channels_[ch];
    c.source = c.sourceReg & (ch == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu);
    c.dest = c.destReg & (ch == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu);
    u32 count = c.countReg & (ch == 3 ? 0xFFFFu : 0x3FFFu);
    c.count = count ? count : (ch == 3 ? 0x10000u : 0x4000u);
    if (ch == 3 && (c.dest >> 24) == 0x0D) bus_.backup().hintDmaLength(static_cast<int>(c.count));
    if (((c.control >> 12) & 3) == 0) transfer(ch);  // immediate
}

void Dma::onHBlank() {
    for (int ch = 0; ch < 4; ++ch) {
        const Channel& c = channels_[ch];
        if ((c.control & 0x8000) && ((c.control >> 12) & 3) == 2) transfer(ch);
    }
}

void Dma::onVBlank() {
    for (int ch = 0; ch < 4; ++ch) {
        const Channel& c = channels_[ch];
        if ((c.control & 0x8000) && ((c.control >> 12) & 3) == 1) transfer(ch);
    }
}

void Dma::transfer(int ch) {
    Channel& c = channels_[ch];
    const bool word = (c.control & 0x0400) != 0;
    const int unit = word ? 4 : 2;
    const int destCtrl = (c.control >> 5) & 3;
    const int srcCtrl = (c.control >> 7) & 3;
    const int destStep = destCtrl == 1 ? -unit : (destCtrl == 2 ? 0 : unit);
    const int srcStep = srcCtrl == 1 ? -unit : (srcCtrl == 2 ? 0 : unit);

    u32 src = c.source & (word ? ~3u : ~1u);
    u32 dst = c.dest & (word ? ~3u : ~1u);
    for (u32 n = 0; n < c.count; ++n) {
        if (word) {
            bus_.write32(dst, bus_.read32(src));
        } else {
            bus_.write16(dst, bus_.read16(src));
        }
        src += static_cast<u32>(srcStep);
        dst += static_cast<u32>(destStep);
    }
    bus_.addCycles(2);
    c.source = src;
    c.dest = dst;

    if (c.control & 0x4000) interrupts_.request(static_cast<u16>(irq::Dma0 << ch));

    const int timing = (c.control >> 12) & 3;
    if ((c.control & 0x0200) && timing != 0) {
        // Repeat: reload the count (and the destination for mode 3).
        u32 count = c.countReg & (ch == 3 ? 0xFFFFu : 0x3FFFu);
        c.count = count ? count : (ch == 3 ? 0x10000u : 0x4000u);
        if (destCtrl == 3) c.dest = c.destReg & (ch == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu);
    } else {
        c.control &= 0x7FFF;
    }
}

}  // namespace gba
