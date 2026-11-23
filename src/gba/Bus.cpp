// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Bus.cpp
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Implements gba::Bus (memory map, wait states, I/O register dispatch) and
//  gba::Dma (the four DMA channels).  See Bus.h for the interface and the
//  memory map table.  Sections in this file:
//    - kBiosCode ..... the few real ARM instructions placed in the BIOS area
//    - Bus ........... construction, wait-state tables, BIOS protection
//    - fetching ...... instruction fetches (with the prefetch approximation)
//    - reads/writes .. per-region data accesses and their quirks
//    - I/O ........... 0x04000000 register dispatch to the other devices
//    - DMA ........... channel start, H-Blank/V-Blank triggers, transfers
//
//  How it fits in
//  --------------
//  The Cpu calls fetch*/read*/write* and then takeCycles().  I/O offsets are
//  forwarded: 0x000-0x05F to Ppu, 0x060-0x0AF kept here (sound, not
//  emulated), 0x0B0-0x0DF to Dma, 0x100-0x10F to Timers, 0x130-0x132 to
//  Keypad, 0x200-0x208 to Interrupts.  Save memory goes to Backup (SRAM,
//  Flash, EEPROM) and the GPIO area of the ROM to Gpio (real time clock).
//
//  Hardware notes
//  --------------
//  - Wait states (GBATEK "GBA System Control", WAITCNT 0x04000204):
//      bits 0-1   SRAM wait:            0..3 = 4, 3, 2, 8 cycles
//      bits 2-3   WS0 first access (N): 4, 3, 2, 8
//      bit  4     WS0 second access (S): 0 = 2, 1 = 1
//      bits 5-6   WS1 first access:     4, 3, 2, 8
//      bit  7     WS1 second access:    0 = 4, 1 = 1
//      bits 8-9   WS2 first access:     4, 3, 2, 8
//      bit  10    WS2 second access:    0 = 8, 1 = 1
//      bits 11-12 PHI terminal output (no effect here)
//      bit  14    game pak prefetch buffer enable
//      bit  15    game pak type (read-only; masked off on write)
//    Total cost of one access = 1 + wait states.
//  - Prefetch: with bit 14 set, the real GBA fetches ROM opcodes in the
//    background while the CPU is busy.  This bus approximates it by charging
//    1 cycle per 16 bits for sequential ROM fetches (README: "prefetch
//    timing is approximated").
//  - Reads beyond the end of the ROM return the low 16 bits of
//    (address / 2), the value left on the cartridge's shared address/data
//    lines (GBATEK "GBA Unpredictable Things").
//  - Byte writes (GBATEK "GBA Memory Map", "Writing 8bit Data to Video
//    Memory"): to palette RAM and background VRAM the byte is written to
//    both halves of the halfword; to sprite VRAM and OAM they are ignored.
//  - Save memory (0x0E/0x0F) has an 8-bit bus: 16/32-bit reads return the
//    byte repeated, 16/32-bit writes store only one byte.
//  - IRQ dispatcher: the real BIOS IRQ handler is copied into the BIOS area
//    so the game's own interrupt handler is called exactly like on hardware
//    (GBATEK "BIOS Interrupt handling"):
//      1. IRQ exception -> PC = 0x18 -> branch to 0x128
//      2. push r0-r3, r12, lr on the IRQ stack
//      3. r0 = 0x04000000; lr = 0x138 (return point)
//      4. ldr pc, [r0, #-4]: reads 0x03FFFFFC, which mirrors to 0x03007FFC
//         in IWRAM, where the game stored the address of its handler
//      5. the handler returns with "BX LR" to 0x138
//      6. pop r0-r3, r12, lr; "SUBS PC, LR, #4" returns from the IRQ and
//         restores CPSR (and the ARM/Thumb state) from SPSR_irq.
//  - BIOS latch values (GBATEK "BIOS Memory"): 0xE129F000 after boot,
//    0xE25EF004 while the IRQ dispatcher runs (fetched at 0x13C),
//    0xE55EC002 after returning from an IRQ (0x144), 0xE3A02004 after a SWI.
//
//  Contributing
//  ------------
//  - Keep the region switch statements in sync between read8/16/32 and
//    write8/16/32; quirks differ per width on purpose (see comments).
//  - If a game hangs waiting for a register, check readIo16(): unknown
//    registers read as 0.
//  - Test: jsmolka "memory", "sram", "flash64", "flash128", "bios" ROMs:
//      make && ./tboyemu --headless --frames 120 --screenshot out.png memory.gba
//    and check the picture says "All tests passed".
// ============================================================================
#include "Bus.h"

