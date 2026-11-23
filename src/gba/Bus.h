// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Bus.h
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Declares gba::Bus, the GBA memory map: every CPU (and DMA) load, store and
//  instruction fetch goes through it.  The bus routes the address to the
//  right device (BIOS, work RAMs, I/O registers, PPU memories, cartridge ROM,
//  save memory), and adds the cost of the access, in CPU cycles, to a
//  counter that the CPU collects after each instruction.  It also declares
//  gba::Dma, the four DMA channels, which the bus owns.
//  See Bus.cpp for the implementation.
//
//  How it fits in
//  --------------
//  Owned by gba::Gba.  Holds references to the devices it maps (Backup,
//  Gpio, Interrupts, Ppu, Timers, Keypad) and owns the BIOS area, EWRAM,
//  IWRAM and the Dma controller.  The Cpu calls fetch16/fetch32 for opcodes,
//  read*/write* for data, then takeCycles() at the end of each instruction.
//  The Bios emulation also uses read*/write* to implement BIOS calls.
//  Gba::runFrame() calls dma().onHBlank()/onVBlank() when the PPU reports
//  those events.
//
//  Hardware notes
//  --------------
//  Memory map (GBATEK "GBA Memory Map"; region = address bits 24-27):
//    0x00000000-0x00003FFF  BIOS (16 KiB, read-protected, see below)
//    0x02000000-0x0203FFFF  EWRAM, on-board work RAM (256 KiB, 16-bit bus,
//                           2 wait states), mirrored through 0x02FFFFFF
//    0x03000000-0x03007FFF  IWRAM, on-chip work RAM (32 KiB, 32-bit bus,
//                           no wait states), mirrored through 0x03FFFFFF
//    0x04000000-0x040003FE  I/O registers
//    0x05000000-0x050003FF  palette RAM (1 KiB, 16-bit bus)
//    0x06000000-0x06017FFF  VRAM (96 KiB, 16-bit bus)
//    0x07000000-0x070003FF  OAM, sprite attributes (1 KiB)
//    0x08000000-0x09FFFFFF  cartridge ROM, wait state 0
//    0x0A000000-0x0BFFFFFF  same ROM, wait state 1 settings
//    0x0C000000-0x0DFFFFFF  same ROM, wait state 2 settings (EEPROM saves
//                           also live in the 0x0D region)
//    0x0E000000-0x0E00FFFF  SRAM / Flash save memory (8-bit bus)
//  Access timing (GBATEK "GBA Memory Map", "GBA System Control"): an access
//  costs 1 cycle plus the region's wait states.  "N" = non-sequential (a new
//  address), "S" = sequential (the next address after the previous access).
//  On a 16-bit bus a 32-bit access is two 16-bit accesses (N + S).  The
//  cartridge wait states come from WAITCNT (0x04000204).
//  BIOS read protection (GBATEK "BIOS Memory"): the BIOS can only be read
//  while the CPU is executing inside it.  From anywhere else, a read returns
//  the last opcode the CPU prefetched from the BIOS (the "BIOS latch").
//
//  Contributing
//  ------------
//  - The bus never rotates misaligned data; the CPU does that (Cpu::loadWord).
//  - Every access must add its cost to cycles_, or the game runs too fast
//    relative to the PPU and timers.
//  - Test with the jsmolka gba-tests "memory", "sram", "flash64",
//    "flash128" and "bios":
//      make && ./tboyemu --headless --frames 120 --screenshot out.png memory.gba
//    The picture must say "All tests passed".
// ============================================================================
#ifndef TBOYEMU_GBA_BUS_H
#define TBOYEMU_GBA_BUS_H

#include <vector>

#include "Backup.h"
#include "Common.h"
#include "Gpio.h"
#include "Ppu.h"
#include "Timers.h"

