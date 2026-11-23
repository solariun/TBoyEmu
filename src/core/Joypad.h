// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Joypad.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Declares Joypad, the Game Boy P1/JOYP register (0xFF00). It keeps the set
//  of buttons currently held (set by the frontend) and the two select bits
//  written by the game, and builds the value the game reads back.
//
//  How it fits in
//  --------------
//  Owned by GameBoy. GameBoy::setButton() (called by the frontend through
//  the Machine interface) calls setButton() here; the Bus routes CPU reads
//  and writes of 0xFF00 to read()/write(). Pressing a button requests the
//  Joypad interrupt from the InterruptController. Implementation: Joypad.cpp.
//
//  Hardware notes
//  --------------
//  Pan Docs, "Joypad Input". P1 layout (0 = selected / pressed):
//    bit 5  select action buttons (A, B, Select, Start)
//    bit 4  select direction buttons (d-pad)
//    bit 3  Down  or Start
//    bit 2  Up    or Select
//    bit 1  Left  or B
//    bit 0  Right or A
//  Bits 6-7 are unused and read as 1. If both groups are selected, the low
//  nibble is the AND of both rows.
//
//  Contributing
//  ------------
//  Remember the register is active low: a pressed button reads 0. The
//  Button enum value doubles as the bit index in pressed_. Test input with:
//      ./tboyemu --headless --frames 600 --input 300:start:5 --screenshot shot.png game.zip
// ============================================================================
#ifndef TBOYEMU_CORE_JOYPAD_H
#define TBOYEMU_CORE_JOYPAD_H

#include "InterruptController.h"
#include "Types.h"

namespace gb {

// P1/JOYP register (0xFF00): a 2x4 key matrix selected by bits 4 and 5.
// Owned by GameBoy; holds a reference to the shared InterruptController.
class Joypad : private NonCopyable {
public:
    explicit Joypad(InterruptController& ic);

    // Called by the frontend (via GameBoy) when a key changes state.
    // L and R (GBA only) are ignored. A new press requests irq::Joypad.
    void setButton(Button button, bool pressed);
    // true while the button is held down.
    bool isPressed(Button button) const;

    // CPU read of P1: 0xC0 | select bits | active-low key lines.
    u8 read() const;
    // CPU write of P1: only bits 4-5 (row select) are writable.
    void write(u8 value);

private:
    InterruptController& ic_;
    u8 select_;   // bits 4-5 as written by the game
    u8 pressed_;  // bit n set => Button(n) held down
};

}  // namespace gb

#endif