#include <cstring>

namespace gba {

namespace {

// High-level-emulated BIOS: only the IRQ dispatcher is real ARM code (at the
// same addresses as in the original BIOS), so games' interrupt handlers run
// exactly as on hardware.  SWI calls are intercepted by the CPU.
// Each entry is {address in the BIOS, 32-bit ARM opcode}; everything else in
// the BIOS area is 0.  The last entry is never executed: it only exists so
// that the BIOS latch (opcode at pc + 8 while executing 0x13C) holds the
// same value as on a real GBA after an interrupt returns.
const u32 kBiosCode[][2] = {
    {0x0018, 0xEA000042},  // b     0x128              (IRQ vector)
    {0x0128, 0xE92D500F},  // stmfd sp!, {r0-r3, r12, lr}
    {0x012C, 0xE3A00301},  // mov   r0, #0x04000000
    {0x0130, 0xE28FE000},  // add   lr, pc, #0
    {0x0134, 0xE510F004},  // ldr   pc, [r0, #-4]       (user handler at 0x03007FFC)
    {0x0138, 0xE8BD500F},  // ldmfd sp!, {r0-r3, r12, lr}
    {0x013C, 0xE25EF004},  // subs  pc, lr, #4
    {0x0144, 0xE55EC002},  // (prefetched after the IRQ return: read-protection value)
};

// Non-sequential wait states selected by a 2-bit WAITCNT field
// (values 0, 1, 2, 3 -> 4, 3, 2, 8 wait states).
const int kWaitN[4] = {4, 3, 2, 8};

// Little-endian loads/stores into byte arrays.
inline u16 load16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline u32 load32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
inline void store16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}
inline void store32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}

// VRAM is 96 KiB but mirrored in 128 KiB blocks: inside each block,
// 0x18000-0x1FFFF mirrors the 32 KiB sprite area at 0x10000-0x17FFF.
inline u32 vramOffset(u32 addr) {
    u32 off = addr & 0x1FFFF;
    return off >= 0x18000 ? off - 0x8000 : off;
}

}  // namespace

// =================================================================== Bus ====

Bus::Bus(const std::vector<u8>& rom, Backup& backup, Gpio* gpio, Interrupts& interrupts, Ppu& ppu, Timers& timers,
         Keypad& keypad)
    : rom_(rom),
      backup_(backup),
      gpio_(gpio),
      interrupts_(interrupts),
      ppu_(ppu),
      timers_(timers),
      keypad_(keypad),
      dma_(*this, interrupts),
      bios_(0x4000, 0),
      ewram_(0x40000, 0),
      iwram_(0x8000, 0) {
    // Build the tiny BIOS image (IRQ dispatcher only).
    for (size_t i = 0; i < sizeof(kBiosCode) / sizeof(kBiosCode[0]); ++i) {
        store32(&bios_[kBiosCode[i][0]], kBiosCode[i][1]);
    }
    romMask_ = 1;
    while (romMask_ < rom_.size()) romMask_ <<= 1;
    romMask_ -= 1;
    // Cartridges bigger than 16 MiB use the whole 0x0D region for ROM, so the
    // EEPROM is only visible at 0x0DFFFF00-0x0DFFFFFF (GBATEK "GBA Cart
    // Backup EEPROM").
    eepromLarge_ = rom_.size() > 16 * 1024 * 1024;
    reset();
}

void Bus::reset() {
    std::fill(ewram_.begin(), ewram_.end(), 0);
    std::fill(iwram_.begin(), iwram_.end(), 0);
    std::memset(soundRegs_, 0, sizeof(soundRegs_));
    waitcnt_ = 0;
    rcnt_ = 0;
    siocnt_ = 0;
    // POSTFLG = 1: the BIOS sets it after the first boot.
    postflg_ = 1;
    cycles_ = 0;
    pc_ = 0x08000000;
    biosLatch_ = 0xE129F000;  // value seen after the boot sequence
    haltRequested_ = false;
    dma_.reset();
    updateWaitStates();
}

