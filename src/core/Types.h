// TBoyEmu - Game Boy / Game Boy Color emulator for the terminal.
// Common types shared by every hardware component.
#ifndef TBOYEMU_CORE_TYPES_H
#define TBOYEMU_CORE_TYPES_H

#include <cstdint>
#include <memory>
#include <utility>

namespace gb {

typedef std::uint8_t u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::uint64_t u64;
typedef std::int8_t s8;

// Timing constants (in "dots" = 4.194304 MHz single-speed clock ticks).
const int kCpuClockHz = 4194304;
const int kDotsPerLine = 456;
const int kLinesPerFrame = 154;
const int kDotsPerFrame = kDotsPerLine * kLinesPerFrame;  // 70224
const int kScreenWidth = 160;
const int kScreenHeight = 144;

// The eight physical buttons of the console.
enum class Button : u8 { Right = 0, Left, Up, Down, A, B, Select, Start };
const int kButtonCount = 8;

// Interrupt request bits (IF / IE registers).
namespace irq {
const u8 VBlank = 1 << 0;
const u8 LcdStat = 1 << 1;
const u8 Timer = 1 << 2;
const u8 Serial = 1 << 3;
const u8 Joypad = 1 << 4;
}  // namespace irq

// C++11 has no std::make_unique.
template <typename T, typename... Args>
std::unique_ptr<T> makeUnique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// Base for objects that must not be copied (hardware components hold
// references to each other, so copying them would be a bug).
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
