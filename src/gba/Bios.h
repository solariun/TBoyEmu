// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Bios.h
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Declares gba::Bios, a high-level emulation (HLE) of the GBA BIOS system
//  calls.  Games call BIOS functions with the SWI instruction ("SWI 0x0B" =
//  CpuSet, "SWI 0x05" = VBlankIntrWait, ...).  Instead of running the
//  original (copyrighted) BIOS code, TBoyEmu implements each function in
//  C++ with the same register inputs and outputs, so no BIOS file is needed.
//  See Bios.cpp for the implementation of every call.
//
//  How it fits in
//  --------------
//  Owned by gba::Gba and registered with Cpu::setSwiHandler().  When the CPU
//  decodes a SWI it calls Bios::swi(number) right away, without changing
//  the CPU mode.  The handler reads its arguments from r0-r3 via
//  Cpu::reg(), uses the Bus for memory, writes results with Cpu::setReg(),
//  and returns; execution continues after the SWI instruction.
//  The only BIOS code that really executes is the IRQ dispatcher placed at
//  0x18 / 0x128 by Bus.cpp (kBiosCode); this class does not handle IRQs.
//
//  Hardware notes
//  --------------
//  GBATEK "BIOS Functions" (function list and parameters per call),
//  "BIOS Halt Functions" (IntrWait and the 0x03007FF8 flags),
//  "BIOS Memory" (read protection latch value after a SWI: 0xE3A02004).
//  Arguments: r0, r1, r2, r3; results in r0 (and r1, r3 for Div).
//  The BIOS uses part of IWRAM: 0x03007FF8 (IRQ flags for IntrWait, written
//  by the game's IRQ handler), 0x03007FFA (SoftReset flag), 0x03007FFC
//  (address of the game's IRQ handler).
//
//  Contributing
//  ------------
//  - Keep the register contract of each call identical to GBATEK; games and
//    the jsmolka "bios" test depend on exact results (e.g. Div's r3).
//  - Sound driver calls (0x19-0x24, 0x28-0x2A) are no-ops: there is no audio.
//  - Test: ./tboyemu --headless --frames 120 --screenshot out.png bios.gba
//    must show "All tests passed".
// ============================================================================
#ifndef TBOYEMU_GBA_BIOS_H
#define TBOYEMU_GBA_BIOS_H

#include <vector>

#include "Bus.h"
#include "Cpu.h"

namespace gba {

// High-level emulation of the GBA BIOS system calls (SWI 0x00 - 0x2A), so
// no copyrighted BIOS image is needed.  Each call is implemented in C++
// with the same inputs/outputs as the original routines.
// Holds references to the Cpu, Bus and Interrupts owned by gba::Gba.
// Single-threaded.  The only state is waiting_ (IntrWait in progress).
class Bios : public SwiHandler {
public:
    Bios(Cpu& cpu, Bus& bus, Interrupts& interrupts);

    // Executes BIOS function `number` (the SWI comment value).  Unknown or
    // unimplemented numbers do nothing.  Sets the BIOS read-protection latch
    // to the value a real BIOS leaves after a SWI.
    void swi(u32 number);

private:
    // 0x00: clear the top of IWRAM, reset the CPU, restart the game.
    void softReset();
    // 0x01: clear the memory areas / registers selected by flags (r0).
    void registerRamReset(u32 flags);
    // 0x04 / 0x05: wait for one of the interrupts in flags (see Bios.cpp).
    void intrWait(bool discardOld, u16 flags);
    // 0x06 / 0x07: signed division; r0 = quotient, r1 = remainder,
    // r3 = |quotient|.
    void divide(s32 numerator, s32 denominator);
    // 0x09: arctangent of r0 (1.14 fixed point).
    void arcTan();
    // 0x0A: angle of the vector (r0 = x, r1 = y), 0x0000-0xFFFF = 0-2*pi.
    void arcTan2();
    // 0x0B: memory copy / fill in 16- or 32-bit units.
    void cpuSet();
    // 0x0C: memory copy / fill in 32-bit units, blocks of 8 words.
    void cpuFastSet();
    // 0x0E: compute background rotation/scaling parameters.
    void bgAffineSet();
    // 0x0F: compute sprite rotation/scaling parameters.
    void objAffineSet();
    // 0x10: expand pixels from 1/2/4/8 bits to 1/2/4/8/16/32 bits.
    void bitUnpack();
    // 0x11 / 0x12: LZ77 decompression (vram: write in 16-bit units).
    void lz77(bool vram);
    // 0x13: Huffman decompression (4- or 8-bit symbols, 32-bit writes).
    void huffman();
    // 0x14 / 0x15: run-length decompression.
    void runLength(bool vram);
    // 0x16 / 0x17 / 0x18: undo delta encoding (8-bit or 16-bit units).
    void diffUnfilter(bool sixteenBit, bool vram);
    // 0x1F: frequency of a MIDI key for the sound driver.
    void midiKey2Freq();

    // Writes decompressed bytes to dst: byte by byte for WRAM ("Wram"
    // variants), halfword by halfword for VRAM, which ignores byte writes.
    void writeOutput(u32 dst, const std::vector<u8>& data, bool vram);

    Cpu& cpu_;
    Bus& bus_;
    Interrupts& interrupts_;
    bool waiting_;  // inside IntrWait / VBlankIntrWait
};

}  // namespace gba

#endif
