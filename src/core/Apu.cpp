#include "Apu.h"

#include <cstring>

namespace gb {

namespace {
// Bits that always read back as 1 for 0xFF10..0xFF2F.
const u8 kReadMask[0x20] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF,  // NR10-NR14
    0xFF, 0x3F, 0x00, 0xFF, 0xBF,  // ----, NR21-NR24
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF,  // NR30-NR34
    0xFF, 0xFF, 0x00, 0x00, 0xBF,  // ----, NR41-NR44
    0x00, 0x00, 0x70,              // NR50-NR52
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
}  // namespace

Apu::Apu() { reset(); }

void Apu::reset() {
    std::memset(regs_, 0, sizeof(regs_));
    regs_[0x16] = 0x80;  // NR52: powered on after boot
    powered_ = true;
}

u8 Apu::read(u16 addr) const {
    int i = addr - 0xFF10;
    if (i < 0 || i >= 0x30) return 0xFF;
    if (i >= 0x20) return regs_[i];  // wave RAM
    // NR52: no channel is ever reported as playing.
    if (i == 0x16) return static_cast<u8>((powered_ ? 0x80 : 0x00) | 0x70);
    return static_cast<u8>(regs_[i] | kReadMask[i]);
}

void Apu::write(u16 addr, u8 value) {
    int i = addr - 0xFF10;
    if (i < 0 || i >= 0x30) return;
    if (i >= 0x20) {
        regs_[i] = value;
        return;
    }
    if (i == 0x16) {
        powered_ = (value & 0x80) != 0;
        if (!powered_) std::memset(regs_, 0, 0x16);
        return;
    }
    if (powered_) regs_[i] = value;
}

}  // namespace gb
