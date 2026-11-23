// TBoyEmu - Game Boy Advance core: shared types and constants.
#ifndef TBOYEMU_GBA_COMMON_H
#define TBOYEMU_GBA_COMMON_H

#include <cstdint>

#include "../core/Types.h"

namespace gba {

using gb::u8;
using gb::u16;
using gb::u32;
using gb::u64;
using gb::s8;
typedef std::int16_t s16;
typedef std::int32_t s32;
typedef std::int64_t s64;
using gb::NonCopyable;
using gb::makeUnique;

// 16.78 MHz CPU, 1232 cycles per scanline, 228 scanlines per frame.
const int kCpuClockHz = 16777216;
const int kCyclesPerLine = 1232;
const int kHDrawCycles = 960;
const int kLinesPerFrame = 228;
const int kVisibleLines = 160;
const int kCyclesPerFrame = kCyclesPerLine * kLinesPerFrame;  // 280896
const int kScreenWidth = 240;
const int kScreenHeight = 160;

// IE / IF bits.
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
class Interrupts : private NonCopyable {
public:
    Interrupts() : ie_(0), if_(0), ime_(false) {}

    void request(u16 mask) { if_ |= mask; }
    bool pending() const { return (ie_ & if_ & 0x3FFF) != 0; }
    bool shouldInterrupt() const { return ime_ && pending(); }

    u16 ie() const { return ie_; }
    u16 flags() const { return if_; }
    bool ime() const { return ime_; }
    void setIe(u16 v) { ie_ = v & 0x3FFF; }
    void acknowledge(u16 v) { if_ &= static_cast<u16>(~v); }  // write 1 to clear
    void setIme(bool v) { ime_ = v; }

private:
    u16 ie_;
    u16 if_;
    bool ime_;
};

}  // namespace gba

#endif
