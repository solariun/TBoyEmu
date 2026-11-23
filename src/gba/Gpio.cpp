// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Gpio.cpp
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Implementation of the cartridge GPIO port and the S-3511 real time clock (see Gpio.h):
//   - the list of games that carry the RTC chip;
//   - the three port registers (data / direction / control);
//   - the bit-banged serial protocol: the game toggles SCK/SIO/CS through the data register,
//     this file reassembles bytes, decodes RTC commands and shifts the answer back out on
//     SIO, one bit per SCK pulse.
//
//  How it fits in
//  --------------
//  Bus calls read()/write() for ROM offsets 0xC4/0xC6/0xC8 when Gba created a Gpio. Nothing
//  else talks to this class; it has no timing of its own (the time is read from the host
//  clock when a date/time command starts).
//
//  Hardware notes
//  --------------
//  GBATEK "GBA Cart Real-Time Clock (RTC)". A transfer starts with CS low and SCK high, then
//  CS goes high. Each bit: the GBA drives SCK low (and SIO with the bit when writing), then
//  SCK high, which clocks the bit. The first byte is the command; commands used here:
//   0 reset, 2 date+time (7 bytes), 3 force IRQ, 4 control register (1 byte), 6 time
//   (3 bytes). kCommandBytes holds the data length for each command number.
//  Date/time bytes: year (00-99), month, day, weekday, hour, minute, second, all BCD; in
//  12-hour mode bit 7 of the hour is the PM flag.
//
//  Contributing
//  ------------
//  - The state machine is only advanced by writes to the data register; keep reads free of
//    side effects (read() is const).
//  - Uses POSIX localtime_r (macOS and Linux targets).
//  - Test with a Pokemon Ruby/Sapphire/Emerald ROM (see Gpio.h).
// ============================================================================
#include "Gpio.h"

#include <cstring>
#include <ctime>

namespace gba {

namespace {

// RTC command numbers (bits 4-6 of the command byte) and the number of data bytes that
// follow each one (indexed by command number 0-7; 0 = no data).
enum RtcCommand { CmdReset = 0, CmdDateTime = 2, CmdForceIrq = 3, CmdControl = 4, CmdTime = 6 };
const int kCommandBytes[8] = {0, 0, 7, 0, 1, 0, 3, 0};

// Binary (0-99) to packed BCD, e.g. 42 -> 0x42.
u8 bcd(int v) { return static_cast<u8>(((v / 10) << 4) | (v % 10)); }

}  // namespace

// Game codes (first 3 characters, region letter ignored): AXV = Pokemon Ruby, AXP = Pokemon
// Sapphire, BPE = Pokemon Emerald, U3I = Boktai, U32 = Boktai 2, U33 = Boktai 3 (Shin
// Bokura no Taiyou).
bool Gpio::cartridgeHasRtc(const char gameCode[4]) {
    static const char* codes[] = {"AXV", "AXP", "BPE", "U3I", "U32", "U33"};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        if (std::strncmp(gameCode, codes[i], 3) == 0) return true;
    }
    return false;
}

// Port idle, all pins inputs, reads disabled; RTC in 24-hour mode with no time latched.
Gpio::Gpio()
    : pins_(0),
      direction_(0),
      control_(0),
      transferStep_(0),
      bits_(0),
      bitsRead_(0),
      bytesRemaining_(0),
      commandActive_(false),
      command_(0),
      rtcControl_(0x40) {  // 24-hour mode
    std::memset(time_, 0, sizeof(time_));
}

// Register reads (offsets within the ROM). Only meaningful when readable() is true; the Bus
// checks that.
u16 Gpio::read(u32 offset) const {
    switch (offset) {
        case 0xC4: return pins_ & 0xF;
        case 0xC6: return direction_;
        case 0xC8: return control_;
        default: return 0;
    }
}

// Register writes. For the data register only the bits configured as outputs (direction 1)
// are taken from the written value; input pins keep the level the RTC drives.
void Gpio::write(u32 offset, u16 value) {
    switch (offset) {
        case 0xC4:
            pins_ = static_cast<u8>((pins_ & ~direction_) | (value & direction_ & 0xF));
            updatePins();
            break;
        case 0xC6: direction_ = value & 0xF; break;
        case 0xC8: control_ = value & 1; break;
        default: break;
    }
}

// The RTC side of the pins: it may only change pins the GBA is not driving.
void Gpio::outputPins(unsigned pins) {
    pins_ = static_cast<u8>((pins_ & direction_) | (pins & ~direction_ & 0xF));
}

