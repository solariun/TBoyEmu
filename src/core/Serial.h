// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Serial.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Declares Serial, the Game Boy link port (registers SB 0xFF01 and
//  SC 0xFF02). No link cable is emulated: a transfer started with the
//  internal clock finishes after the time 8 bits take at 8192 Hz, shifts in
//  0xFF (what a disconnected port reads) and raises the Serial interrupt.
//  Every byte the game sends is also appended to a text buffer, because
//  Blargg's test ROMs print their results through the link port.
//
//  How it fits in
//  --------------
//  Owned by GameBoy. Bus::tick() calls tick(4) every CPU M-cycle; the Bus
//  routes 0xFF01/0xFF02 to read()/write(). GameBoy::serialOutput() exposes
//  output(), which --headless mode prints. Implementation: Serial.cpp.
//
//  Hardware notes
//  --------------
//  Pan Docs, "Serial Data Transfer (Link Cable)":
//    SB (0xFF01): the byte to send / the byte received.
//    SC (0xFF02): bit 7 transfer start/in progress, bit 1 clock speed (CGB),
//                 bit 0 clock select (1 = internal clock, this Game Boy is master).
//  With the internal clock at 8192 Hz one bit takes 512 dots, so a whole
//  byte takes 8 * 512 = 4096 dots. When it completes, SC bit 7 is cleared
//  and IF bit 3 (Serial) is requested.
//  Transfers using the external clock never complete (no partner).
//
//  Contributing
//  ------------
//  The output buffer is what tells you whether a test ROM passed. Check with:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
//  which should print "Passed all tests".
// ============================================================================
#ifndef TBOYEMU_CORE_SERIAL_H
#define TBOYEMU_CORE_SERIAL_H

#include <string>

#include "InterruptController.h"
#include "Types.h"

namespace gb {

// Link port (SB/SC).  No cable is attached: transfers driven by the
// internal clock complete after the proper delay and shift in 0xFF.
// Outgoing bytes are recorded so test ROMs (which print through the link
// port) can be checked in headless mode.
// Owned by GameBoy; holds a reference to the shared InterruptController.
class Serial : private NonCopyable {
public:
    explicit Serial(InterruptController& ic);

    // Advances a running transfer by the given number of clock cycles
    // (the Bus passes 4 per M-cycle).
    void tick(int cycles);
    // CPU access to SB (0xFF01) or SC (0xFF02); the Bus only passes those two.
    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);

    // Every byte sent with the internal clock, as characters (capped at 64 KiB).
    const std::string& output() const { return output_; }

private:
    InterruptController& ic_;
    // SB: serial data byte.
    u8 sb_;
    // SC: serial control (bit 7 start, bit 0 internal clock).
    u8 sc_;
    int remaining_;  // cycles until the running transfer completes
    // Bytes sent so far (test ROM text output).
    std::string output_;
};

}  // namespace gb

#endif
