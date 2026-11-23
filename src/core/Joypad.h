#ifndef TBOYEMU_CORE_JOYPAD_H
#define TBOYEMU_CORE_JOYPAD_H

#include "InterruptController.h"
#include "Types.h"

namespace gb {

// P1/JOYP register (0xFF00): a 2x4 key matrix selected by bits 4 and 5.
class Joypad : private NonCopyable {
public:
    explicit Joypad(InterruptController& ic);

    void setButton(Button button, bool pressed);
    bool isPressed(Button button) const;

    u8 read() const;
    void write(u8 value);

private:
    InterruptController& ic_;
    u8 select_;   // bits 4-5 as written by the game
    u8 pressed_;  // bit n set => Button(n) held down
};

}  // namespace gb

#endif
