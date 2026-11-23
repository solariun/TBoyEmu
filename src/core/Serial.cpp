#include "Serial.h"

namespace gb {

Serial::Serial(InterruptController& ic) : ic_(ic), sb_(0), sc_(0x7E), remaining_(0) {}

void Serial::tick(int cycles) {
    if (remaining_ <= 0) return;
    remaining_ -= cycles;
    if (remaining_ <= 0) {
        sb_ = 0xFF;  // nothing connected
        sc_ &= 0x7F;
        ic_.request(irq::Serial);
    }
}

u8 Serial::read(u16 addr) const {
    if (addr == 0xFF01) return sb_;
    return static_cast<u8>(sc_ | 0x7C);
}

void Serial::write(u16 addr, u8 value) {
    if (addr == 0xFF01) {
        sb_ = value;
        return;
    }
    sc_ = value;
    if ((value & 0x81) == 0x81) {  // start, internal clock
        if (output_.size() < 1 << 16) output_.push_back(static_cast<char>(sb_));
        remaining_ = 8 * 512;  // 8 bits at 8192 Hz
    }
}

}  // namespace gb
