#include "Gba.h"

#include <algorithm>
#include <stdexcept>

namespace gba {

namespace {
std::vector<u8> padRom(std::vector<u8> rom) {
    if (rom.size() < 0xC0) throw std::runtime_error("GBA ROM image is too small");
    if (rom.size() > 32 * 1024 * 1024) rom.resize(32 * 1024 * 1024);
    return rom;
}
}  // namespace

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
    for (size_t i = 0xA0; i < 0xAC; ++i) {
        char c = static_cast<char>(rom_[i]);
        if (c == 0) break;
        title_.push_back((c >= 32 && c < 127) ? c : '?');
    }
}

void Gba::runFrame() {
    // Guard against a frame never completing (e.g. a crashed game).
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