// Fills the per-region cost tables (index = address bits 24-27) from
// WAITCNT.  Defaults: 1 cycle for everything (BIOS, IWRAM, I/O, OAM).
void Bus::updateWaitStates() {
    for (int r = 0; r < 16; ++r) {
        cost16N_[r] = cost16S_[r] = cost32N_[r] = 1;
    }
    // EWRAM: 2 wait states, 16-bit bus.
    // 16-bit access: 1 + 2 = 3; 32-bit access: two 16-bit accesses = 6.
    cost16N_[2] = cost16S_[2] = 3;
    cost32N_[2] = 6;
    // Palette and VRAM: 16-bit bus.
    // No wait states, but a 32-bit access takes two cycles.
    cost32N_[5] = cost32N_[6] = 2;
    // Cartridge ROM wait states 0/1/2 (mirrors at 08, 0A, 0C).
    // n[] = first-access (N) wait states, s[] = second-access (S) wait
    // states for WS0, WS1, WS2 (bit layout in the file header).
    const int n[3] = {kWaitN[(waitcnt_ >> 2) & 3], kWaitN[(waitcnt_ >> 5) & 3], kWaitN[(waitcnt_ >> 8) & 3]};
    const int s[3] = {(waitcnt_ & 0x10) ? 1 : 2, (waitcnt_ & 0x80) ? 1 : 4, (waitcnt_ & 0x400) ? 1 : 8};
    for (int ws = 0; ws < 3; ++ws) {
        // Each wait state area covers two regions (e.g. 0x08 and 0x09).
        for (int k = 0; k < 2; ++k) {
            int r = 8 + ws * 2 + k;
            cost16N_[r] = 1 + n[ws];
            cost16S_[r] = 1 + s[ws];
            // The cartridge bus is 16 bits wide: a 32-bit access is N + S.
            cost32N_[r] = cost16N_[r] + cost16S_[r];
        }
    }
    // SRAM / Flash: 8-bit bus.
    // Same cost whatever the access width (only one byte is transferred).
    int sram = 1 + kWaitN[waitcnt_ & 3];
    cost16N_[14] = cost16S_[14] = cost32N_[14] = sram;
    cost16N_[15] = cost16S_[15] = cost32N_[15] = sram;
    prefetch_ = (waitcnt_ & 0x4000) != 0;
}

// Unmapped reads.  On hardware they return the last prefetched opcode; this
// emulator returns 0, which is enough for the games and tests it targets.
u32 Bus::openBus(u32 /*addr*/) const { return 0; }

// BIOS data read.  pc_ is the instruction currently executing (set by the
// CPU before each step); the BIOS contents are readable only from code
// running inside the BIOS itself.
u32 Bus::readBios32(u32 addr) const {
    if (pc_ < 0x4000) return load32(&bios_[addr & 0x3FFC]);
    return biosLatch_;  // BIOS is read-protected when not executing from it
}

// ----------------------------------------------------------- fetching ----

// Thumb opcode fetch.  ROM, IWRAM and BIOS are handled inline (fast path,
// no protection check: fetching is always allowed).  Other regions use
// read16 and, if sequential, the S cost replaces the N cost it charged.
u16 Bus::fetch16(u32 addr, bool sequential) {
    unsigned r = (addr >> 24) & 0xF;
    if (r >= 8 && r <= 0xD) {
        // Cartridge ROM: sequential fetches cost 1 cycle with the prefetch
        // buffer enabled, otherwise the S cost; a jump costs N.
        cycles_ += sequential ? (prefetch_ ? 1 : cost16S_[r]) : cost16N_[r];
        u32 off = addr & 0x01FFFFFE;
        if (off + 1 < rom_.size()) return load16(&rom_[off]);
        return static_cast<u16>(addr >> 1);
    }
    if (r == 3) {
        cycles_ += 1;
        return load16(&iwram_[addr & 0x7FFE]);
    }
    if (r == 0 && addr < 0x4000) {
        cycles_ += 1;
        return load16(&bios_[addr & 0x3FFE]);
    }
    int before = cycles_;
    u16 v = read16(addr);
    if (sequential) cycles_ = before + cost16S_[r];
    return v;
}