// Samples the host's local time into time_ (BCD). Called when a date/time or time command
// begins, so all bytes of one read are consistent.
void Gpio::latchTime() {
    std::time_t now = std::time(0);
    std::tm t;
    localtime_r(&now, &t);
    // tm_year counts from 1900; the RTC stores two digits. tm_mon is 0-based, the RTC 1-based.
    time_[0] = bcd(t.tm_year % 100);
    time_[1] = bcd(t.tm_mon + 1);
    time_[2] = bcd(t.tm_mday);
    time_[3] = bcd(t.tm_wday);
    // RTC control bit 6: 24-hour mode. In 12-hour mode bit 7 of the hour byte flags PM.
    if (rtcControl_ & 0x40) {
        time_[4] = bcd(t.tm_hour);
    } else {
        time_[4] = static_cast<u8>(bcd(t.tm_hour % 12) | (t.tm_hour >= 12 ? 0x80 : 0));
    }
    time_[5] = bcd(t.tm_min);
    time_[6] = bcd(t.tm_sec);
}

// Bit to present on SIO for the current read command. bytesRemaining_ counts down from the
// command's length, so 7 - bytesRemaining_ walks time_ from the first byte: date+time reads
// start at the year (index 0), time-only reads (3 bytes) start at the hour (index 4).
unsigned Gpio::outputBit() const {
    if (!commandActive_) return 0;
    u8 byte = 0;
    switch ((command_ >> 4) & 7) {
        case CmdControl: byte = rtcControl_; break;
        case CmdDateTime:
        case CmdTime: byte = time_[7 - bytesRemaining_]; break;
        default: break;
    }
    return (byte >> bitsRead_) & 1;
}

// Called after 8 bits have been clocked in from the GBA (write direction).
// - No command active: the byte is a command. It is valid only if its low nibble is 0110;
//   then its data length is looked up, a reset clears the control register, and the
//   date/time commands latch the host time for the reply.
// - Command active: the byte is data for a write command. Only the control register write is
//   stored; date/time writes are accepted but ignored (the host clock is authoritative).
void Gpio::processByte() {
    --bytesRemaining_;
    if (!commandActive_) {
        if ((bits_ & 0x0F) == 0x06) {  // valid command: magic nibble 0110
            command_ = bits_;
            int cmd = (command_ >> 4) & 7;
            bytesRemaining_ = kCommandBytes[cmd];
            commandActive_ = bytesRemaining_ > 0;
            if (cmd == CmdReset) rtcControl_ = 0;
            if (cmd == CmdDateTime || cmd == CmdTime) latchTime();
        }
    } else if (((command_ >> 4) & 7) == CmdControl) {
        rtcControl_ = bits_;
    }
    bits_ = 0;
    bitsRead_ = 0;
    if (bytesRemaining_ <= 0) {
        commandActive_ = false;
        command_ = 0;
    }
}

// Serial protocol: CS high + SCK edges clock bits in/out, LSB first.
// transferStep_ 0 -> 1: SCK high with CS low (pins & 5 == 1, i.e. bit 0 set, bit 2 clear).
// transferStep_ 1 -> 2: CS rises while SCK stays high (pins & 5 == 5): transfer starts.
// In step 2, every data write is one of:
//  - SCK low: latch SIO into bit bitsRead_ of bits_ (the GBA sets up the bit to send);
//  - SCK high with CS high: the clock pulse. For a write command (command_ bit 7 clear,
//    including while the command byte itself arrives) it counts the bit; for a read command
//    the RTC drives the next answer bit onto SIO and counts it;
//  - CS low: the transfer ends and all command state is dropped.
void Gpio::updatePins() {
    switch (transferStep_) {
        case 0:
            if ((pins_ & 5) == 1) transferStep_ = 1;
            break;
        case 1:
            if ((pins_ & 5) == 5) {
                transferStep_ = 2;
            } else if ((pins_ & 5) != 1) {
                transferStep_ = 0;
            }
            break;
        default:
            if (!(pins_ & 1)) {  // SCK low: sample SIO
                bits_ = static_cast<u8>((bits_ & ~(1 << bitsRead_)) | (((pins_ >> 1) & 1) << bitsRead_));
            } else if (pins_ & 4) {  // SCK high, CS high
                bool reading = (command_ & 0x80) != 0;
                if (!reading) {
                    if (++bitsRead_ == 8) processByte();
                } else {
                    // Drive SIO (bit 1) with the answer bit; SCK and CS stay high (5).
                    outputPins(5 | (outputBit() << 1));
                    if (++bitsRead_ == 8) {
                        // One reply byte sent; finish the command after its last byte.
                        --bytesRemaining_;
                        if (bytesRemaining_ <= 0) {
                            commandActive_ = false;
                            command_ = 0;
                        }
                        bitsRead_ = 0;
                    }
                }
            } else {  // CS dropped: end of transfer
                bitsRead_ = 0;
                bytesRemaining_ = 0;
                commandActive_ = false;
                command_ = 0;
                // If SCK is still high the next transfer can start directly from step 1.
                transferStep_ = pins_ & 1;
                outputPins(1);
            }
            break;
    }
}

}  // namespace gba