namespace gba {

class Bus;

// The four DMA channels (0x040000B0 - 0x040000DF).
//
// Each channel has 12 bytes of registers (GBATEK "GBA DMA Transfers"):
//   +0 DMAxSAD   source address (32 bit, write-only)
//   +4 DMAxDAD   destination address (32 bit, write-only)
//   +8 DMAxCNT_L word count (16 bit, write-only; 0 means the maximum)
//   +A DMAxCNT_H control (16 bit, readable):
//        bits 5-6   destination control: 0 increment, 1 decrement, 2 fixed,
//                   3 increment and reload the address on each repeat
//        bits 7-8   source control: 0 increment, 1 decrement, 2 fixed
//        bit  9     repeat (restart at every H-Blank / V-Blank)
//        bit  10    transfer unit: 0 = 16-bit, 1 = 32-bit
//        bits 12-13 start timing: 0 immediately, 1 V-Blank, 2 H-Blank,
//                   3 special (sound FIFO / video capture; never started here)
//        bit  14    raise an IRQ at the end of the transfer
//        bit  15    enable
// A whole transfer is done at once when triggered (the CPU is stalled on the
// real hardware too, so the game cannot observe the difference except in
// timing).  Owned by the Bus; single-threaded.
class Dma : private NonCopyable {
public:
    // bus: used for the transfers themselves (and their cycle costs).
    // interrupts: to raise the DMA0-DMA3 IRQs.
    Dma(Bus& bus, Interrupts& interrupts);

    // Clears every register: all channels disabled.
    void reset();
    // Register access; only the control register (offset % 12 == 10) reads
    // back, the address and count registers are write-only (read 0).
    u16 read(u32 offset) const;  // offset from 0x040000B0
    // Writing the control register with bit 15 going 0 -> 1 starts the
    // channel (latches the addresses and count; immediate mode runs now).
    void write(u32 offset, u16 value);
    // Called by Gba at the start of each H-Blank / V-Blank: runs every
    // enabled channel whose start timing matches.
    void onHBlank();
    void onVBlank();

private:
    struct Channel {
        // Values as written by the program (the visible registers).
        u32 sourceReg, destReg;
        u16 countReg, control;
        u32 source, dest, count;  // internal (latched) state
    };
    // Latches source/dest/count from the registers; runs the transfer if the
    // start timing is "immediate".
    void start(int ch);
    // Copies count units, raises the IRQ if requested, then either reloads
    // for the next repeat or disables the channel.
    void transfer(int ch);

    Bus& bus_;
    Interrupts& interrupts_;
    Channel channels_[4];
};

// GBA memory map (BIOS, EWRAM, IWRAM, I/O, palette, VRAM, OAM, cartridge
// ROM and save memory) with the access-time (wait state) model used to
// count CPU cycles.  Owns the DMA controller and the work RAMs.
// Lifetime: owned by gba::Gba; the references it holds point to other
// members of Gba.  Single-threaded.  Not copyable.
class Bus : private NonCopyable {
public:
    // gpio may be null (cartridge without a real time clock).
    // rom: the cartridge image (kept by reference, must outlive the Bus).
    Bus(const std::vector<u8>& rom, Backup& backup, Gpio* gpio, Interrupts& interrupts, Ppu& ppu, Timers& timers,
        Keypad& keypad);

    // Clears the work RAMs and the bus-owned registers to their state after
    // the BIOS boot sequence; resets the DMA channels.
    void reset();

    // Data accesses (addresses are aligned by the bus; rotation of
    // misaligned loads is the CPU's job).  Each one adds its cycle cost.
    // 8-bit and 16-bit accesses are charged as non-sequential (N).
    u8 read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);
    void write8(u32 addr, u8 value);
    void write16(u32 addr, u16 value);
    void write32(u32 addr, u32 value);

    // Instruction fetches (sequential fetches from ROM are cheap thanks to
    // the prefetch buffer).
    // sequential: true when addr follows the previous fetch (no branch).
    u16 fetch16(u32 addr, bool sequential);
    u32 fetch32(u32 addr, bool sequential);

    // Returns the cycles accumulated since the last call and resets the
    // counter (called by Cpu::step() once per instruction).
    int takeCycles() {
        int c = cycles_;
        cycles_ = 0;
        return c;
    }
    // Extra cycles not tied to a memory access (DMA overhead).
    void addCycles(int c) { cycles_ += c; }

