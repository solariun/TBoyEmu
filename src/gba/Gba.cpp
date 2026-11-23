// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Gba.cpp
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Implementation of gba::Gba (see Gba.h): validates the ROM image, builds and wires all
//  components, reads the title from the header, and runs the main emulation loop for one
//  frame.
//
//  How it fits in
//  --------------
//  The frontend calls runFrame() once per displayed frame (EmulatorSession paces it at
//  59.73 Hz; HeadlessRunner runs it as fast as possible). runFrame() drives the Cpu one
//  instruction at a time and lets Timers, Ppu and Dma catch up with the cycles it consumed.
//
//  Hardware notes
//  --------------
//  - The GBA cartridge address space is 32 MiB, so larger images are cut to that size.
//  - When the CPU is halted (HALTCNT / BIOS Halt/IntrWait), only an interrupt can wake it;
//    the next possible interrupt sources are the PPU line events and timer overflows, so
//    the loop jumps directly to the nearest of them instead of stepping cycle by cycle.
//  - DMA timing: H-Blank DMA starts when a visible line enters H-Blank, V-Blank DMA at line
//    160 (GBATEK "GBA DMA Transfers").
//
//  Contributing
//  ------------
//  - Timers are ticked before the Ppu for each step; both receive exactly the cycles the CPU
//    (or the halt skip) consumed, keeping all components in lockstep.
//  - The 4-frame cycle budget is a safety net so the frontend never freezes when a game
//    disables the display timing in a weird way; do not remove it.
//  - Measure speed and correctness with headless runs (README.md "Testing"), e.g.
//      time ./tboyemu --headless --frames 3600 rom.gba
// ============================================================================
#include "Gba.h"

#include <algorithm>
#include <stdexcept>

namespace gba {

namespace {
// Validates the ROM size: it must at least contain the 0xC0-byte cartridge header (the
// constructor reads the title and game code from it) and is truncated to the 32 MiB that
// the cartridge bus can address.
std::vector<u8> padRom(std::vector<u8> rom) {
    if (rom.size() < 0xC0) throw std::runtime_error("GBA ROM image is too small");
    if (rom.size() > 32 * 1024 * 1024) rom.resize(32 * 1024 * 1024);
    return rom;
}
}  // namespace

// Construction follows the member declaration order in Gba.h:
//  rom_ -> backup_ (detected from rom_) -> gpio_ (only for games with an RTC, identified by
//  the game code at header offset 0xAC) -> interrupts_ -> ppu_/timers_/keypad_ (raise
//  interrupts) -> bus_ (maps all of the above) -> cpu_ (executes through bus_) -> bios_
//  (needs cpu_ and bus_), which is then installed as the CPU's SWI handler so BIOS calls are
//  emulated in C++ (no BIOS image needed).
Gba::Gba(std::vector<u8> rom)
    : rom_(padRom(std::move(rom))),
      backup_(Backup::create(rom_)),
      gpio_(Gpio::cartridgeHasRtc(reinterpret_cast<const char*>(&rom_[0xAC])) ? new Gpio() : 0),
      interrupts_(),
      ppu_(interrupts_),
      timers_(interrupts_),
      keypad_(interrupts_),
      bus_(rom_, *backup_, gpio_.get(), interrupts_, ppu_, timers_, keypad_),
      cpu_(bus_, interrupts_),
      bios_(cpu_, bus_, interrupts_) {
    cpu_.setSwiHandler(&bios_);
    // Title: up to 12 ASCII characters at 0xA0-0xAB, NUL-padded; anything non-printable is
    // replaced by '?' so it is safe to print in the terminal.
    for (size_t i = 0xA0; i < 0xAC; ++i) {
        char c = static_cast<char>(rom_[i]);
        if (c == 0) break;
        title_.push_back((c >= 32 && c < 127) ? c : '?');
    }
}

// Main loop of one frame. Each iteration:
//  1. executes one CPU instruction (Cpu::step() returns the cycles it took, 0 if halted);
//  2. while halted, advances time to the next PPU event or timer overflow instead;
//  3. feeds those cycles to the Timers and the Ppu;
//  4. starts H-Blank / V-Blank DMA on the corresponding PPU events;
//  5. returns as soon as the PPU reports a finished frame (start of V-Blank).
void Gba::runFrame() {
    // Guard against a frame never completing (e.g. a crashed game).
    // (budget = 4 frames' worth of cycles; if exhausted the frame simply ends early)
    for (long budget = kCyclesPerFrame * 4L; budget > 0;) {
        int cycles = cpu_.step();
        if (cycles == 0) {
            // Halted: jump straight to the next event that can wake it.
            cycles = std::max(1, std::min(ppu_.cyclesUntilEvent(), timers_.cyclesUntilOverflow()));
        }
        budget -= cycles;
        timers_.tick(cycles);
        u8 events = ppu_.tick(cycles);
        if (events & Ppu::EventHBlank) bus_.dma().onHBlank();
        if (events & Ppu::EventVBlank) bus_.dma().onVBlank();
        if (events & Ppu::EventFrame) return;
    }
}

}  // namespace gba
