#include "Gpio.h"

#include <cstring>
#include <ctime>

namespace gba {

namespace {

enum RtcCommand { CmdReset = 0, CmdDateTime = 2, CmdForceIrq = 3, CmdControl = 4, CmdTime = 6 };
const int kCommandBytes[8] = {0, 0, 7, 0, 1, 0, 3, 0};

u8 bcd(int v) { return static_cast<u8>(((v / 10) << 4) | (v % 10)); }

}  // namespace

bool Gpio::cartridgeHasRtc(const char gameCode[4]) {
    static const char* codes[] = {"AXV", "AXP", "BPE", "U3I", "U32", "U33"};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        if (std::strncmp(gameCode, codes[i], 3) == 0) return true;
    }
    return false;
}

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

u16 Gpio::read(u32 offset) const {
    switch (offset) {
        case 0xC4: return pins_ & 0xF;
        case 0xC6: return direction_;
        case 0xC8: return control_;
        default: return 0;
    }
}

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

void Gpio::outputPins(unsigned pins) {
    pins_ = static_cast<u8>((pins_ & direction_) | (pins & ~direction_ & 0xF));
}

void Gpio::latchTime() {
    std::time_t now = std::time(0);
    std::tm t;
    localtime_r(&now, &t);
    time_[0] = bcd(t.tm_year % 100);
    time_[1] = bcd(t.tm_mon + 1);
    time_[2] = bcd(t.tm_mday);
    time_[3] = bcd(t.tm_wday);
    if (rtcControl_ & 0x40) {
        time_[4] = bcd(t.tm_hour);
    } else {
        time_[4] = static_cast<u8>(bcd(t.tm_hour % 12) | (t.tm_hour >= 12 ? 0x80 : 0));
    }
    time_[5] = bcd(t.tm_min);
    time_[6] = bcd(t.tm_sec);
}

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
                    outputPins(5 | (outputBit() << 1));
                    if (++bitsRead_ == 8) {
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
                transferStep_ = pins_ & 1;
                outputPins(1);
            }
            break;
    }
}

}  // namespace gba
