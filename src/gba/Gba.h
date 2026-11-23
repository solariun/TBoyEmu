#ifndef TBOYEMU_GBA_GBA_H
#define TBOYEMU_GBA_GBA_H

#include <string>
#include <vector>

#include "../core/Machine.h"
#include "Backup.h"
#include "Bios.h"
#include "Bus.h"
#include "Cpu.h"
#include "Gpio.h"
#include "Ppu.h"
#include "Timers.h"

namespace gba {

// The Game Boy Advance: owns every component (declaration order = wiring
// order) and runs them in lockstep, one CPU instruction at a time.
class Gba : public gb::Machine {
public:
    explicit Gba(std::vector<u8> rom);

    void runFrame();
    void setButton(gb::Button button, bool pressed) { keypad_.setButton(button, pressed); }
    const gb::FrameBuffer& frame() const { return ppu_.frame(); }
    gb::System system() const { return gb::System::GameBoyAdvance; }
    bool colorScreen() const { return true; }
    std::string title() const { return title_; }
    std::string hardware() const { return std::string(backup_->name()) + (gpio_ ? "+RTC" : ""); }
    bool loadSave(const std::string& path) { return backup_->load(path); }
    bool writeSave(const std::string& path) const { return backup_->save(path); }

private:
    std::vector<u8> rom_;
    std::unique_ptr<Backup> backup_;
    std::unique_ptr<Gpio> gpio_;  // real time clock, when the cartridge has one
    std::string title_;
    Interrupts interrupts_;
    Ppu ppu_;
    Timers timers_;
    Keypad keypad_;
    Bus bus_;
    Cpu cpu_;
    Bios bios_;
};

}  // namespace gba

#endif
