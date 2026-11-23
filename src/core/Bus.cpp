#include "Bus.h"

#include <cstring>

namespace gb {

Bus::Bus(Cartridge& cart, Ppu& ppu, Timer& timer, Joypad& joypad, Serial& serial, Apu& apu,
         InterruptController& ic, bool cgbMode)
    : cart_(cart),
      ppu_(ppu),
      timer_(timer),
      joypad_(joypad),
      serial_(serial),
      apu_(apu),
      ic_(ic),
      cgb_(cgbMode),
      oamDma_(*this, ppu),
      hdma_(*this, ppu) {
    reset();
}

void Bus::reset() {
    std::memset(wram_, 0, sizeof(wram_));
    std::memset(hram_, 0, sizeof(hram_));
    wramBank_ = 1;
    key1_ = 0;
    doubleSpeed_ = false;
    frameReady_ = false;
}

void Bus::tick() {
    timer_.tick(4);
    serial_.tick(4);
    // The PPU runs from the fixed 4 MHz clock: in double speed mode one CPU
    // M-cycle only lasts two dots.
    u8 events = ppu_.tick(doubleSpeed_ ? 2 : 4);
    if (events & Ppu::EventHBlank) hdma_.onHBlank();
    if (events & Ppu::EventFrame) frameReady_ = true;
    oamDma_.tick();
}

void Bus::switchSpeed() {
    doubleSpeed_ = !doubleSpeed_;
    key1_ = 0;
    timer_.write(0xFF04, 0);  // STOP resets DIV
}

u8 Bus::dmaRead(u16 addr) {
    if (addr >= 0xFE00) return 0xFF;
    return read(addr);
}

u8 Bus::read(u16 addr) {
    if (addr < 0x8000) return cart_.read(addr);
    if (addr < 0xA000) return ppu_.readVram(addr);
    if (addr < 0xC000) return cart_.read(addr);
    if (addr < 0xD000) return wram_[0][addr & 0x0FFF];
    if (addr < 0xE000) return wram_[wramBank_][addr & 0x0FFF];
    if (addr < 0xFE00) return read(static_cast<u16>(addr - 0x2000));  // echo RAM
    if (addr < 0xFEA0) return ppu_.readOam(addr);
    if (addr < 0xFF00) return cgb_ ? 0x00 : 0xFF;  // unusable area
    if (addr < 0xFF80) return readIo(addr);
    if (addr < 0xFFFF) return hram_[addr & 0x7F];
    return ic_.readIE();
}

void Bus::write(u16 addr, u8 value) {
    if (addr < 0x8000) {
        cart_.write(addr, value);
    } else if (addr < 0xA000) {
        ppu_.writeVram(addr, value);
    } else if (addr < 0xC000) {
        cart_.write(addr, value);
    } else if (addr < 0xD000) {
        wram_[0][addr & 0x0FFF] = value;
    } else if (addr < 0xE000) {
        wram_[wramBank_][addr & 0x0FFF] = value;
    } else if (addr < 0xFE00) {
        write(static_cast<u16>(addr - 0x2000), value);
    } else if (addr < 0xFEA0) {
        ppu_.writeOam(addr, value);
    } else if (addr < 0xFF00) {
        // unusable
    } else if (addr < 0xFF80) {
        writeIo(addr, value);
    } else if (addr < 0xFFFF) {
        hram_[addr & 0x7F] = value;
    } else {
        ic_.writeIE(value);
    }
}

u8 Bus::readIo(u16 addr) {
    if (addr == 0xFF00) return joypad_.read();
    if (addr == 0xFF01 || addr == 0xFF02) return serial_.read(addr);
    if (addr >= 0xFF04 && addr <= 0xFF07) return timer_.read(addr);
    if (addr == 0xFF0F) return ic_.readIF();
    if (addr >= 0xFF10 && addr <= 0xFF3F) return apu_.read(addr);
    if (addr == 0xFF46) return oamDma_.page();
    if (addr >= 0xFF40 && addr <= 0xFF4B) return ppu_.readRegister(addr);
    if (cgb_) {
        if (addr == 0xFF4D) return static_cast<u8>(0x7E | (doubleSpeed_ ? 0x80 : 0) | (key1_ & 1));
        if (addr == 0xFF4F) return ppu_.readRegister(addr);
        if (addr >= 0xFF51 && addr <= 0xFF55) return hdma_.read(addr);
        if (addr >= 0xFF68 && addr <= 0xFF6C) return ppu_.readRegister(addr);
        if (addr == 0xFF70) return static_cast<u8>(0xF8 | wramBank_);
    }
    return 0xFF;
}

void Bus::writeIo(u16 addr, u8 value) {
    if (addr == 0xFF00) {
        joypad_.write(value);
    } else if (addr == 0xFF01 || addr == 0xFF02) {
        serial_.write(addr, value);
    } else if (addr >= 0xFF04 && addr <= 0xFF07) {
        timer_.write(addr, value);
    } else if (addr == 0xFF0F) {
        ic_.writeIF(value);
    } else if (addr >= 0xFF10 && addr <= 0xFF3F) {
        apu_.write(addr, value);
    } else if (addr == 0xFF46) {
        oamDma_.start(value);
    } else if (addr >= 0xFF40 && addr <= 0xFF4B) {
        ppu_.writeRegister(addr, value);
    } else if (cgb_) {
        if (addr == 0xFF4D) {
            key1_ = value & 1;
        } else if (addr == 0xFF4F || (addr >= 0xFF68 && addr <= 0xFF6C)) {
            ppu_.writeRegister(addr, value);
        } else if (addr >= 0xFF51 && addr <= 0xFF55) {
            hdma_.write(addr, value);
        } else if (addr == 0xFF70) {
            wramBank_ = value & 0x07;
            if (wramBank_ == 0) wramBank_ = 1;
        }
    }
}

}  // namespace gb
