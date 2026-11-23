// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Apu.cpp
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Implements the sound register stub declared in Apu.h: storage for
//  0xFF10-0xFF3F, the per-register "bits that read back as 1" masks, and
//  the NR52 power switch. No audio samples are generated.
//
//  How it fits in
//  --------------
//  Bus::readIo()/writeIo() call Apu::read()/write() for 0xFF10-0xFF3F.
//  GameBoy::reset() calls Apu::reset(). See Apu.h for the interface.
//
//  Hardware notes
//  --------------
//  Read masks follow Pan Docs, "Audio Registers" (and the commonly used
//  table of write-only bits): e.g. NRx4 only exposes bit 6 (length enable),
//  NRx3 (frequency low) is write-only and reads 0xFF, NR52 bits 4-6 always
//  read 1. Channel "on" flags (NR52 bits 0-3) always read 0 here because no
//  channel ever plays. Wave RAM (0xFF30-0xFF3F) reads back as written.
//
//  Contributing
//  ------------
//  Indexes in this file are relative to 0xFF10 (index 0x16 is NR52 at
//  0xFF26). If sound is ever added, keep these masks: games rely on them.
//  Test with: make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
// ============================================================================
#include "Apu.h"

#include <cstring>

namespace gb {

namespace {
// Bits that always read back as 1 for 0xFF10..0xFF2F.
// One entry per address, index = addr - 0xFF10. Entries for unmapped
// addresses (0xFF15, 0xFF1F, 0xFF27-0xFF2F) are 0xFF so they read as 0xFF.
// NR52 (index 0x16) is handled separately in read().
const u8 kReadMask[0x20] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF,  // NR10-NR14
    0xFF, 0x3F, 0x00, 0xFF, 0xBF,  // ----, NR21-NR24
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF,  // NR30-NR34
    0xFF, 0xFF, 0x00, 0x00, 0xBF,  // ----, NR41-NR44
    0x00, 0x00, 0x70,              // NR50-NR52
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
}  // namespace

Apu::Apu() { reset(); }

// Post-boot state: every register 0 except NR52, which reports power on.
void Apu::reset() {
    std::memset(regs_, 0, sizeof(regs_));
    regs_[0x16] = 0x80;  // NR52: powered on after boot
    powered_ = true;
}

u8 Apu::read(u16 addr) const {
    // i is the register index relative to 0xFF10; out of range reads 0xFF.
    int i = addr - 0xFF10;
    if (i < 0 || i >= 0x30) return 0xFF;
    if (i >= 0x20) return regs_[i];  // wave RAM
    // NR52: no channel is ever reported as playing.
    // Bit 7 = power, bits 4-6 always 1, bits 0-3 (channel active) = 0.
    if (i == 0x16) return static_cast<u8>((powered_ ? 0x80 : 0x00) | 0x70);
    // Other registers: write-only / unused bits are forced to 1.
    return static_cast<u8>(regs_[i] | kReadMask[i]);
}

void Apu::write(u16 addr, u8 value) {
    int i = addr - 0xFF10;
    if (i < 0 || i >= 0x30) return;
    // Wave RAM is always writable (it is not cleared by power off either).
    if (i >= 0x20) {
        regs_[i] = value;
        return;
    }
    // NR52: only bit 7 (power) is writable. Powering off clears every
    // register from NR10 (0xFF10) up to NR51 (0xFF25), i.e. the first 0x16 bytes.
    if (i == 0x16) {
        powered_ = (value & 0x80) != 0;
        if (!powered_) std::memset(regs_, 0, 0x16);
        return;
    }
    // While powered off the other registers ignore writes.
    if (powered_) regs_[i] = value;
}

}  // namespace gb
