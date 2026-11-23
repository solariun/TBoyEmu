#include "Joypad.h"

namespace gb {

Joypad::Joypad(InterruptController& ic) : ic_(ic), select_(0x30), pressed_(0) {}

void Joypad::setButton(Button button, bool pressed) {
    u8 bit = static_cast<u8>(1u << static_cast<int>(button));
    bool wasPressed = (pressed_ & bit) != 0;
    if (pressed) {
        pressed_ |= bit;
        if (!wasPressed) ic_.request(irq::Joypad);  // high-to-low transition
    } else {
        pressed_ &= static_cast<u8>(~bit);
    }
}

bool Joypad::isPressed(Button button) const {
    return (pressed_ >> static_cast<int>(button)) & 1;
}

u8 Joypad::read() const {
    u8 lines = 0x0F;  // active low
    if (!(select_ & 0x10)) lines &= static_cast<u8>(~(pressed_ & 0x0F));         // d-pad
    if (!(select_ & 0x20)) lines &= static_cast<u8>(~((pressed_ >> 4) & 0x0F));  // buttons
    return static_cast<u8>(0xC0 | select_ | lines);
}

void Joypad::write(u8 value) { select_ = value & 0x30; }

}  // namespace gb
