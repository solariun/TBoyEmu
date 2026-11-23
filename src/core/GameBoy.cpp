// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/GameBoy.cpp
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Implements GameBoy (see GameBoy.h): constructs and wires the components,
//  applies the post-boot-ROM state in reset(), classifies the cartridge as
//  GB / GB+GBC / GBC, and runs the CPU until a frame is complete.
//
//  How it fits in
//  --------------
//  EmulatorSession calls runFrame() about 59.73 times per second (or as fast
//  as possible in --headless mode). runFrame() steps the Cpu; every CPU
//  memory access ticks the Bus, which advances the PPU; when the PPU reaches
//  V-Blank the Bus raises its frame-ready flag and runFrame() returns.
//
//  Hardware notes
//  --------------
//  Post-boot DIV: the internal 16-bit counter is set to 0xABCC (DMG) or
//  0x267C (CGB), the values this emulator uses for the counter when the boot
//  ROM jumps to 0x0100. IF starts with the V-Blank bit set and IE at 0.
//  CGB flag (header 0x0143): 0xC0 = CGB only, bit 7 set = CGB enhanced,
//  otherwise DMG only (Pan Docs, "The Cartridge Header").
//
//  Contributing
//  ------------
//  Do not reorder the constructor's initializer list against the member
//  declarations in GameBoy.h. Test with:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
// ============================================================================
#include "GameBoy.h"

namespace gb {

// Members are initialized in declaration order; each component receives
// references to the ones it talks to.
GameBoy::GameBoy(std::unique_ptr<Cartridge> cartridge)
    : cartridge_(std::move(cartridge)),
      cgbMode_(cartridge_->header().supportsCgb()),
      interrupts_(),
      timer_(interrupts_),
      joypad_(interrupts_),
      serial_(interrupts_),
      apu_(),
      ppu_(interrupts_, cgbMode_),
      bus_(*cartridge_, ppu_, timer_, joypad_, serial_, apu_, interrupts_, cgbMode_),
      cpu_(bus_, interrupts_) {
    reset();
}

System GameBoy::system() const {
    u8 flag = cartridge_->header().cgbFlag;
    if (flag == 0xC0) return System::GameBoyColor;
    if (flag & 0x80) return System::GameBoyDual;
    return System::GameBoy;
}

// State left by the boot ROM; the boot ROM itself is never executed.
void GameBoy::reset() {
    interrupts_.writeIF(0x01);
    interrupts_.writeIE(0x00);
    timer_.reset(cgbMode_ ? 0x267C : 0xABCC);  // DIV value after the boot ROM
    apu_.reset();
    ppu_.reset();
    bus_.reset();
    cpu_.reset(cgbMode_);
}

void GameBoy::runFrame() {
    // A frame is 70224 dots; the cycle cap guarantees progress even if the
    // game keeps the LCD off for a long time.
    // (Each cpu_.step() takes at least one M-cycle, so the guard of
    // 2 * 70224 steps is always more than one frame of emulated time.)
    for (int guard = 0; guard < kDotsPerFrame * 2; ++guard) {
        cpu_.step();
        if (bus_.consumeFrameReady()) return;
    }
}

}  // namespace gb
