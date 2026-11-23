// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Serial.cpp
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Implements the link port declared in Serial.h: starting a transfer,
//  counting down its duration, completing it with 0xFF received (nothing is
//  connected) and recording the bytes the game sends.
//
//  How it fits in
//  --------------
//  Bus::tick() calls Serial::tick(4) once per CPU M-cycle; Bus::readIo()/
//  writeIo() call read()/write() for 0xFF01-0xFF02. On completion it requests
//  irq::Serial from the InterruptController. See Serial.h for the interface.
//
//  Hardware notes
//  --------------
//  Pan Docs, "Serial Data Transfer (Link Cable)". A transfer starts when SC
//  is written with bit 7 (start) and bit 0 (internal clock) both set. At
//  8192 Hz each of the 8 bits takes 512 cycles of the 4.194304 MHz clock.
//  Unused SC bits (2-6) read as 1.
//
//  Contributing
//  ------------
//  The byte is recorded when the transfer starts (that is when SB holds the
//  value being sent). Test with:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
// ============================================================================
#include "Serial.h"

namespace gb {

// Post-boot values: SB = 0, SC = 0x7E (no transfer, external clock).
Serial::Serial(InterruptController& ic) : ic_(ic), sb_(0), sc_(0x7E), remaining_(0) {}

void Serial::tick(int cycles) {
    // Nothing to do unless a transfer is running.
    if (remaining_ <= 0) return;
    remaining_ -= cycles;
    // Transfer complete: receive 0xFF, clear SC bit 7 (busy) and interrupt.
    if (remaining_ <= 0) {
        sb_ = 0xFF;  // nothing connected
        sc_ &= 0x7F;
        ic_.request(irq::Serial);
    }
}

u8 Serial::read(u16 addr) const {
    if (addr == 0xFF01) return sb_;
    // SC: bits 2-6 are unused and read as 1.
    return static_cast<u8>(sc_ | 0x7C);
}

void Serial::write(u16 addr, u8 value) {
    if (addr == 0xFF01) {
        sb_ = value;
        return;
    }
    sc_ = value;
    // Only internally clocked transfers make progress; with the external
    // clock the transfer waits forever for a partner that does not exist.
    if ((value & 0x81) == 0x81) {  // start, internal clock
        // Record the outgoing byte for --headless output (bounded to 64 KiB
        // so a game that streams link data cannot grow memory without limit).
        if (output_.size() < 1 << 16) output_.push_back(static_cast<char>(sb_));
        remaining_ = 8 * 512;  // 8 bits at 8192 Hz
    }
}

}  // namespace gb