// ARM opcode fetch: same as fetch16, but 32 bits (two halfwords on the
// 16-bit cartridge bus).
u32 Bus::fetch32(u32 addr, bool sequential) {
    unsigned r = (addr >> 24) & 0xF;
    if (r >= 8 && r <= 0xD) {
        cycles_ += sequential ? (prefetch_ ? 2 : 2 * cost16S_[r]) : cost32N_[r];
        u32 off = addr & 0x01FFFFFC;
        if (off + 3 < rom_.size()) return load32(&rom_[off]);
        // Past the end of the ROM: each halfword reads as its address / 2.
        return static_cast<u32>((addr >> 1) & 0xFFFF) | (static_cast<u32>(((addr + 2) >> 1) & 0xFFFF) << 16);
    }
    if (r == 3) {
        cycles_ += 1;
        return load32(&iwram_[addr & 0x7FFC]);
    }
    if (r == 0 && addr < 0x4000) {
        cycles_ += 1;
        return load32(&bios_[addr & 0x3FFC]);
    }
    return read32(addr);
}

// -------------------------------------------------------------- reads ----

// Addresses at or above 0x10000000 are unmapped: they are routed to
// region 1 (also unmapped), which costs 1 cycle and returns openBus().
// The masks (addr & size - 1) implement the mirroring of each region.
u8 Bus::read8(u32 addr) {
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) r = 1;
    cycles_ += cost16N_[r];
    switch (r) {
        // BIOS (protected): pick the addressed byte of the word.
        case 0x0: return addr < 0x4000 ? static_cast<u8>(readBios32(addr) >> ((addr & 3) * 8)) : 0;
        case 0x2: return ewram_[addr & 0x3FFFF];
        case 0x3: return iwram_[addr & 0x7FFF];
        case 0x4: {
            // I/O: read the containing halfword, pick the byte.  The I/O area
            // is not mirrored; above 0x400 reads 0.
            u32 off = addr & 0x00FFFFFF;
            if (off >= 0x400) return 0;
            return static_cast<u8>(readIo16(off & ~1u) >> ((off & 1) * 8));
        }
        case 0x5: return ppu_.palette()[addr & 0x3FF];
        case 0x6: return ppu_.vram()[vramOffset(addr)];
        case 0x7: return ppu_.oam()[addr & 0x3FF];
        case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
            // EEPROM save chips sit in the 0x0D region and answer one bit
            // per access (serial protocol handled by Backup).
            if (r == 0xD && backup_.type() == Backup::Type::Eeprom && (!eepromLarge_ || addr >= 0x0DFFFF00)) {
                return static_cast<u8>(backup_.readSerial());
            }
            // The three wait-state areas all map the same 32 MiB ROM.
            u32 off = addr & 0x01FFFFFF;
            if (gpioAt(off) && gpio_->readable()) return static_cast<u8>(gpio_->read(off & ~1u) >> ((off & 1) * 8));
            if (off < rom_.size()) return rom_[off];
            return static_cast<u8>((addr >> 1) >> ((addr & 1) * 8));
        }
        case 0xE: case 0xF: return backup_.read8(addr);
        default: return static_cast<u8>(openBus(addr));
    }
}

// 16-bit read; the address is forced to a halfword boundary (the CPU
// rotates the result for misaligned LDRH).
u16 Bus::read16(u32 addr) {
    addr &= ~1u;
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) r = 1;
    cycles_ += cost16N_[r];
    switch (r) {
        case 0x0: return addr < 0x4000 ? static_cast<u16>(readBios32(addr) >> ((addr & 2) * 8)) : 0;
        case 0x2: return load16(&ewram_[addr & 0x3FFFE]);
        case 0x3: return load16(&iwram_[addr & 0x7FFE]);
        case 0x4: {
            u32 off = addr & 0x00FFFFFF;
            return off < 0x400 ? readIo16(off) : 0;
        }
        case 0x5: return load16(&ppu_.palette()[addr & 0x3FE]);
        case 0x6: return load16(&ppu_.vram()[vramOffset(addr)]);
        case 0x7: return load16(&ppu_.oam()[addr & 0x3FE]);
        case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
            if (r == 0xD && backup_.type() == Backup::Type::Eeprom && (!eepromLarge_ || addr >= 0x0DFFFF00)) {
                return backup_.readSerial();
            }
            u32 off = addr & 0x01FFFFFE;
            if (gpioAt(off) && gpio_->readable()) return gpio_->read(off);
            if (off + 1 < rom_.size()) return load16(&rom_[off]);
            return static_cast<u16>(addr >> 1);
        }
        // 8-bit save bus: the byte appears in both halves.
        case 0xE: case 0xF: return static_cast<u16>(backup_.read8(addr) * 0x0101u);
        default: return static_cast<u16>(openBus(addr));
    }
}

