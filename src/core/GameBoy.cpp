#include "GameBoy.h"

namespace gb {

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
    for (int guard = 0; guard < kDotsPerFrame * 2; ++guard) {
        cpu_.step();
        if (bus_.consumeFrameReady()) return;
    }
}

}  // namespace gb
