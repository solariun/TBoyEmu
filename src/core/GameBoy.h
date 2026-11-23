#ifndef TBOYEMU_CORE_GAMEBOY_H
#define TBOYEMU_CORE_GAMEBOY_H

#include <string>

#include "Apu.h"
#include "Bus.h"
#include "Cartridge.h"
#include "Cpu.h"
#include "InterruptController.h"
#include "Joypad.h"
#include "Machine.h"
#include "Ppu.h"
#include "Serial.h"
#include "Timer.h"

namespace gb {

// The whole console: owns every hardware component and wires them together.
// Member order matters: components are constructed in declaration order and
// later ones receive references to earlier ones.
class GameBoy : public Machine {
public:
    explicit GameBoy(std::unique_ptr<Cartridge> cartridge);

    void reset();

    // Machine
    void runFrame();  // emulate until the next completed video frame
    void setButton(Button button, bool pressed) { joypad_.setButton(button, pressed); }
    const FrameBuffer& frame() const { return ppu_.frame(); }
    System system() const;
    bool colorScreen() const { return cgbMode_; }
    std::string title() const { return cartridge_->header().title; }
    std::string hardware() const { return cartridge_->header().typeName(); }
    bool loadSave(const std::string& path) { return cartridge_->loadSave(path); }
    bool writeSave(const std::string& path) const { return cartridge_->writeSave(path); }
    std::string serialOutput() const { return serial_.output(); }

    Cartridge& cartridge() { return *cartridge_; }
    const Cartridge& cartridge() const { return *cartridge_; }
    bool cgbMode() const { return cgbMode_; }

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
