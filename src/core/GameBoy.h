// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/GameBoy.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Declares GameBoy, the complete Game Boy / Game Boy Color console. It owns
//  every hardware component (cartridge, interrupt controller, timer, joypad,
//  serial port, APU stub, PPU, bus and CPU), wires them together by
//  reference, and implements the gb::Machine interface that the frontend
//  uses (run a frame, set buttons, get the frame, battery saves).
//
//  How it fits in
//  --------------
//  Created by MachineFactory (src/MachineFactory.cpp) for .gb/.gbc ROMs and
//  driven by the frontend's EmulatorSession through Machine. Component
//  graph (see README.md, "Architecture"):
//    GameBoy -> Cartridge -> Mbc
//            -> InterruptController, Timer, Joypad, Serial, Apu, Ppu
//            -> Bus (memory map, owns OamDma and Hdma)
//            -> Cpu (SM83, ticks the Bus on every memory access)
//  Implementation: GameBoy.cpp.
//
//  Hardware notes
//  --------------
//  The emulator skips the boot ROM: reset() puts the CPU registers, DIV and
//  the I/O registers in the state the boot ROM leaves them (Pan Docs,
//  "Power Up Sequence"). A CGB-aware cartridge (header 0x0143 bit 7 set)
//  runs in Color mode; anything else runs in DMG mode with the built-in
//  green palette.
//
//  Contributing
//  ------------
//  Member declaration order is the construction order: a component may only
//  take references to members declared above it. Test with:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
//  and instr_timing, dmg-acid2, cgb-acid2 (README.md, "Testing").
// ============================================================================
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
// Owned by the frontend through std::unique_ptr<Machine>; single-threaded.
class GameBoy : public Machine {
public:
    // Takes ownership of the cartridge; Color mode is chosen from its header.
    explicit GameBoy(std::unique_ptr<Cartridge> cartridge);

    // Puts the whole machine in the post-boot-ROM state (RAM cleared).
    // Battery RAM in the cartridge is not touched.
    void reset();

    // Machine
    void runFrame();  // emulate until the next completed video frame
    void setButton(Button button, bool pressed) { joypad_.setButton(button, pressed); }
    const FrameBuffer& frame() const { return ppu_.frame(); }
    // GB, GB/GBC or GBC, from the CGB flag of the cartridge header.
    System system() const;
    bool colorScreen() const { return cgbMode_; }
    std::string title() const { return cartridge_->header().title; }
    // Mapper name, e.g. "MBC5" (see CartridgeHeader::typeName()).
    std::string hardware() const { return cartridge_->header().typeName(); }
    bool loadSave(const std::string& path) { return cartridge_->loadSave(path); }
    bool writeSave(const std::string& path) const { return cartridge_->writeSave(path); }
    std::string serialOutput() const { return serial_.output(); }

    // Direct access for tools and tests.
    Cartridge& cartridge() { return *cartridge_; }
    const Cartridge& cartridge() const { return *cartridge_; }
    // true when running in Game Boy Color mode.
    bool cgbMode() const { return cgbMode_; }

private:
    // Declared (and therefore constructed) in dependency order.
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
