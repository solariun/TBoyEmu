// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Types.h
//  Layer:  Game Boy core (shared with the Game Boy Advance core and the frontend)
//
//  What this file does
//  -------------------
//  Common types shared by every hardware component: the fixed-width integer
//  aliases (u8, u16, u32, u64, s8), the Game Boy timing and screen constants,
//  the Button enum used by both consoles, the interrupt request bit masks of
//  the Game Boy IF/IE registers, a C++11 replacement for std::make_unique and
//  the NonCopyable base class used by all hardware components.
//
//  How it fits in
//  --------------
//  Included (directly or indirectly) by almost every file in src/core, by the
//  Game Boy Advance core in src/gba (which reuses Button, the integer aliases,
//  makeUnique and NonCopyable) and by the terminal frontend. Everything lives
//  in namespace gb. This header has no dependencies other than the standard
//  library, so it must stay at the bottom of the include graph.
//
//  Hardware notes
//  --------------
//  The Game Boy master clock runs at 4.194304 MHz (Pan Docs, "Timing"). One
//  "dot" is one tick of that clock; one CPU M-cycle is 4 dots in normal speed
//  (2 dots in CGB double speed). A scanline is 456 dots and a frame is 154
//  scanlines (144 visible + 10 V-Blank), i.e. 70224 dots, about 59.73 frames
//  per second (Pan Docs, "Rendering Overview"). The interrupt bits match the
//  IF/IE layout in Pan Docs, "Interrupts".
//
//  Contributing
//  ------------
//  Changing anything here recompiles (and may affect) both cores. The Button
//  values are used as bit positions by the Game Boy Joypad (bits 0-7) and by
//  the GBA keypad, so do not reorder the enum. Test a change with:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
//  and with the other test ROMs listed in README.md (instr_timing, dmg-acid2,
//  cgb-acid2 and the GBA jsmolka gba-tests).
// ============================================================================
#ifndef TBOYEMU_CORE_TYPES_H
#define TBOYEMU_CORE_TYPES_H

#include <cstdint>
#include <memory>
#include <utility>

namespace gb {

// Fixed-width integer aliases used everywhere in the emulator.
// u8/u16 match the Game Boy's 8-bit data bus and 16-bit address bus;
// u32 is the GBA's native word; u64 is used for long time spans (RTC).
// s8 is used for signed 8-bit displacements (JR e, ADD SP,e, signed tile IDs).
typedef std::uint8_t u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::uint64_t u64;
typedef std::int8_t s8;

// Timing constants (in "dots" = 4.194304 MHz single-speed clock ticks).
// kCpuClockHz:     dots per second (the master clock frequency).
// kDotsPerLine:    length of one scanline, including H-Blank (Pan Docs: 456 dots).
// kLinesPerFrame:  144 visible lines + 10 V-Blank lines (LY 0..153).
// kDotsPerFrame:   one full video frame; 4194304 / 70224 = ~59.73 frames/s.
// kScreenWidth / kScreenHeight: visible Game Boy LCD size in pixels.
const int kCpuClockHz = 4194304;
const int kDotsPerLine = 456;
const int kLinesPerFrame = 154;
const int kDotsPerFrame = kDotsPerLine * kLinesPerFrame;  // 70224
const int kScreenWidth = 160;
const int kScreenHeight = 144;

// Game Boy buttons first; L and R exist only on the Game Boy Advance.
// The numeric value of each button is its bit position: the Game Boy Joypad
// stores the pressed buttons as a bit mask where bits 0-3 are the d-pad
// (Right, Left, Up, Down) and bits 4-7 are A, B, Select, Start, which is the
// same order as the low nibble of the P1/JOYP register. Do not reorder.
enum class Button : u8 { Right = 0, Left, Up, Down, A, B, Select, Start, L, R };
// Number of entries in Button (used to size per-button arrays in the frontend).
const int kButtonCount = 10;

// Interrupt request bits (IF / IE registers).
// Bit 0 has the highest priority; each bit also selects the handler address
// 0x40 + 8 * bit (VBlank 0x40, LCD STAT 0x48, Timer 0x50, Serial 0x58,
// Joypad 0x60). See Pan Docs, "Interrupts" and "Interrupt Sources".
namespace irq {
const u8 VBlank = 1 << 0;
const u8 LcdStat = 1 << 1;
const u8 Timer = 1 << 2;
const u8 Serial = 1 << 3;
const u8 Joypad = 1 << 4;
}  // namespace irq

// C++11 has no std::make_unique.
// makeUnique<T>(args...) allocates a T with the given constructor arguments
// and returns it owned by a std::unique_ptr<T> (perfect forwarding).
template <typename T, typename... Args>
std::unique_ptr<T> makeUnique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// Base for objects that must not be copied (hardware components hold
// references to each other, so copying them would be a bug).
// The copy constructor and copy assignment are declared private and never
// defined (the C++03 idiom), so any accidental copy fails to compile or link.
// Inherit privately: "class Foo : private NonCopyable".
class NonCopyable {
protected:
    NonCopyable() {}
    ~NonCopyable() {}

private:
    NonCopyable(const NonCopyable&);
    NonCopyable& operator=(const NonCopyable&);
};

}  // namespace gb

#endif
