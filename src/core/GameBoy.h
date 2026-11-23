#ifndef TBOYEMU_CORE_GAMEBOY_H
#define TBOYEMU_CORE_GAMEBOY_H

#include <string>

#include "Apu.h"
#include "Bus.h"
#include "Cartridge.h"
#include "Cpu.h"
#include "InterruptController.h"
#include "Joypad.h"
#include "Ppu.h"
#include "Serial.h"
#include "Timer.h"

namespace gb {

// The whole console: owns every hardware component and wires them together.
// Member order matters: components are constructed in declaration order and
// later ones receive references to earlier ones.
class GameBoy : private NonCopyable {
public:
    explicit GameBoy(std::unique_ptr<Cartridge> cartridge);

    void reset();
    void runFrame();  // emulate until the next completed video frame

    void setButton(Button button, bool pressed) { joypad_.setButton(button, pressed); }

    const FrameBuffer& frame() const { return ppu_.frame(); }
    Cartridge& cartridge() { return *cartridge_; }
    const Cartridge& cartridge() const { return *cartridge_; }
    bool cgbMode() const { return cgbMode_; }
    const std::string& serialOutput() const { return serial_.output(); }

private:
    std::unique_ptr<Cartridge> cartridge_;
    bool cgbMode_;
    InterruptController interrupts_;
    Timer timer_;
    Joypad joypad_;
    Serial serial_;
    Apu apu_;
    Ppu ppu_;
    Bus bus_;
    Cpu cpu_;
};

}  // namespace gb

#endif