// 32-bit read; the address is forced to a word boundary.  Regions with a
// 32-bit (or fast) path are handled directly; I/O and unmapped regions are
// split into two 16-bit reads (which also charge their cost).
u32 Bus::read32(u32 addr) {
    addr &= ~3u;
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) r = 1;
    switch (r) {
        case 0x0: cycles_ += 1; return addr < 0x4000 ? readBios32(addr) : 0;
        case 0x2: cycles_ += cost32N_[2]; return load32(&ewram_[addr & 0x3FFFC]);
        case 0x3: cycles_ += 1; return load32(&iwram_[addr & 0x7FFC]);
        case 0x5: cycles_ += 2; return load32(&ppu_.palette()[addr & 0x3FC]);
        case 0x6: cycles_ += 2; return load32(&ppu_.vram()[vramOffset(addr)]);
        case 0x7: cycles_ += 1; return load32(&ppu_.oam()[addr & 0x3FC]);
        case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
            // Note: no EEPROM / GPIO check here; those are accessed with
            // 16-bit transfers.
            cycles_ += cost32N_[r];
            u32 off = addr & 0x01FFFFFC;
            if (off + 3 < rom_.size()) return load32(&rom_[off]);
            return static_cast<u32>((addr >> 1) & 0xFFFF) | (static_cast<u32>(((addr + 2) >> 1) & 0xFFFF) << 16);
        }
        // 8-bit save bus: the byte appears in all four lanes.
        case 0xE: case 0xF: cycles_ += cost32N_[r]; return backup_.read8(addr) * 0x01010101u;
        default: {
            u32 lo = read16(addr);
            u32 hi = read16(addr + 2);
            return lo | (hi << 16);
        }
    }
}

// ------------------------------------------------------------- writes ----

// 8-bit write.  Writes to BIOS and ROM are ignored; unmapped addresses too.
void Bus::write8(u32 addr, u8 value) {
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) return;
    cycles_ += cost16N_[r];
    switch (r) {
        case 0x2: ewram_[addr & 0x3FFFF] = value; break;
        case 0x3: iwram_[addr & 0x7FFF] = value; break;
        case 0x4: {
            u32 off = addr & 0x00FFFFFF;
            if (off < 0x400) writeIo8(off, value);
            break;
        }
        case 0x5:  // 8-bit writes to palette fill the whole halfword
            store16(&ppu_.palette()[addr & 0x3FE], static_cast<u16>(value * 0x0101u));
            break;
        case 0x6: {
            // Background VRAM is 64 KiB in tile modes (0-2) and 80 KiB in
            // bitmap modes (3-5); above that is sprite VRAM.
            u32 off = vramOffset(addr);
            u32 bgLimit = ppu_.bitmapMode() ? 0x14000 : 0x10000;
            if (off < bgLimit) store16(&ppu_.vram()[off & ~1u], static_cast<u16>(value * 0x0101u));
            break;  // 8-bit writes to sprite VRAM are ignored
        }
        case 0x7: break;  // 8-bit writes to OAM are ignored
        case 0xE: case 0xF: backup_.write8(addr, value); break;
        default: break;
    }
}

// 16-bit write; the address is forced to a halfword boundary except for
// save memory, where the low address bit selects which byte is stored.
void Bus::write16(u32 addr, u16 value) {
    if ((addr >> 25) == 7 && addr < 0x10000000) {  // 0x0E / 0x0F: 8-bit save memory
        cycles_ += cost16N_[14];
        backup_.write8(addr, static_cast<u8>(value >> ((addr & 1) * 8)));
        return;
    }
    addr &= ~1u;
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) return;
    cycles_ += cost16N_[r];
    switch (r) {
        case 0x2: store16(&ewram_[addr & 0x3FFFE], value); break;
        case 0x3: store16(&iwram_[addr & 0x7FFE], value); break;
        case 0x4: {
            u32 off = addr & 0x00FFFFFF;
            if (off < 0x400) writeIo16(off, value);
            break;
        }
        case 0x5: store16(&ppu_.palette()[addr & 0x3FE], value); break;
        case 0x6: store16(&ppu_.vram()[vramOffset(addr)], value); break;
        case 0x7: store16(&ppu_.oam()[addr & 0x3FE], value); break;
        // ROM is read-only, except the GPIO registers (RTC) at 0x080000C4.
        case 0x8:
            if (gpioAt(addr & 0x01FFFFFE)) gpio_->write(addr & 0x01FFFFFE, value);
            break;
        // EEPROM: one bit per 16-bit write (bit 0), normally sent by DMA3.
        case 0xD:
            if (backup_.type() == Backup::Type::Eeprom && (!eepromLarge_ || addr >= 0x0DFFFF00)) {
                backup_.writeSerial(value);
            }
            break;
        default: break;
    }
}

