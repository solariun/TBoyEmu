#ifndef TBOYEMU_GBA_BIOS_H
#define TBOYEMU_GBA_BIOS_H

#include <vector>

#include "Bus.h"
#include "Cpu.h"

namespace gba {

// High-level emulation of the GBA BIOS system calls (SWI 0x00 - 0x2A), so
// no copyrighted BIOS image is needed.  Each call is implemented in C++
// with the same inputs/outputs as the original routines.
class Bios : public SwiHandler {
public:
    Bios(Cpu& cpu, Bus& bus, Interrupts& interrupts);

    void swi(u32 number);

private:
    void softReset();
    void registerRamReset(u32 flags);
    void intrWait(bool discardOld, u16 flags);
    void divide(s32 numerator, s32 denominator);
    void arcTan();
    void arcTan2();
    void cpuSet();
    void cpuFastSet();
    void bgAffineSet();
    void objAffineSet();
    void bitUnpack();
    void lz77(bool vram);
    void huffman();
    void runLength(bool vram);
    void diffUnfilter(bool sixteenBit, bool vram);
    void midiKey2Freq();

    void writeOutput(u32 dst, const std::vector<u8>& data, bool vram);

    Cpu& cpu_;
    Bus& bus_;
    Interrupts& interrupts_;
    bool waiting_;  // inside IntrWait / VBlankIntrWait
};

}  // namespace gba

#endif
