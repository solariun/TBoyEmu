// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/InterruptController.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Holds the two Game Boy interrupt registers, IF (interrupt flags, 0xFF0F)
//  and IE (interrupt enable, 0xFFFF), and offers a small API to raise,
//  acknowledge and query interrupts. Header-only: every method is inline.
//
//  How it fits in
//  --------------
//  Owned by GameBoy. Timer, Joypad, Serial and Ppu call request() when they
//  raise an interrupt; the Bus routes CPU reads/writes of 0xFF0F and 0xFFFF
//  to readIF/writeIF/readIE/writeIE; the Cpu calls pending() before each
//  instruction (and in HALT) and acknowledge() when it dispatches one.
//  The master enable flag IME is not here: it is CPU state (Cpu::ime_).
//
//  Hardware notes
//  --------------
//  IF/IE bit layout (Pan Docs, "Interrupts"):
//    bit 0 V-Blank, bit 1 LCD STAT, bit 2 Timer, bit 3 Serial, bit 4 Joypad.
//  Only the low 5 bits exist in IF; its upper 3 bits always read as 1.
//  IE is a full 8-bit read/write register (the upper bits are stored but
//  never trigger anything). An interrupt is pending when the same bit is set
//  in both IF and IE; it is serviced only if IME is also set, but a pending
//  interrupt always wakes the CPU from HALT.
//
//  Contributing
//  ------------
//  Components must never write IF through the memory map; always use
//  request() so the masking stays in one place. Test interrupt changes with
//  Blargg cpu_instrs (test 02 "interrupts"):
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
// ============================================================================
#ifndef TBOYEMU_CORE_INTERRUPTCONTROLLER_H
#define TBOYEMU_CORE_INTERRUPTCONTROLLER_H

#include "Types.h"

namespace gb {

// IF (0xFF0F) and IE (0xFFFF): every component that raises interrupts
// talks to this object instead of poking at memory.
// Owned by GameBoy, referenced by the components; single-threaded.
class InterruptController : private NonCopyable {
public:
    // Power-on value of IF; GameBoy::reset() then sets the post-boot values.
    InterruptController() : if_(0xE1), ie_(0) {}

    // Sets the given irq:: bit(s) in IF. Bits above 4 are ignored.
    void request(u8 mask) { if_ |= (mask & 0x1F); }
    // Clears the given bit(s) in IF (done by the CPU when it jumps to a handler).
    void acknowledge(u8 mask) { if_ &= static_cast<u8>(~mask); }
    // Bit mask of interrupts that are both requested and enabled (IF & IE),
    // limited to the 5 real sources. Non-zero means "wake from HALT".
    u8 pending() const { return if_ & ie_ & 0x1F; }

    // CPU-visible register access (called by the Bus).
    // IF reads with bits 5-7 set, as on hardware.
    u8 readIF() const { return static_cast<u8>(if_ | 0xE0); }
    void writeIF(u8 v) { if_ = v & 0x1F; }
    u8 readIE() const { return ie_; }
    void writeIE(u8 v) { ie_ = v; }

private:
    // IF register; after the first write only bits 0-4 are stored.
    u8 if_;
    // IE register, all 8 bits stored as written.
    u8 ie_;
};

}  // namespace gb

#endif