// 32-bit write; regions with a direct path store the word, everything else
// (I/O, ROM/GPIO/EEPROM) is split into two 16-bit writes, low half first.
void Bus::write32(u32 addr, u32 value) {
    if ((addr >> 25) == 7 && addr < 0x10000000) {  // 0x0E / 0x0F: 8-bit save memory
        cycles_ += cost16N_[14];
        backup_.write8(addr, static_cast<u8>(value >> ((addr & 3) * 8)));
        return;
    }
    addr &= ~3u;
    unsigned r = (addr >> 24) & 0xF;
    if (addr >= 0x10000000) return;
    switch (r) {
        case 0x2: cycles_ += cost32N_[2]; store32(&ewram_[addr & 0x3FFFC], value); break;
        case 0x3: cycles_ += 1; store32(&iwram_[addr & 0x7FFC], value); break;
        case 0x5: cycles_ += 2; store32(&ppu_.palette()[addr & 0x3FC], value); break;
        case 0x6: cycles_ += 2; store32(&ppu_.vram()[vramOffset(addr)], value); break;
        case 0x7: cycles_ += 1; store32(&ppu_.oam()[addr & 0x3FC], value); break;
        default:
            write16(addr, static_cast<u16>(value));
            write16(addr + 2, static_cast<u16>(value >> 16));
            break;
    }
}

// ---------------------------------------------------------------- I/O ----

// Reads the I/O halfword at offset off (from 0x04000000, even, < 0x400).
// (GBATEK "GBA I/O Map".)  Unimplemented registers read as 0.
u16 Bus::readIo16(u32 off) {
    // 0x000-0x05F: LCD registers (DISPCNT, DISPSTAT, VCOUNT, BG*, WIN*, ...).
    if (off < 0x60) return ppu_.readIo(off);
    // 0x060-0x0AF: sound registers.  Values written are read back, except:
    if (off < 0xB0) {
        // SOUNDCNT_X: only the master enable bit (7); channel "on" bits 0-3
        // stay 0 because no channel ever plays.
        if (off == 0x84) return soundRegs_[(0x84 - 0x60) / 2] & 0x0080;  // no channel ever plays
        // SOUNDBIAS: 0x0200 (the BIOS default) until the game writes it.
        if (off == 0x88) return soundRegs_[(0x88 - 0x60) / 2] ? soundRegs_[(0x88 - 0x60) / 2] : 0x0200;
        return soundRegs_[(off - 0x60) / 2];
    }
    // 0x0B0-0x0DF: DMA0-DMA3.
    if (off < 0xE0) return dma_.read(off - 0xB0);
    // 0x100-0x10F: TM0CNT_L/H ... TM3CNT_L/H.
    if (off >= 0x100 && off < 0x110) return timers_.read(off - 0x100);
    switch (off) {
        case 0x128: return siocnt_;              // SIOCNT
        case 0x130: return keypad_.keyInput();   // KEYINPUT (0 = pressed)
        case 0x132: return keypad_.control();    // KEYCNT
        case 0x134: return rcnt_;                // RCNT
        case 0x200: return interrupts_.ie();     // IE
        case 0x202: return interrupts_.flags();  // IF
        case 0x204: return waitcnt_;             // WAITCNT
        case 0x208: return interrupts_.ime() ? 1 : 0;  // IME
        case 0x300: return postflg_;             // POSTFLG (HALTCNT is write-only)
        default: return 0;
    }
}

