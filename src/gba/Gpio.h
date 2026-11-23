#ifndef TBOYEMU_GBA_GPIO_H
#define TBOYEMU_GBA_GPIO_H

#include "Common.h"

namespace gba {

// Cartridge GPIO port (ROM addresses 0x080000C4 data, C6 direction, C8
// control) with a Seiko S-3511 real time clock on it, as used by Pokemon
// Ruby / Sapphire / Emerald and Boktai.  The clock reports the host time.
// Pins: 0 = SCK, 1 = SIO, 2 = CS.
class Gpio : private NonCopyable {
public:
    // True for the games known to have the RTC chip (by game code).
    static bool cartridgeHasRtc(const char gameCode[4]);

    Gpio();

    bool readable() const { return (control_ & 1) != 0; }
    u16 read(u32 offset) const;  // offset within the ROM (0xC4, 0xC6, 0xC8)
    void write(u32 offset, u16 value);

private:
    void updatePins();
    void processByte();
    unsigned outputBit() const;
    void outputPins(unsigned pins);
    void latchTime();

    u8 pins_;       // current pin levels
    u8 direction_;  // 1 = driven by the GBA
    u8 control_;

    // RTC serial state
    int transferStep_;
    u8 bits_;
    int bitsRead_;
    int bytesRemaining_;
    bool commandActive_;
    u8 command_;
    u8 rtcControl_;
    u8 time_[7];  // BCD: year, month, day, weekday, hour, minute, second
};

}  // namespace gba

#endif
