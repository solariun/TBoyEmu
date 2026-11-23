// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Bus.cpp
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Implements the memory map declared in Bus.h: address decoding for CPU
//  reads and writes, the I/O register dispatch (0xFF00-0xFF7F), work RAM
//  banking, the CGB speed switch, and tick(), which advances the timer,
//  serial port, PPU and DMA engines by one CPU M-cycle.
//
//  How it fits in
//  --------------
//  Cpu::read8()/write8()/idle() call tick() and then read()/write().
//  Every other component is reached from here; see Bus.h for the map.
//
//  Hardware notes
//  --------------
//  Pan Docs, "Memory Map", "I/O Ranges" and "CGB Registers":
//    0xFF00 P1, 0xFF01-02 serial, 0xFF04-07 timer, 0xFF0F IF,
//    0xFF10-3F audio, 0xFF40-4B LCD (0xFF46 is OAM DMA),
//    CGB: 0xFF4D KEY1, 0xFF4F VBK, 0xFF51-55 HDMA, 0xFF68-6B palettes,
//         0xFF6C OPRI, 0xFF70 SVBK.
//  Unmapped I/O reads return 0xFF. The PPU is clocked by the fixed
//  4.194304 MHz dot clock while the CPU M-cycle halves in double speed,
//  so each M-cycle is 4 dots in normal speed and 2 dots in double speed.
//
//  Contributing
//  ------------
//  The check order in readIo()/writeIo() matters: 0xFF46 (OAM DMA) is tested
//  before the generic 0xFF40-0xFF4B PPU range. Test with:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
// ============================================================================
#include "Bus.h"

#include <cstring>

namespace gb {

// Stores references to all components; the DMA engines get a reference to
// this Bus (for dmaRead/doubleSpeed) and to the PPU (to write OAM/VRAM).
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

// One CPU M-cycle worth of hardware time.
void Bus::tick() {
    // Timer and serial run from the CPU clock: always 4 cycles per M-cycle.
    timer_.tick(4);
    serial_.tick(4);
    // The PPU runs from the fixed 4 MHz clock: in double speed mode one CPU
    // M-cycle only lasts two dots.
    u8 events = ppu_.tick(doubleSpeed_ ? 2 : 4);
    // Entering H-Blank lets an armed H-Blank DMA copy its next 16 bytes.
    if (events & Ppu::EventHBlank) hdma_.onHBlank();
    if (events & Ppu::EventFrame) frameReady_ = true;
    // OAM DMA copies one byte per M-cycle.
    oamDma_.tick();
}

// Called by Cpu::stop() when KEY1 bit 0 is set: toggles the speed and
// clears the armed bit.
void Bus::switchSpeed() {
    doubleSpeed_ = !doubleSpeed_;
    key1_ = 0;
    timer_.write(0xFF04, 0);  // STOP resets DIV
}

u8 Bus::dmaRead(u16 addr) {
    if (addr >= 0xFE00) return 0xFF;
    return read(addr);
}

// Address decoding, in ascending address order (see the map in Bus.h).
u8 Bus::read(u16 addr) {
    if (addr < 0x8000) return cart_.read(addr);
    if (addr < 0xA000) return ppu_.readVram(addr);
    if (addr < 0xC000) return cart_.read(addr);
    if (addr < 0xD000) return wram_[0][addr & 0x0FFF];
    if (addr < 0xE000) return wram_[wramBank_][addr & 0x0FFF];
    if (addr < 0xFE00) return read(static_cast<u16>(addr - 0x2000));  // echo RAM
    if (addr < 0xFEA0) return ppu_.readOam(addr);
    // FEA0-FEFF: the returned value differs between models (Pan Docs,
    // "FEA0-FEFF range"); this emulator returns 0x00 on CGB and 0xFF on DMG.
    if (addr < 0xFF00) return cgb_ ? 0x00 : 0xFF;  // unusable area
    if (addr < 0xFF80) return readIo(addr);
    if (addr < 0xFFFF) return hram_[addr & 0x7F];
    return ic_.readIE();
}

// Same decoding as read(). Writes to 0x0000-0x7FFF do not change ROM: they
// are commands for the MBC (bank switching, RAM enable).
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
        // Echo RAM: write through to C000-DDFF.
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

// I/O register reads (0xFF00-0xFF7F). Registers that do not exist (or are
// CGB-only while in DMG mode) read as 0xFF.
u8 Bus::readIo(u16 addr) {
    if (addr == 0xFF00) return joypad_.read();
    if (addr == 0xFF01 || addr == 0xFF02) return serial_.read(addr);
    if (addr >= 0xFF04 && addr <= 0xFF07) return timer_.read(addr);
    if (addr == 0xFF0F) return ic_.readIF();
    if (addr >= 0xFF10 && addr <= 0xFF3F) return apu_.read(addr);
    // DMA register reads back the last source page written.
    if (addr == 0xFF46) return oamDma_.page();
    if (addr >= 0xFF40 && addr <= 0xFF4B) return ppu_.readRegister(addr);
    if (cgb_) {
        // KEY1: bit 7 current speed, bit 0 armed, unused bits read 1.
        if (addr == 0xFF4D) return static_cast<u8>(0x7E | (doubleSpeed_ ? 0x80 : 0) | (key1_ & 1));
        // VBK (VRAM bank) lives in the PPU.
        if (addr == 0xFF4F) return ppu_.readRegister(addr);
        if (addr >= 0xFF51 && addr <= 0xFF55) return hdma_.read(addr);
        // BCPS/BCPD/OCPS/OCPD palettes and OPRI live in the PPU.
        if (addr >= 0xFF68 && addr <= 0xFF6C) return ppu_.readRegister(addr);
        // SVBK: bits 0-2 WRAM bank, unused bits read 1.
        if (addr == 0xFF70) return static_cast<u8>(0xF8 | wramBank_);
    }
    return 0xFF;
}

// I/O register writes (0xFF00-0xFF7F). Writes to unmapped registers are ignored.
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
        // Start an OAM DMA from address value * 0x100.
        oamDma_.start(value);
    } else if (addr >= 0xFF40 && addr <= 0xFF4B) {
        ppu_.writeRegister(addr, value);
    } else if (cgb_) {
        if (addr == 0xFF4D) {
            // KEY1: only bit 0 (arm the switch) is writable.
            key1_ = value & 1;
        } else if (addr == 0xFF4F || (addr >= 0xFF68 && addr <= 0xFF6C)) {
            ppu_.writeRegister(addr, value);
        } else if (addr >= 0xFF51 && addr <= 0xFF55) {
            hdma_.write(addr, value);
        } else if (addr == 0xFF70) {
            // SVBK: bank 0 cannot be mapped at 0xD000; writing 0 selects bank 1.
            wramBank_ = value & 0x07;
            if (wramBank_ == 0) wramBank_ = 1;
        }
    }
}

}  // namespace gb
