#ifndef TBOYEMU_GBA_BUS_H
#define TBOYEMU_GBA_BUS_H

#include <vector>

#include "Backup.h"
#include "Common.h"
#include "Gpio.h"
#include "Ppu.h"
#include "Timers.h"

namespace gba {

class Bus;

// The four DMA channels (0x040000B0 - 0x040000DF).
class Dma : private NonCopyable {
public:
    Dma(Bus& bus, Interrupts& interrupts);

    void reset();
    u16 read(u32 offset) const;  // offset from 0x040000B0
    void write(u32 offset, u16 value);
    void onHBlank();
    void onVBlank();

private:
    struct Channel {
        u32 sourceReg, destReg;
        u16 countReg, control;
        u32 source, dest, count;  // internal (latched) state
    };
    void start(int ch);
    void transfer(int ch);

    Bus& bus_;
    Interrupts& interrupts_;
    Channel channels_[4];
};

// GBA memory map (BIOS, EWRAM, IWRAM, I/O, palette, VRAM, OAM, cartridge
// ROM and save memory) with the access-time (wait state) model used to
// count CPU cycles.  Owns the DMA controller and the work RAMs.
class Bus : private NonCopyable {
public:
    // gpio may be null (cartridge without a real time clock).
    Bus(const std::vector<u8>& rom, Backup& backup, Gpio* gpio, Interrupts& interrupts, Ppu& ppu, Timers& timers,
        Keypad& keypad);

    void reset();

    // Data accesses (addresses are aligned by the bus; rotation of
    // misaligned loads is the CPU's job).  Each one adds its cycle cost.
    u8 read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);
    void write8(u32 addr, u8 value);
    void write16(u32 addr, u16 value);
    void write32(u32 addr, u32 value);

    // Instruction fetches (sequential fetches from ROM are cheap thanks to
    // the prefetch buffer).
    u16 fetch16(u32 addr, bool sequential);
    u32 fetch32(u32 addr, bool sequential);

    int takeCycles() {
        int c = cycles_;
        cycles_ = 0;
        return c;
    }
    void addCycles(int c) { cycles_ += c; }

    // CPU state the bus needs: BIOS read protection and HALTCNT.
    void setPc(u32 pc) { pc_ = pc; }
    void setBiosLatch(u32 value) { biosLatch_ = value; }
    // Executing from BIOS: protected reads later return the opcode fetched
    // two instructions ahead (pc + 8).
    void latchBiosAt(u32 addr) {
        const u8* p = &bios_[addr & 0x3FFC];
        biosLatch_ = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
                     (static_cast<u32>(p[3]) << 24);
    }
    bool consumeHaltRequest() {
        bool h = haltRequested_;
        haltRequested_ = false;
        return h;
    }

    Dma& dma() { return dma_; }
    Backup& backup() { return backup_; }

private:
    u16 readIo16(u32 offset);
    void writeIo16(u32 offset, u16 value);
    void writeIo8(u32 offset, u8 value);
    u32 readBios32(u32 addr) const;
    void updateWaitStates();
    u32 openBus(u32 addr) const;
    bool gpioAt(u32 romOffset) const { return gpio_ && romOffset >= 0xC4 && romOffset <= 0xC9; }

    const std::vector<u8>& rom_;
    Backup& backup_;
    Gpio* gpio_;
    Interrupts& interrupts_;
    Ppu& ppu_;
    Timers& timers_;
    Keypad& keypad_;
    Dma dma_;

    std::vector<u8> bios_;
    std::vector<u8> ewram_;
    std::vector<u8> iwram_;
    u16 soundRegs_[0x30];  // 0x60-0xBF: stored, no audio produced
    u16 waitcnt_;
    u16 rcnt_, siocnt_;
    u8 postflg_;

    // Cycle cost per region (addr >> 24): 16-bit non-sequential,
    // 16-bit sequential, 32-bit non-sequential.
    int cost16N_[16], cost16S_[16], cost32N_[16];
    bool prefetch_;
    int cycles_;
    u32 pc_;
    u32 biosLatch_;
    bool haltRequested_;
    u32 romMask_;
    bool eepromLarge_;  // >16 MiB ROM: EEPROM only at the top of 0x0D
};

}  // namespace gba

#endif