// Writes the I/O halfword at offset off (even, < 0x400).
void Bus::writeIo16(u32 off, u16 v) {
    if (off < 0x60) {
        ppu_.writeIo(off, v);
        return;
    }
    if (off < 0xB0) {
        // Sound: stored only so that reads return the written values.
        soundRegs_[(off - 0x60) / 2] = v;
        return;
    }
    if (off < 0xE0) {
        dma_.write(off - 0xB0, v);
        return;
    }
    if (off >= 0x100 && off < 0x110) {
        timers_.write(off - 0x100, v);
        return;
    }
    switch (off) {
        case 0x128:
            // No link cable: a started transfer completes at once.
            // SIOCNT bit 7 = start/busy (cleared: done), bit 14 = IRQ enable.
            siocnt_ = static_cast<u16>(v & ~0x80);
            if ((v & 0x80) && (v & 0x4000)) interrupts_.request(irq::Serial);
            break;
        case 0x132: keypad_.setControl(v); break;  // KEYCNT
        case 0x134: rcnt_ = v; break;              // RCNT
        case 0x200: interrupts_.setIe(v); break;   // IE
        // IF: writing 1 to a bit acknowledges (clears) that interrupt.
        case 0x202: interrupts_.acknowledge(v); break;
        case 0x204:
            // WAITCNT: bits 13 and 15 are not writable.
            waitcnt_ = v & 0x5FFF;
            updateWaitStates();
            break;
        case 0x208: interrupts_.setIme(v & 1); break;  // IME: only bit 0
        case 0x300:
            // A 16-bit write covers POSTFLG (low byte) and HALTCNT (high
            // byte), so it also halts the CPU.
            postflg_ = v & 1;
            haltRequested_ = true;  // HALTCNT is the high byte
            break;
        default: break;
    }
}

// 8-bit I/O write.  A few registers need special handling because a plain
// read-modify-write would be wrong: IF (writing back the read value would
// acknowledge every pending interrupt) and POSTFLG/HALTCNT (two different
// registers in one halfword).
void Bus::writeIo8(u32 off, u8 value) {
    if (off == 0x202 || off == 0x203) {
        interrupts_.acknowledge(static_cast<u16>(value << ((off & 1) * 8)));
        return;
    }
    if (off == 0x300) {
        postflg_ = value & 1;
        return;
    }
    if (off == 0x301) {
        haltRequested_ = true;  // HALTCNT (bit 7 = STOP, treated as HALT)
        return;
    }
    // Read-modify-write the surrounding halfword.
    u32 aligned = off & ~1u;
    u16 current = readIo16(aligned);
    u16 merged = (off & 1) ? static_cast<u16>((current & 0x00FF) | (value << 8))
                           : static_cast<u16>((current & 0xFF00) | value);
    writeIo16(aligned, merged);
}

// =================================================================== DMA ====

Dma::Dma(Bus& bus, Interrupts& interrupts) : bus_(bus), interrupts_(interrupts) { reset(); }

void Dma::reset() { std::memset(channels_, 0, sizeof(channels_)); }

// offset / 12 = channel, offset % 12 = register within the channel (see
// the register table in Bus.h).  Only DMAxCNT_H (offset 10) is readable.
u16 Dma::read(u32 offset) const {
    int ch = static_cast<int>(offset / 12);
    if (ch > 3) return 0;
    return (offset % 12) == 10 ? channels_[ch].control : 0;
}

void Dma::write(u32 offset, u16 v) {
    int ch = static_cast<int>(offset / 12);
    if (ch > 3) return;
    Channel& c = channels_[ch];
    switch (offset % 12) {
        case 0: c.sourceReg = (c.sourceReg & 0xFFFF0000u) | v; break;                        // SAD low
        case 2: c.sourceReg = (c.sourceReg & 0x0000FFFFu) | (static_cast<u32>(v) << 16); break;  // SAD high
        case 4: c.destReg = (c.destReg & 0xFFFF0000u) | v; break;                            // DAD low
        case 6: c.destReg = (c.destReg & 0x0000FFFFu) | (static_cast<u32>(v) << 16); break;      // DAD high
        case 8: c.countReg = v; break;                                                       // CNT_L
        case 10: {                                                                           // CNT_H
            // Only a 0 -> 1 transition of the enable bit (re)starts the
            // channel; rewriting control while enabled keeps the latched state.
            bool wasEnabled = (c.control & 0x8000) != 0;
            c.control = v;
            if (!wasEnabled && (v & 0x8000)) start(ch);
            break;
        }
    }
}