    // CPU state the bus needs: BIOS read protection and HALTCNT.
    // setPc: address of the instruction being executed; BIOS reads are only
    // allowed while it is inside the BIOS (< 0x4000).
    void setPc(u32 pc) { pc_ = pc; }
    // Forces the value returned by protected BIOS reads (used by the BIOS
    // emulation after a SWI, since no real BIOS code ran).
    void setBiosLatch(u32 value) { biosLatch_ = value; }
    // Executing from BIOS: protected reads later return the opcode fetched
    // two instructions ahead (pc + 8).
    void latchBiosAt(u32 addr) {
        const u8* p = &bios_[addr & 0x3FFC];
        biosLatch_ = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
                     (static_cast<u32>(p[3]) << 24);
    }
    // True once after the program wrote HALTCNT (0x04000301); the CPU then
    // enters HALT.  Reading clears the request.
    bool consumeHaltRequest() {
        bool h = haltRequested_;
        haltRequested_ = false;
        return h;
    }

    Dma& dma() { return dma_; }
    Backup& backup() { return backup_; }

private:
    // I/O register access; off is the offset from 0x04000000 (< 0x400).
    u16 readIo16(u32 offset);
    void writeIo16(u32 offset, u16 value);
    void writeIo8(u32 offset, u8 value);
    // BIOS word read with read protection applied (see readBios32 in .cpp).
    u32 readBios32(u32 addr) const;
    // Recomputes cost16N_/cost16S_/cost32N_ and prefetch_ from waitcnt_.
    void updateWaitStates();
    // Value of a read from an unmapped address (simplified: always 0).
    u32 openBus(u32 addr) const;
    // True when romOffset is one of the cartridge GPIO registers
    // (0x080000C4 data, 0x080000C6 direction, 0x080000C8 control).
    bool gpioAt(u32 romOffset) const { return gpio_ && romOffset >= 0xC4 && romOffset <= 0xC9; }

    const std::vector<u8>& rom_;
    Backup& backup_;
    Gpio* gpio_;  // not owned; null if the cartridge has no RTC
    Interrupts& interrupts_;
    Ppu& ppu_;
    Timers& timers_;
    Keypad& keypad_;
    Dma dma_;

    // 16 KiB.  Zero except for the IRQ dispatcher placed at 0x18 / 0x128
    // by the constructor (see kBiosCode in Bus.cpp).
    std::vector<u8> bios_;
    std::vector<u8> ewram_;  // 256 KiB at 0x02000000
    std::vector<u8> iwram_;  // 32 KiB at 0x03000000
    u16 soundRegs_[0x30];  // 0x60-0xBF: stored, no audio produced
    u16 waitcnt_;          // WAITCNT, 0x04000204 (bits 0-12 and 14 kept)
    u16 rcnt_, siocnt_;    // RCNT 0x04000134, SIOCNT 0x04000128 (no link cable)
    u8 postflg_;           // POSTFLG 0x04000300 bit 0 (1 = booted)

    // Cycle cost per region (addr >> 24): 16-bit non-sequential,
    // 16-bit sequential, 32-bit non-sequential.
    // Each value is 1 + wait states (see updateWaitStates()).
    int cost16N_[16], cost16S_[16], cost32N_[16];
    bool prefetch_;         // WAITCNT bit 14: game pak prefetch buffer enabled
    int cycles_;            // cycles accumulated since the last takeCycles()
    u32 pc_;                // address of the instruction being executed
    u32 biosLatch_;         // value returned by protected BIOS reads
    bool haltRequested_;    // HALTCNT was written; see consumeHaltRequest()
    // ROM size rounded up to a power of two, minus 1.  Computed in the
    // constructor but not used by the current address decoding.
    u32 romMask_;
    bool eepromLarge_;  // >16 MiB ROM: EEPROM only at the top of 0x0D
};

}  // namespace gba

#endif
