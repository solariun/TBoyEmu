// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Bus.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Declares Bus, the Game Boy memory map. It decodes every 16-bit CPU address
//  and forwards the access to the component that owns it (cartridge, PPU,
//  timer, joypad, serial, APU, interrupt controller), stores work RAM and
//  high RAM itself, implements the CGB-only registers it owns (KEY1 speed
//  switch, SVBK WRAM bank), owns the two DMA engines, and is the "clock" of
//  the machine: tick() advances all clocked hardware by one CPU M-cycle.
//
//  How it fits in
//  --------------
//  Owned by GameBoy and constructed after every component it references.
//  The Cpu calls tick() and then read()/write() for every memory access it
//  makes, so the rest of the machine advances in lock-step with the CPU.
//  OamDma and Hdma (Dma.h) are members and read memory through dmaRead().
//  GameBoy::runFrame() polls consumeFrameReady() to know when to return.
//  Implementation: Bus.cpp.
//
//  Hardware notes
//  --------------
//  Memory map (Pan Docs, "Memory Map"):
//    0000-7FFF cartridge ROM (bank 0 + switchable bank, via the MBC)
//    8000-9FFF VRAM (CGB: 2 banks, VBK 0xFF4F)
//    A000-BFFF cartridge RAM / RTC (via the MBC)
//    C000-CFFF WRAM bank 0
//    D000-DFFF WRAM bank 1 (CGB: banks 1-7 selected by SVBK 0xFF70)
//    E000-FDFF echo RAM (mirror of C000-DDFF)
//    FE00-FE9F OAM (sprite attribute table)
//    FEA0-FEFF unusable
//    FF00-FF7F I/O registers
//    FF80-FFFE high RAM (HRAM)
//    FFFF      IE register
//  KEY1 (0xFF4D, CGB): bit 7 current speed (1 = double), bit 0 switch armed;
//  executing STOP with bit 0 set toggles the speed (Pan Docs, "CGB Registers").
//
//  Contributing
//  ------------
//  Keep the order of operations in tick(): the CPU's access happens after
//  the hardware advanced, which is what M-cycle accurate test ROMs expect.
//  New I/O registers go in readIo()/writeIo(). Test with:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
//  plus instr_timing, dmg-acid2 and cgb-acid2 (README.md, "Testing").
// ============================================================================
#ifndef TBOYEMU_CORE_BUS_H
#define TBOYEMU_CORE_BUS_H

#include "Apu.h"
#include "Cartridge.h"
#include "Dma.h"
#include "InterruptController.h"
#include "Joypad.h"
#include "Ppu.h"
#include "Serial.h"
#include "Timer.h"
#include "Types.h"

namespace gb {

// The memory map.  Routes every CPU access to the component that owns the
// address, owns work RAM / high RAM and the CGB speed switch, and advances
// all clocked hardware one M-cycle at a time (tick()).
// Owned by GameBoy; holds references to the other components (which must
// outlive it) and owns the DMA engines. Single-threaded.
class Bus : private NonCopyable {
public:
    // cgbMode: true when running a CGB-aware cartridge in Color mode; enables
    // the CGB registers (KEY1, VBK, HDMA, palettes, SVBK) and WRAM banks 2-7.
    Bus(Cartridge& cart, Ppu& ppu, Timer& timer, Joypad& joypad, Serial& serial, Apu& apu,
        InterruptController& ic, bool cgbMode);

    // Clears WRAM/HRAM, selects WRAM bank 1 and normal speed.
    void reset();

    // CPU memory access. These do NOT advance time; the Cpu calls tick()
    // first (see Cpu::read8/write8).
    u8 read(u16 addr);
    void write(u16 addr, u8 value);
    // Read for the DMA engines: like read(), but 0xFE00 and above (OAM, I/O,
    // HRAM, IE) read as 0xFF.
    u8 dmaRead(u16 addr);  // used by the DMA engines (no I/O side effects)

    // Advance the machine by one CPU M-cycle.
    // Timer and serial get 4 clock cycles; the PPU gets 4 dots (2 in double
    // speed); H-Blank DMA and frame completion are handled from PPU events;
    // OAM DMA copies one byte.
    void tick();

    // M-cycles the CPU must idle because a VRAM DMA copied data (resets to 0).
    int takeStallCycles() { return hdma_.takeStallCycles(); }
    // true once per completed video frame (V-Blank start, or every 70224
    // dots while the LCD is off); reading it clears the flag.
    bool consumeFrameReady() {
        bool f = frameReady_;
        frameReady_ = false;
        return f;
    }

    // CGB double speed (KEY1 / STOP).
    // doubleSpeed(): current speed. speedSwitchArmed(): KEY1 bit 0 set, so
    // the next STOP must switch speed. switchSpeed(): performs the switch.
    bool doubleSpeed() const { return doubleSpeed_; }
    bool speedSwitchArmed() const { return cgb_ && (key1_ & 1); }
    void switchSpeed();

private:
    // I/O register area 0xFF00-0xFF7F.
    u8 readIo(u16 addr);
    void writeIo(u16 addr, u8 value);

    // Components the bus routes to (owned by GameBoy).
    Cartridge& cart_;
    Ppu& ppu_;
    Timer& timer_;
    Joypad& joypad_;
    Serial& serial_;
    Apu& apu_;
    InterruptController& ic_;
    // Color (CGB) mode enabled.
    bool cgb_;

    // DMA engines (OAM DMA on all models, VRAM DMA on CGB).
    OamDma oamDma_;
    Hdma hdma_;

    // Work RAM: 8 banks of 4 KiB (DMG only uses banks 0 and 1).
    u8 wram_[8][0x1000];
    // High RAM 0xFF80-0xFFFE (the 0x80th byte is unused: 0xFFFF is IE).
    u8 hram_[0x80];
    // SVBK: WRAM bank mapped at 0xD000, 1-7.
    u8 wramBank_;
    // KEY1 bit 0 (speed switch armed).
    u8 key1_;
    // true while the CGB runs in double speed mode.
    bool doubleSpeed_;
    // Set by tick() when the PPU completes a frame; see consumeFrameReady().
    bool frameReady_;
};

}  // namespace gb

#endif
