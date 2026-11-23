// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Common.h
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Shared vocabulary of the whole GBA core:
//   - integer type aliases (u8/u16/u32/u64 and the signed s8/s16/s32/s64), re-using
//     the Game Boy core's definitions from core/Types.h so both cores agree;
//   - global timing constants of the GBA video/CPU clock (cycles per scanline, per frame,
//     screen size);
//   - the interrupt bit masks of the IE / IF registers (namespace gba::irq);
//   - the Interrupts class, the tiny model of IE, IF and IME that every other GBA
//     component uses to raise interrupts.
//
//  How it fits in
//  --------------
//  Every file in src/gba includes this header. Gba (Gba.h) owns the single Interrupts
//  instance and hands a reference to Ppu, Timers, Keypad, Bus, Dma, Cpu and Bios. Devices
//  call Interrupts::request(); the Cpu polls shouldInterrupt() between instructions; the
//  Bus maps reads/writes of 0x04000200 (IE), 0x04000202 (IF) and 0x04000208 (IME) onto
//  the accessors below.
//
//  Hardware notes
//  --------------
//  - CPU clock: 2^24 Hz = 16.78 MHz. One scanline is 1232 cycles: 960 cycles of H-Draw
//    (240 pixels * 4 cycles) followed by 272 cycles of H-Blank. A frame is 228 lines:
//    160 visible + 68 lines of V-Blank, i.e. 280896 cycles (about 59.73 frames/s).
//    See GBATEK "LCD Dimensions and Timings".
//  - IE (0x04000200) and IF (0x04000202) share the same 14-bit layout (bits 0-13); IF bits
//    are cleared by WRITING 1 to them ("acknowledge"). IME (0x04000208) bit 0 is the master
//    enable. See GBATEK "GBA Interrupt Control".
//
//  Contributing
//  ------------
//  - Keep this header dependency-free (only <cstdint> and core/Types.h): it is included
//    everywhere, so adding heavy includes slows the whole build.
//  - If you change a timing constant, Ppu, Timers and the frame loop in Gba.cpp all depend
//    on it; re-run the jsmolka gba-tests ROMs listed in README.md and a game in headless
//    mode, e.g.  make && ./tboyemu --headless --frames 600 --screenshot out.png rom.gba
// ============================================================================
#ifndef TBOYEMU_GBA_COMMON_H
#define TBOYEMU_GBA_COMMON_H

#include <cstdint>

#include "../core/Types.h"

namespace gba {

// Integer aliases. The unsigned ones and s8 come from the Game Boy core (core/Types.h) so
// that shared code (FrameBuffer, Machine interface) uses exactly the same types; the wider
// signed types are only needed by the GBA (affine math, ARM arithmetic) and defined here.
using gb::u8;
using gb::u16;
using gb::u32;
using gb::u64;
using gb::s8;
typedef std::int16_t s16;
typedef std::int32_t s32;
typedef std::int64_t s64;
// NonCopyable: base class that deletes copy construction/assignment (hardware components
// are wired together by reference, so copying one would leave dangling references).
// makeUnique: C++11 replacement for std::make_unique.
using gb::NonCopyable;
using gb::makeUnique;

// 16.78 MHz CPU, 1232 cycles per scanline, 228 scanlines per frame.
// All values are in CPU cycles (one cycle = 1/16777216 s) unless stated otherwise.
// kCpuClockHz: 2^24 Hz.
// kCyclesPerLine: full scanline, H-Draw + H-Blank (308 dots * 4 cycles).
// kHDrawCycles: visible part of a line (240 dots * 4 cycles); H-Blank starts here.
// kLinesPerFrame: VCOUNT runs 0..227. kVisibleLines: lines 0..159 are drawn, 160..227 are V-Blank.
// kScreenWidth / kScreenHeight: LCD size in pixels.
const int kCpuClockHz = 16777216;
const int kCyclesPerLine = 1232;
const int kHDrawCycles = 960;
const int kLinesPerFrame = 228;
const int kVisibleLines = 160;
const int kCyclesPerFrame = kCyclesPerLine * kLinesPerFrame;  // 280896
const int kScreenWidth = 240;
const int kScreenHeight = 160;

// IE / IF bits.
// Bit layout of IE (0x04000200) and IF (0x04000202), per GBATEK:
//   0 V-Blank, 1 H-Blank, 2 V-Counter match, 3-6 Timer 0-3 overflow, 7 Serial,
//   8-11 DMA 0-3 end, 12 Keypad, 13 Game Pak (cartridge removed / external IRQ).
namespace irq {
const u16 VBlank = 1 << 0;
const u16 HBlank = 1 << 1;
const u16 VCount = 1 << 2;
const u16 Timer0 = 1 << 3;  // Timer n = Timer0 << n
const u16 Serial = 1 << 7;
const u16 Dma0 = 1 << 8;  // DMA n = Dma0 << n
const u16 Keypad = 1 << 12;
const u16 GamePak = 1 << 13;
}  // namespace irq

// IE, IF and IME (0x04000200 - 0x04000208).
// Models the three interrupt control registers. Owned by Gba, shared by reference with
// every device that raises interrupts (Ppu, Timers, Keypad, Dma) and with the Cpu/Bios that
// consume them. Single-threaded, like the rest of the emulator; lives as long as the Gba.
class Interrupts : private NonCopyable {
public:
    // Power-on state: nothing enabled, nothing pending, master enable off.
    Interrupts() : ie_(0), if_(0), ime_(false) {}

    // Raises one or more interrupt sources by setting their IF bits (mask uses irq:: bits).
    // The request stays latched until software acknowledges it, even if IE is 0.
    void request(u16 mask) { if_ |= mask; }
    // True when at least one source is both enabled (IE) and requested (IF). This ignores
    // IME on purpose: the HALT state of the CPU is left on IE & IF alone (GBATEK "Halt").
    bool pending() const { return (ie_ & if_ & 0x3FFF) != 0; }
    // True when the CPU should take the IRQ exception now: IME set and something pending.
    // (The CPSR I flag is checked by the Cpu itself.)
    bool shouldInterrupt() const { return ime_ && pending(); }

    // Raw register values, for Bus reads of IE / IF / IME.
    u16 ie() const { return ie_; }
    u16 flags() const { return if_; }
    bool ime() const { return ime_; }
    // IE write: only the 14 defined bits are kept.
    void setIe(u16 v) { ie_ = v & 0x3FFF; }
    void acknowledge(u16 v) { if_ &= static_cast<u16>(~v); }  // write 1 to clear
    // IME write: only bit 0 matters, the caller passes it as a bool.
    void setIme(bool v) { ime_ = v; }

private:
    // IE: enabled sources, bits 0-13. IF: requested sources, bits 0-13 (writing 1 clears).
    // IME: bit 0 of 0x04000208, the master interrupt enable.
    u16 ie_;
    u16 if_;
    bool ime_;
};

}  // namespace gba

#endif
