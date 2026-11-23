// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Gba.h
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Declares gba::Gba, the complete Game Boy Advance console. It implements the gb::Machine
//  interface shared with the Game Boy core (run a frame, set buttons, get the frame, load
//  and write the battery save), owns every GBA hardware component and connects them.
//
//  How it fits in
//  --------------
//  - Created by emu::createMachine() (MachineFactory.cpp) when the ROM is detected as a GBA
//    game; the frontend (EmulatorSession / HeadlessRunner) only sees the gb::Machine
//    interface.
//  - Component tree (README.md "Architecture"):
//      Gba
//       +- Backup      save chip (SRAM / Flash / EEPROM), detected from the ROM
//       +- Gpio        optional cartridge real time clock
//       +- Interrupts  IE / IF / IME
//       +- Ppu         video: timing, rendering, palette/VRAM/OAM
//       +- Timers, Keypad
//       +- Bus         memory map, wait states, I/O registers (+ Dma)
//       +- Cpu         ARM7TDMI
//       +- Bios        high-level emulation of the BIOS calls (SWI handler of the Cpu)
//  Implementation: Gba.cpp.
//
//  Hardware notes
//  --------------
//  ROM header (GBATEK "GBA Cartridge Header"): title at 0xA0 (12 bytes, uppercase ASCII),
//  game code at 0xAC (4 bytes), fixed value 0x96 at 0xB2. The header ends at 0xC0.
//
//  Contributing
//  ------------
//  - Member DECLARATION order is the construction order, and each component receives
//    references to those constructed before it: never reorder the members below.
//  - Test with the jsmolka gba-tests ROMs and a few games in headless mode (README.md):
//      make && ./tboyemu --headless --frames 600 --screenshot out.png rom.gba
// ============================================================================
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
// Lifetime: owned by the frontend through std::unique_ptr<gb::Machine>; single-threaded.
class Gba : public gb::Machine {
public:
    // Takes ownership of the ROM image (moved in). Throws std::runtime_error if the image is
    // smaller than the 0xC0-byte header. Chooses the save chip and the RTC from the ROM.
    explicit Gba(std::vector<u8> rom);

    // Emulates until the PPU completes a frame (V-Blank start), about 280896 cycles.
    void runFrame();
    // gb::Machine interface: forwards buttons to KEYINPUT, exposes the PPU's front buffer.
    void setButton(gb::Button button, bool pressed) { keypad_.setButton(button, pressed); }
    const gb::FrameBuffer& frame() const { return ppu_.frame(); }
    gb::System system() const { return gb::System::GameBoyAdvance; }
    bool colorScreen() const { return true; }
    // Game title from the ROM header (non-printable characters shown as '?').
    std::string title() const { return title_; }
    // Short hardware description for the status bar, e.g. "FLASH 128K+RTC".
    std::string hardware() const { return std::string(backup_->name()) + (gpio_ ? "+RTC" : ""); }
    // Battery save I/O (raw chip dump in the .sav file); see Backup::load()/save().
    bool loadSave(const std::string& path) { return backup_->load(path); }
    bool writeSave(const std::string& path) const { return backup_->save(path); }

private:
    // Cartridge ROM (at most 32 MiB); Bus reads it by reference.
    std::vector<u8> rom_;
    // Save chip; never null (SRAM by default).
    std::unique_ptr<Backup> backup_;
    std::unique_ptr<Gpio> gpio_;  // real time clock, when the cartridge has one
    // Title decoded from the ROM header.
    std::string title_;
    // Hardware components, in wiring order (see the Contributing note in the file header).
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
