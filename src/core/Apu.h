// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Apu.h
//  Layer:  Game Boy core
//
//  What this file does
//  -------------------
//  Declares Apu, a register-only stub of the Game Boy Audio Processing Unit.
//  TBoyEmu produces no sound (it runs in a terminal), but games write and
//  read back the sound registers, and some poll NR52 or depend on the
//  "unused bits read as 1" behavior, so the register file is kept with the
//  correct read masks and the power on/off behavior of NR52.
//
//  How it fits in
//  --------------
//  Owned by GameBoy. The Bus forwards CPU accesses to 0xFF10-0xFF3F here.
//  It is not clocked (no tick()) and raises no interrupts.
//  Implementation: Apu.cpp.
//
//  Hardware notes
//  --------------
//  Register map (Pan Docs, "Audio Registers"):
//    0xFF10-0xFF14 NR10-NR14  channel 1 (pulse with sweep)
//    0xFF16-0xFF19 NR21-NR24  channel 2 (pulse)
//    0xFF1A-0xFF1E NR30-NR34  channel 3 (wave)
//    0xFF20-0xFF23 NR41-NR44  channel 4 (noise)
//    0xFF24-0xFF26 NR50-NR52  master volume, panning, sound on/off
//    0xFF30-0xFF3F            wave pattern RAM (16 bytes, 32 4-bit samples)
//  Turning the APU off (NR52 bit 7 = 0) clears NR10-NR51 and makes them
//  read-only until it is turned on again.
//
//  Contributing
//  ------------
//  A real APU would need a tick() from Bus::tick() and the frame sequencer
//  driven by DIV. Keep read() const and side-effect free. Test with:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
// ============================================================================
#ifndef TBOYEMU_CORE_APU_H
#define TBOYEMU_CORE_APU_H

#include "Types.h"

namespace gb {

// Sound is not produced (terminal build), but games read back and poll the
// sound registers, so they are kept with the correct read masks.
// Owned by GameBoy; single-threaded; no references to other components.
class Apu : private NonCopyable {
public:
    // Constructs the APU in its post-boot state (see reset()).
    Apu();

    // Clears all registers and wave RAM and leaves the APU powered on,
    // as the boot ROM does.
    void reset();
    // CPU read of addr (0xFF10-0xFF3F). Unused bits read as 1; addresses
    // outside the range return 0xFF.
    u8 read(u16 addr) const;
    // CPU write of addr (0xFF10-0xFF3F). Ignored while powered off, except
    // for NR52 and wave RAM.
    void write(u16 addr, u8 value);

private:
    // Raw register values as last written, indexed by addr - 0xFF10.
    u8 regs_[0x30];  // 0xFF10 - 0xFF3F
    // NR52 bit 7: APU master power.
    bool powered_;
};

}  // namespace gb

#endif
