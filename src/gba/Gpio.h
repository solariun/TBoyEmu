// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Gpio.h
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Declares gba::Gpio, the 4-bit general purpose I/O port that some cartridges expose inside
//  the ROM address space, together with the only device emulated on it: the Seiko S-3511
//  real time clock (RTC). The clock reports the host computer's local time, so in-game
//  clocks (berries in Pokemon, sunlight cycles) follow the real date.
//
//  How it fits in
//  --------------
//  - Gba creates a Gpio only when cartridgeHasRtc() recognises the game code in the ROM
//    header (offset 0xAC); otherwise the pointer is null and the addresses read as ROM.
//  - Bus checks the three port addresses on ROM reads/writes: writes always reach write();
//    reads reach read() only when readable() is true (control register bit 0), otherwise
//    the ROM bytes underneath are returned.
//  - Gba::hardware() appends "+RTC" to the save type string when the port exists.
//  Implementation: Gpio.cpp.
//
//  Hardware notes
//  --------------
//  GBATEK "GBA Cart I/O Port (GPIO)" and "GBA Cart Real-Time Clock (RTC)":
//   - 0x080000C4 data (bits 0-3 = pins), 0x080000C6 direction (bit set = pin is an output
//     driven by the GBA), 0x080000C8 control (bit 0: 1 = port readable, 0 = write-only).
//   - RTC pins: bit 0 = SCK (serial clock), bit 1 = SIO (serial data), bit 2 = CS (chip
//     select). Data is shifted LSB first.
//   - Date/time registers are BCD.
//
//  Contributing
//  ------------
//  - Writing the clock (setting the time from a game) is not supported: the host time is
//    always reported.
//  - To check a change, boot Pokemon Ruby/Sapphire/Emerald: these games warn that "the
//    internal battery has run dry" when the clock does not answer correctly, e.g.
//      ./tboyemu --headless --frames 1200 --screenshot out.png pokemon_emerald.gba
// ============================================================================
#ifndef TBOYEMU_GBA_GPIO_H
#define TBOYEMU_GBA_GPIO_H

#include "Common.h"

namespace gba {

// Cartridge GPIO port (ROM addresses 0x080000C4 data, C6 direction, C8
// control) with a Seiko S-3511 real time clock on it, as used by Pokemon
// Ruby / Sapphire / Emerald and Boktai.  The clock reports the host time.
// Pins: 0 = SCK, 1 = SIO, 2 = CS.
// Owned by Gba via unique_ptr (absent for cartridges without RTC); single-threaded.
class Gpio : private NonCopyable {
public:
    // True for the games known to have the RTC chip (by game code).
    // gameCode points at the 4-character game code in the ROM header (offset 0xAC); only the
    // first 3 characters are compared, so every region of a game matches.
    static bool cartridgeHasRtc(const char gameCode[4]);

    Gpio();

    // Control register bit 0: when 0 the port is write-only and reads see the ROM.
    bool readable() const { return (control_ & 1) != 0; }
    u16 read(u32 offset) const;  // offset within the ROM (0xC4, 0xC6, 0xC8)
    // Register write; a data write (0xC4) updates the output pins and runs the RTC protocol.
    void write(u32 offset, u16 value);

private:
    // RTC serial state machine, run after every data write (pin change).
    void updatePins();
    // Handles a complete 8-bit byte received from the GBA (command or parameter).
    void processByte();
    // Next bit (0/1) the RTC shifts out for the active read command.
    unsigned outputBit() const;
    // Sets the levels of the pins the RTC drives (those NOT driven by the GBA).
    void outputPins(unsigned pins);
    // Copies the host local time into time_ (BCD), honoring the 12/24-hour setting.
    void latchTime();

    u8 pins_;       // current pin levels
    u8 direction_;  // 1 = driven by the GBA
    // Control register (0xC8): only bit 0 (read enable) is kept.
    u8 control_;

    // RTC serial state
    // transferStep_: 0 idle, 1 saw SCK high with CS low, 2 CS raised = transfer active.
    // bits_: byte being shifted in; bitsRead_: bit index 0-7 within the current byte.
    // bytesRemaining_: bytes still expected for the active command.
    // commandActive_: a command byte was accepted and its data bytes are in progress.
    // command_: the active command byte as assembled LSB first (bit 7 = read,
    //           bits 4-6 = command number, low nibble must be 0110).
    // rtcControl_: the RTC control register (bit 6 = 24-hour mode).
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