// Latches the internal source, destination and count.  Address masks
// (GBATEK "GBA DMA Transfers"): DMA0 can only read internal memory
// (27-bit source), DMA1-3 can read the cartridge (28-bit); only DMA3 can
// write to the cartridge area (28-bit destination, used for EEPROM).
// Count: 14 bits (16 bits for DMA3), 0 means 0x4000 (0x10000 for DMA3).
void Dma::start(int ch) {
    Channel& c = channels_[ch];
    c.source = c.sourceReg & (ch == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu);
    c.dest = c.destReg & (ch == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu);
    u32 count = c.countReg & (ch == 3 ? 0xFFFFu : 0x3FFFu);
    c.count = count ? count : (ch == 3 ? 0x10000u : 0x4000u);
    // A DMA3 to EEPROM tells the Backup how long the serial command is,
    // which reveals whether the chip is 512 B (6-bit addresses) or 8 KiB
    // (14-bit addresses).
    if (ch == 3 && (c.dest >> 24) == 0x0D) bus_.backup().hintDmaLength(static_cast<int>(c.count));
    if (((c.control >> 12) & 3) == 0) transfer(ch);  // immediate
}

// Start timing 2 = H-Blank.  Real hardware does not run H-Blank DMA during
// V-Blank lines; the Ppu takes care of that by reporting EventHBlank for
// visible lines (0-159) only.
void Dma::onHBlank() {
    for (int ch = 0; ch < 4; ++ch) {
        const Channel& c = channels_[ch];
        if ((c.control & 0x8000) && ((c.control >> 12) & 3) == 2) transfer(ch);
    }
}

// Start timing 1 = V-Blank.
void Dma::onVBlank() {
    for (int ch = 0; ch < 4; ++ch) {
        const Channel& c = channels_[ch];
        if ((c.control & 0x8000) && ((c.control >> 12) & 3) == 1) transfer(ch);
    }
}

// Runs a whole transfer of c.count units at once.  Each unit is a normal
// bus read + write, so wait states are charged as for the CPU; 2 extra
// cycles model the DMA start-up overhead.  Lower channel numbers have
// priority on hardware; here channels triggered together run in order 0-3.
void Dma::transfer(int ch) {
    Channel& c = channels_[ch];
    const bool word = (c.control & 0x0400) != 0;
    const int unit = word ? 4 : 2;
    // Address control: 0 increment, 1 decrement, 2 fixed, 3 increment
    // (for the destination: increment + reload on repeat; for the source
    // 3 is prohibited and behaves like increment here).
    const int destCtrl = (c.control >> 5) & 3;
    const int srcCtrl = (c.control >> 7) & 3;
    const int destStep = destCtrl == 1 ? -unit : (destCtrl == 2 ? 0 : unit);
    const int srcStep = srcCtrl == 1 ? -unit : (srcCtrl == 2 ? 0 : unit);

    // Addresses are forced to the unit's alignment.
    u32 src = c.source & (word ? ~3u : ~1u);
    u32 dst = c.dest & (word ? ~3u : ~1u);
    for (u32 n = 0; n < c.count; ++n) {
        if (word) {
            bus_.write32(dst, bus_.read32(src));
        } else {
            bus_.write16(dst, bus_.read16(src));
        }
        src += static_cast<u32>(srcStep);
        dst += static_cast<u32>(destStep);
    }
    bus_.addCycles(2);
    // The internal addresses keep advancing across repeats.
    c.source = src;
    c.dest = dst;

    // IRQ bits DMA0-DMA3 are consecutive in IE/IF (bits 8-11).
    if (c.control & 0x4000) interrupts_.request(static_cast<u16>(irq::Dma0 << ch));

    const int timing = (c.control >> 12) & 3;
    if ((c.control & 0x0200) && timing != 0) {
        // Repeat: reload the count (and the destination for mode 3).
        // The channel stays enabled and runs again at the next trigger.
        u32 count = c.countReg & (ch == 3 ? 0xFFFFu : 0x3FFFu);
        c.count = count ? count : (ch == 3 ? 0x10000u : 0x4000u);
        if (destCtrl == 3) c.dest = c.destReg & (ch == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu);
    } else {
        // One-shot (or immediate): clear the enable bit (bit 15).
        c.control &= 0x7FFF;
    }
}

}  // namespace gba
