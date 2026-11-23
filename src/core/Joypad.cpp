// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Joypad.cpp
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Implements the P1/JOYP key matrix declared in Joypad.h: tracking held
//  buttons, raising the Joypad interrupt on a new press, and computing the
//  active-low value of 0xFF00 from the selected rows.
//
//  How it fits in
//  --------------
//  GameBoy::setButton() -> Joypad::setButton(); Bus::readIo()/writeIo() ->
//  read()/write() for 0xFF00. See Joypad.h for the register layout.
//
//  Hardware notes
//  --------------
//  Pan Docs, "Joypad Input". The hardware raises the Joypad interrupt when
//  one of the P1 input lines goes from high to low, which only happens for
//  buttons in a selected row. This implementation simplifies that: any new
//  press requests the interrupt, whatever rows are selected. That is enough
//  to wake games that wait in HALT/STOP for a key.
//
//  Contributing
//  ------------
//  pressed_ uses the Button enum values as bit positions: bits 0-3 are the
//  d-pad (Right, Left, Up, Down), bits 4-7 the action buttons (A, B, Select,
//  Start), which matches the P1 low nibble order of each row.
// ============================================================================
#include "Joypad.h"

namespace gb {

// Post-boot: no row selected (bits 4-5 = 1), no button held.
Joypad::Joypad(InterruptController& ic) : ic_(ic), select_(0x30), pressed_(0) {}

void Joypad::setButton(Button button, bool pressed) {
    if (static_cast<int>(button) >= 8) return;  // L/R: Game Boy Advance only
    u8 bit = static_cast<u8>(1u << static_cast<int>(button));
    bool wasPressed = (pressed_ & bit) != 0;
    if (pressed) {
        pressed_ |= bit;
        // Interrupt only on the press itself, not on repeated "still held" calls.
        if (!wasPressed) ic_.request(irq::Joypad);  // high-to-low transition
    } else {
        pressed_ &= static_cast<u8>(~bit);
    }
}

bool Joypad::isPressed(Button button) const {
    return (pressed_ >> static_cast<int>(button)) & 1;
}

u8 Joypad::read() const {
    // Start with all four lines high (nothing pressed), then pull low the
    // lines of the pressed buttons in each selected row (select bit = 0).
    u8 lines = 0x0F;  // active low
    if (!(select_ & 0x10)) lines &= static_cast<u8>(~(pressed_ & 0x0F));         // d-pad
    if (!(select_ & 0x20)) lines &= static_cast<u8>(~((pressed_ >> 4) & 0x0F));  // buttons
    // Bits 6-7 unused (read 1), bits 4-5 echo the selection.
    return static_cast<u8>(0xC0 | select_ | lines);
}

// Only the row-select bits 4-5 are writable; the key lines are inputs.
void Joypad::write(u8 value) { select_ = value & 0x30; }

}  // namespace gb
