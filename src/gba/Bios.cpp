// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/gba/Bios.cpp
//  Layer:  Game Boy Advance core
//
//  What this file does
//  -------------------
//  Implements the high-level-emulated BIOS calls declared in Bios.h: the
//  swi() dispatcher and one method per BIOS function (reset, halt and
//  interrupt wait, math, memory copy, affine setup, decompression).
//
//  How it fits in
//  --------------
//  Cpu::executeArm()/executeThumb() call Bios::swi() when they decode a SWI.
//  Arguments come from Cpu::reg(), memory goes through Bus::read*/write*
//  (so accesses are timed like CPU accesses and reach I/O and VRAM with the
//  usual rules), results are stored with Cpu::setReg().
//
//  Hardware notes
//  --------------
//  - IntrWait / VBlankIntrWait (GBATEK "BIOS Halt Functions") is the subtle
//    one, because a real BIOS loops inside the call while interrupts are
//    serviced.  This implementation cannot block inside C++, so it works in
//    rounds:
//      1. Enable IME.  Read the BIOS interrupt flags at 0x03007FF8 (the game's
//         IRQ handler must OR each interrupt it handles into this halfword).
//      2. If one of the wanted flags is set: clear it and return.  Done.
//      3. Otherwise halt the CPU and rewind its PC to the SWI instruction
//         itself (Cpu::repeatInstruction()).
//      4. Cpu::step() stays halted until (IE & IF) != 0, then takes the IRQ.
//         The IRQ dispatcher at 0x18/0x128 calls the game's handler, which
//         updates 0x03007FF8.  "SUBS PC, LR, #4" returns to the SWI.
//      5. The SWI executes again -> back to step 1.  waiting_ remembers that
//         this is a repeat, so "discard old flags" is applied only on the
//         first round (otherwise the flag just set by the handler would be
//         thrown away).
//  - BIOS read protection: after any SWI the latch is set to 0xE3A02004,
//    the opcode a real BIOS leaves prefetched when returning from a SWI
//    (GBATEK "BIOS Memory").
//  - Decompression formats: GBATEK "BIOS Decompression Functions".  Every
//    compressed block starts with a 32-bit header: bits 4-7 type, bits 8-31
//    decompressed size in bytes.
//
//  Contributing
//  ------------
//  - Never block or loop waiting for hardware here; use the halt +
//    repeatInstruction() pattern of intrWait() instead.
//  - Write to VRAM in 16-bit units (byte writes to VRAM are dropped or
//    doubled by the bus, as on hardware).
//  - Test: ./tboyemu --headless --frames 120 --screenshot out.png bios.gba
//    must show "All tests passed"; also check a few games that decompress
//    graphics at boot.
// ============================================================================
#include "Bios.h"

#include <cmath>

namespace gba {

namespace {
const double kPi = 3.14159265358979323846;
}

Bios::Bios(Cpu& cpu, Bus& bus, Interrupts& interrupts)
    : cpu_(cpu), bus_(bus), interrupts_(interrupts), waiting_(false) {}

// BIOS function dispatcher (numbers from GBATEK "BIOS Function Summary").
// Not implemented (no effect): 0x19 SoundBias, 0x1A-0x1E and 0x20-0x24
// sound driver, 0x26 HardReset, 0x27 CustomHalt, 0x28-0x2A sound driver
// VSync / jump list.
void Bios::swi(u32 number) {
    switch (number) {
        // ---- reset and power ----
        case 0x00: softReset(); break;
        case 0x01: registerRamReset(cpu_.reg(0)); break;
        // Halt: sleep until any enabled interrupt (IE & IF) is pending.
        case 0x02: cpu_.halt(); break;                                   // Halt
        // Stop is a deeper sleep (LCD off) on hardware; treated as Halt.
        case 0x03: cpu_.halt(); break;                                   // Stop
        // IntrWait: r0 = discard old flags (0/1), r1 = wanted IRQ mask.
        case 0x04: intrWait(cpu_.reg(0) != 0, static_cast<u16>(cpu_.reg(1))); break;
        case 0x05: intrWait(true, irq::VBlank); break;                   // VBlankIntrWait
        // ---- arithmetic ----
        // Div: r0 / r1.  DivArm: r1 / r0 (argument order of the ARM C library).
        case 0x06: divide(static_cast<s32>(cpu_.reg(0)), static_cast<s32>(cpu_.reg(1))); break;
        case 0x07: divide(static_cast<s32>(cpu_.reg(1)), static_cast<s32>(cpu_.reg(0))); break;  // DivArm
        case 0x08: {                                                     // Sqrt
            // r0 = floor(sqrt(r0)), r0 unsigned 32-bit.  The double result is
            // corrected by the two loops so it is exact for every input.
            u32 v = cpu_.reg(0);
            u32 root = static_cast<u32>(std::sqrt(static_cast<double>(v)));
            while (static_cast<u64>(root) * root > v) --root;
            while (static_cast<u64>(root + 1) * (root + 1) <= v) ++root;
            cpu_.setReg(0, root);
            break;
        }
        case 0x09: arcTan(); break;
        case 0x0A: arcTan2(); break;
        // ---- memory copy ----
        case 0x0B: cpuSet(); break;
        case 0x0C: cpuFastSet(); break;
        // The checksum of the real GBA BIOS (the DS BIOS differs); some
        // games check it.
        case 0x0D: cpu_.setReg(0, 0xBAAE187F); break;                    // GetBiosChecksum
        // ---- rotation / scaling ----
        case 0x0E: bgAffineSet(); break;
        case 0x0F: objAffineSet(); break;
        // ---- decompression ----
        case 0x10: bitUnpack(); break;
        case 0x11: lz77(false); break;            // LZ77UnCompWram
        case 0x12: lz77(true); break;             // LZ77UnCompVram
        case 0x13: huffman(); break;              // HuffUnComp
        case 0x14: runLength(false); break;       // RLUnCompWram
        case 0x15: runLength(true); break;        // RLUnCompVram
        case 0x16: diffUnfilter(false, false); break;  // Diff8bitUnFilterWram
        case 0x17: diffUnfilter(false, true); break;   // Diff8bitUnFilterVram
        case 0x18: diffUnfilter(true, false); break;   // Diff16bitUnFilter
        // ---- sound ----
        case 0x1F: midiKey2Freq(); break;
        // MultiBoot (link-cable download to other GBAs): r0 = 1 means failure.
        case 0x25: cpu_.setReg(0, 1); break;                             // MultiBoot: fails
        default: break;  // sound driver calls etc.: no audio, nothing to do
    }
    bus_.setBiosLatch(0xE3A02004);  // value seen after a BIOS call
}

// SoftReset (0x00): clears 0x03007E00-0x03007FFF (the BIOS stack and
// variables area), resets the CPU registers and stacks, and restarts at
// 0x08000000 (ROM) or, if the byte at 0x03007FFA was non-zero, at
// 0x02000000 (EWRAM, used by multiboot programs).  The flag is read before
// the area is cleared.
void Bios::softReset() {
    u8 flag = bus_.read8(0x03007FFA);
    for (u32 a = 0x03007E00; a < 0x03008000; a += 4) bus_.write32(a, 0);
    cpu_.reset();
    if (flag) cpu_.branchTo(0x02000000);
}

// RegisterRamReset (0x01), flags in r0:
//   bit 0 EWRAM (256 KiB)        bit 4 OAM
//   bit 1 IWRAM except the last 0x200 bytes (BIOS stack/variables)
//   bit 2 palette RAM            bit 5 serial registers  (not handled)
//   bit 3 VRAM                   bit 6 sound registers   (not handled)
//   bit 7 other I/O registers: here the LCD block only -- DISPCNT = 0x0080
//         (forced blank), 0x04000008-0x0400005F cleared, then the affine
//         matrices of BG2/BG3 set to identity (PA = PD = 0x100 = 1.0).
void Bios::registerRamReset(u32 flags) {
    if (flags & 0x01) for (u32 a = 0x02000000; a < 0x02040000; a += 4) bus_.write32(a, 0);
    if (flags & 0x02) for (u32 a = 0x03000000; a < 0x03007E00; a += 4) bus_.write32(a, 0);
    if (flags & 0x04) for (u32 a = 0x05000000; a < 0x05000400; a += 4) bus_.write32(a, 0);
    if (flags & 0x08) for (u32 a = 0x06000000; a < 0x06018000; a += 4) bus_.write32(a, 0);
    if (flags & 0x10) for (u32 a = 0x07000000; a < 0x07000400; a += 4) bus_.write32(a, 0);
    if (flags & 0x80) {
        bus_.write16(0x04000000, 0x0080);
        for (u32 a = 0x04000008; a < 0x04000060; a += 2) bus_.write16(a, 0);
        bus_.write16(0x04000020, 0x100);  // BG2PA
        bus_.write16(0x04000026, 0x100);  // BG2PD
        bus_.write16(0x04000030, 0x100);  // BG3PA
        bus_.write16(0x04000036, 0x100);  // BG3PD
    }
}

// IntrWait: the user IRQ handler ORs the handled interrupts into
// 0x03007FF8.  While none of the wanted ones arrived, the CPU halts and the
// SWI is executed again after the interrupt returns.
// (Step-by-step description in the file header.)
// discardOld: r0 != 0 -> first clear the wanted flags, so that only an
//             interrupt arriving AFTER the call counts.  VBlankIntrWait
//             always discards.
// flags:      the wanted interrupts, same bit layout as IE/IF.
void Bios::intrWait(bool discardOld, u16 flags) {
    // The BIOS forces IME = 1, otherwise the IRQ that ends the wait could
    // never be serviced.
    interrupts_.setIme(true);
    u16 biosFlags = bus_.read16(0x03007FF8);
    // Discard only on the first round (waiting_ is false); on later rounds
    // the flag may have been set by the handler we are waiting for.
    if (discardOld && !waiting_) {
        biosFlags = static_cast<u16>(biosFlags & ~flags);
        bus_.write16(0x03007FF8, biosFlags);
    }
    if (biosFlags & flags) {
        // Wanted interrupt happened: acknowledge it in the BIOS flags, return.
        bus_.write16(0x03007FF8, static_cast<u16>(biosFlags & ~flags));
        waiting_ = false;
        return;
    }
    // Not yet: sleep, and run this same SWI again after the next IRQ.
    // r1 = flags, as the real routine leaves it (VBlankIntrWait passes 1).
    waiting_ = true;
    cpu_.setReg(1, flags);
    cpu_.halt();
    cpu_.repeatInstruction();
}

// Div: r0 = numerator / denominator (rounded toward zero),
// r1 = numerator % denominator (same sign as the numerator),
// r3 = |quotient|.  The 64-bit math avoids overflow for 0x80000000 / -1.
void Bios::divide(s32 numerator, s32 denominator) {
    if (denominator == 0) {
        // The real BIOS loops forever; return something sane instead.
        cpu_.setReg(0, numerator < 0 ? 0xFFFFFFFFu : 1);
        cpu_.setReg(1, static_cast<u32>(numerator));
        cpu_.setReg(3, 1);
        return;
    }
    s64 q = static_cast<s64>(numerator) / denominator;
    s64 r = static_cast<s64>(numerator) % denominator;
    cpu_.setReg(0, static_cast<u32>(q));
    cpu_.setReg(1, static_cast<u32>(r));
    cpu_.setReg(3, static_cast<u32>(q < 0 ? -q : q));
}

// ArcTan (0x09): r0 = tan(theta) as signed 1.14 fixed point; returns
// theta in r0, -pi/2..pi/2 mapped to 0xC000..0x4000 (GBATEK "BIOS
// Arithmetic Functions").  Uses the BIOS's own polynomial so results are
// bit-exact.
void Bios::arcTan() {
    // Same polynomial as the BIOS (1.14 fixed point).
    s32 i = static_cast<s32>(cpu_.reg(0));
    s32 a = -((i * i) >> 14);
    s32 b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    cpu_.setReg(0, static_cast<u32>((i * b) >> 16));
}

// ArcTan2 (0x0A): r0 = x, r1 = y (signed 16-bit); returns the angle of
// (x, y) in r0, 0..2*pi mapped to 0x0000..0xFFFF.  Computed with
// floating point (may differ from the BIOS in the last bit).
void Bios::arcTan2() {
    double x = static_cast<s16>(cpu_.reg(0));
    double y = static_cast<s16>(cpu_.reg(1));
    double angle = std::atan2(y, x);
    if (angle < 0) angle += 2 * kPi;
    cpu_.setReg(0, static_cast<u32>(angle / (2 * kPi) * 65536.0) & 0xFFFF);
}

// CpuSet (0x0B): r0 = source, r1 = destination, r2 = control:
//   bits 0-20 number of units, bit 24 fill (repeat the first source unit),
//   bit 26 unit size (0 = 16-bit, 1 = 32-bit).
// Addresses are forced to the unit alignment.
void Bios::cpuSet() {
    u32 src = cpu_.reg(0), dst = cpu_.reg(1), ctrl = cpu_.reg(2);
    u32 count = ctrl & 0x1FFFFF;
    bool fill = (ctrl & 0x01000000u) != 0;
    if (ctrl & 0x04000000u) {
        src &= ~3u;
        dst &= ~3u;
        u32 value = bus_.read32(src);
        for (u32 i = 0; i < count; ++i) {
            if (!fill) value = bus_.read32(src + i * 4);
            bus_.write32(dst + i * 4, value);
        }
    } else {
        src &= ~1u;
        dst &= ~1u;
        u16 value = bus_.read16(src);
        for (u32 i = 0; i < count; ++i) {
            if (!fill) value = bus_.read16(src + i * 2);
            bus_.write16(dst + i * 2, value);
        }
    }
}

// CpuFastSet (0x0C): like CpuSet but always 32-bit, and the word count is
// rounded up to a multiple of 8 (the BIOS copies 8 words at a time).
void Bios::cpuFastSet() {
    u32 src = cpu_.reg(0) & ~3u, dst = cpu_.reg(1) & ~3u, ctrl = cpu_.reg(2);
    u32 count = ((ctrl & 0x1FFFFF) + 7) & ~7u;
    bool fill = (ctrl & 0x01000000u) != 0;
    u32 value = bus_.read32(src);
    for (u32 i = 0; i < count; ++i) {
        if (!fill) value = bus_.read32(src + i * 4);
        bus_.write32(dst + i * 4, value);
    }
}

// BgAffineSet (0x0E): r0 = source, r1 = destination, r2 = count.
// Source entry (20 bytes):
//   +0  s32 texture center x (8.8 fixed)   +4  s32 texture center y
//   +8  s16 screen center x (pixels)       +10 s16 screen center y
//   +12 s16 scale x (8.8)                  +14 s16 scale y (8.8)
//   +16 u16 angle (only bits 8-15 used: 0..255 = 0..2*pi)
// Destination entry (16 bytes): PA, PB, PC, PD (s16, 8.8) then the
// reference point X, Y (s32, 8 fractional bits), i.e. the layout of
// BG2PA..BG2Y, chosen so that the screen center shows the texture center.
void Bios::bgAffineSet() {
    u32 src = cpu_.reg(0), dst = cpu_.reg(1);
    u32 count = cpu_.reg(2);
    for (u32 n = 0; n < count; ++n, src += 20, dst += 16) {
        double ox = static_cast<s32>(bus_.read32(src)) / 256.0;
        double oy = static_cast<s32>(bus_.read32(src + 4)) / 256.0;
        double dx = static_cast<s16>(bus_.read16(src + 8));
        double dy = static_cast<s16>(bus_.read16(src + 10));
        double sx = static_cast<s16>(bus_.read16(src + 12)) / 256.0;
        double sy = static_cast<s16>(bus_.read16(src + 14)) / 256.0;
        double theta = (bus_.read16(src + 16) >> 8) / 128.0 * kPi;
        double c = std::cos(theta), s = std::sin(theta);
        // Matrix = rotation * scale: PA = cos*sx, PB = -sin*sx,
        // PC = sin*sy, PD = cos*sy.
        double a = c * sx, b = -s * sx, cc = s * sy, d = c * sy;
        // Start point so that screen (dx, dy) maps to texture (ox, oy).
        double rx = ox - (a * dx + b * dy);
        double ry = oy - (cc * dx + d * dy);
        bus_.write16(dst, static_cast<u16>(static_cast<s16>(a * 256)));
        bus_.write16(dst + 2, static_cast<u16>(static_cast<s16>(b * 256)));
        bus_.write16(dst + 4, static_cast<u16>(static_cast<s16>(cc * 256)));
        bus_.write16(dst + 6, static_cast<u16>(static_cast<s16>(d * 256)));
        bus_.write32(dst + 8, static_cast<u32>(static_cast<s32>(rx * 256)));
        bus_.write32(dst + 12, static_cast<u32>(static_cast<s32>(ry * 256)));
    }
}

// ObjAffineSet (0x0F): r0 = source, r1 = destination, r2 = count,
// r3 = byte offset between PA, PB, PC and PD in the destination (2 for a
// packed array, 8 to write directly into OAM, where the four parameters
// are interleaved with the sprite attributes).
// Source entry (8 bytes): s16 scale x, s16 scale y (8.8), u16 angle
// (bits 8-15 used), 2 bytes padding.
void Bios::objAffineSet() {
    u32 src = cpu_.reg(0), dst = cpu_.reg(1);
    u32 count = cpu_.reg(2), stride = cpu_.reg(3);
    for (u32 n = 0; n < count; ++n, src += 8, dst += stride * 4) {
        double sx = static_cast<s16>(bus_.read16(src)) / 256.0;
        double sy = static_cast<s16>(bus_.read16(src + 2)) / 256.0;
        double theta = (bus_.read16(src + 4) >> 8) / 128.0 * kPi;
        double c = std::cos(theta), s = std::sin(theta);
        bus_.write16(dst, static_cast<u16>(static_cast<s16>(c * sx * 256)));
        bus_.write16(dst + stride, static_cast<u16>(static_cast<s16>(-s * sx * 256)));
        bus_.write16(dst + stride * 2, static_cast<u16>(static_cast<s16>(s * sy * 256)));
        bus_.write16(dst + stride * 3, static_cast<u16>(static_cast<s16>(c * sy * 256)));
    }
}

// BitUnPack (0x10): r0 = source, r1 = destination (written in 32-bit
// words), r2 = pointer to the unpack info:
//   +0 u16 source length in bytes
//   +2 u8  source unit width in bits (1, 2, 4 or 8)
//   +3 u8  destination unit width in bits (1, 2, 4, 8, 16 or 32)
//   +4 u32 bits 0-30: offset added to each unit;
//          bit 31: also add the offset to zero units (otherwise zero stays 0)
// Units are taken from each source byte starting at bit 0.
// Invalid widths make the call do nothing.
void Bios::bitUnpack() {
    u32 src = cpu_.reg(0), dst = cpu_.reg(1), info = cpu_.reg(2);
    u32 length = bus_.read16(info);
    u32 srcWidth = bus_.read8(info + 2);
    u32 dstWidth = bus_.read8(info + 3);
    u32 offset = bus_.read32(info + 4);
    bool zeroToo = (offset & 0x80000000u) != 0;
    offset &= 0x7FFFFFFF;
    if (srcWidth == 0 || dstWidth == 0 || srcWidth > 8 || dstWidth > 32) return;

    u32 out = 0, outBits = 0;
    u32 mask = (1u << srcWidth) - 1;
    for (u32 i = 0; i < length; ++i) {
        u8 byte = bus_.read8(src + i);
        for (u32 bit = 0; bit < 8; bit += srcWidth) {
            u32 v = (byte >> bit) & mask;
            if (v || zeroToo) v += offset;
            out |= (dstWidth == 32 ? v : (v & ((1u << dstWidth) - 1))) << outBits;
            outBits += dstWidth;
            // A full 32-bit word has been assembled: store it.
            if (outBits >= 32) {
                bus_.write32(dst, out);
                dst += 4;
                out = 0;
                outBits = 0;
            }
        }
    }
}

void Bios::writeOutput(u32 dst, const std::vector<u8>& data, bool vram) {
    if (vram) {
        // VRAM can't take byte writes: output goes out in halfwords.
        for (size_t i = 0; i + 1 < data.size(); i += 2) {
            bus_.write16(dst + static_cast<u32>(i), static_cast<u16>(data[i] | (data[i + 1] << 8)));
        }
        // Odd size: the last byte is written with a zero high byte.
        if (data.size() & 1) bus_.write16(dst + static_cast<u32>(data.size() - 1), data.back());
    } else {
        for (size_t i = 0; i < data.size(); ++i) bus_.write8(dst + static_cast<u32>(i), data[i]);
    }
}

// LZ77UnComp (0x11 Wram / 0x12 Vram): r0 = source, r1 = destination.
// After the 32-bit header, the data is a series of blocks: one flag byte,
// then 8 items, flag bits read from bit 7 down to bit 0:
//   bit = 0: one literal byte, copied to the output.
//   bit = 1: a 2-byte back-reference.  First byte: bits 4-7 = length - 3,
//            bits 0-3 = high bits of (distance - 1); second byte = low 8
//            bits of (distance - 1).  Copies `length` bytes starting
//            `distance` bytes back in the output (may overlap).
// The output is built in a buffer first, then written by writeOutput().
void Bios::lz77(bool vram) {
    u32 src = cpu_.reg(0), dst = cpu_.reg(1);
    u32 size = bus_.read32(src) >> 8;
    src += 4;
    std::vector<u8> out;
    out.reserve(size);
    while (out.size() < size) {
        u8 flags = bus_.read8(src++);
        for (int b = 7; b >= 0 && out.size() < size; --b) {
            if (flags & (1 << b)) {
                u8 b0 = bus_.read8(src++);
                u8 b1 = bus_.read8(src++);
                u32 disp = (((b0 & 0xFu) << 8) | b1) + 1;
                u32 len = (b0 >> 4) + 3u;
                for (u32 k = 0; k < len && out.size() < size; ++k) {
                    // A reference before the start of the output reads 0.
                    out.push_back(disp <= out.size() ? out[out.size() - disp] : 0);
                }
            } else {
                out.push_back(bus_.read8(src++));
            }
        }
    }
    writeOutput(dst, out, vram);
}

// HuffUnComp (0x13): r0 = source, r1 = destination (32-bit writes).
// Layout after the header (header bits 0-3 = symbol size, 4 or 8 bits):
//   +4  tree size byte: (tree table size / 2) - 1
//   +5  root node, followed by the rest of the tree table
//   then the bit stream, read as 32-bit words from bit 31 down to bit 0.
// Each internal node byte: bits 0-5 = offset to its children, bit 7 =
// child 0 is a data leaf, bit 6 = child 1 is a data leaf.  The children
// are at ((node address & ~1) + offset * 2 + 2) and the next byte.
// A stream bit 0 takes child 0, bit 1 takes child 1; on reaching a leaf the
// symbol is emitted and decoding restarts at the root.
void Bios::huffman() {
    u32 src = cpu_.reg(0), dst = cpu_.reg(1);
    u32 header = bus_.read32(src);
    u32 dataBits = header & 0xF;
    u32 size = header >> 8;
    if (dataBits != 4 && dataBits != 8) dataBits = 8;
    u32 treeBase = src + 4;
    u32 treeBytes = (bus_.read8(treeBase) + 1u) * 2;
    u32 root = treeBase + 1;
    u32 stream = treeBase + treeBytes;

    u32 node = root;
    u8 nodeValue = bus_.read8(node);
    u32 outWord = 0, outBits = 0, written = 0;
    while (written < size) {
        u32 word = bus_.read32(stream);
        stream += 4;
        for (int bit = 31; bit >= 0 && written < size; --bit) {
            u32 dir = (word >> bit) & 1;
            u32 next = (node & ~1u) + (nodeValue & 0x3Fu) * 2 + 2 + dir;
            bool isData = dir ? (nodeValue & 0x40) != 0 : (nodeValue & 0x80) != 0;
            node = next;
            nodeValue = bus_.read8(node);
            if (isData) {
                // Symbols are packed into a 32-bit word, lowest bits first.
                outWord |= static_cast<u32>(nodeValue & ((1u << dataBits) - 1)) << outBits;
                outBits += dataBits;
                if (outBits == 32) {
                    bus_.write32(dst, outWord);
                    dst += 4;
                    written += 4;
                    outWord = 0;
                    outBits = 0;
                }
                node = root;
                nodeValue = bus_.read8(node);
            }
        }
    }
}

// RLUnComp (0x14 Wram / 0x15 Vram): r0 = source, r1 = destination.
// After the header, a series of runs, each starting with a flag byte:
//   bit 7 = 1: compressed run, one data byte repeated (bits 0-6) + 3 times;
//   bit 7 = 0: (bits 0-6) + 1 literal bytes follow.
void Bios::runLength(bool vram) {
    u32 src = cpu_.reg(0), dst = cpu_.reg(1);
    u32 size = bus_.read32(src) >> 8;
    src += 4;
    std::vector<u8> out;
    out.reserve(size);
    while (out.size() < size) {
        u8 flag = bus_.read8(src++);
        if (flag & 0x80) {
            u32 len = (flag & 0x7Fu) + 3;
            u8 value = bus_.read8(src++);
            for (u32 k = 0; k < len && out.size() < size; ++k) out.push_back(value);
        } else {
            u32 len = (flag & 0x7Fu) + 1;
            for (u32 k = 0; k < len && out.size() < size; ++k) out.push_back(bus_.read8(src++));
        }
    }
    writeOutput(dst, out, vram);
}

// Diff8bitUnFilter (0x16 Wram / 0x17 Vram) and Diff16bitUnFilter (0x18):
// r0 = source, r1 = destination.  Each output unit is the running sum of
// the input units (the data was stored as differences between neighbors).
// The 16-bit version writes halfwords directly (fine for VRAM too).
void Bios::diffUnfilter(bool sixteenBit, bool vram) {
    u32 src = cpu_.reg(0), dst = cpu_.reg(1);
    u32 size = bus_.read32(src) >> 8;
    src += 4;
    if (sixteenBit) {
        u16 acc = 0;
        for (u32 i = 0; i < size; i += 2) {
            acc = static_cast<u16>(acc + bus_.read16(src + i));
            bus_.write16(dst + i, acc);
        }
        return;
    }
    std::vector<u8> out;
    out.reserve(size);
    u8 acc = 0;
    for (u32 i = 0; i < size; ++i) {
        acc = static_cast<u8>(acc + bus_.read8(src + i));
        out.push_back(acc);
    }
    writeOutput(dst, out, vram);
}

// MidiKey2Freq (0x1F): r0 = pointer to a sound driver WaveData structure,
// r1 = MIDI key, r2 = fine adjust (1/256 of a key).  Returns in r0:
//   [WaveData + 4] / 2 ^ ((180 - (key + fine / 256)) / 12)
// (GBATEK "BIOS Sound Functions").
void Bios::midiKey2Freq() {
    u32 wave = cpu_.reg(0);
    double key = static_cast<double>(cpu_.reg(1)) + static_cast<double>(cpu_.reg(2)) / 256.0;
    double freq = bus_.read32(wave + 4) / std::pow(2.0, (180.0 - key) / 12.0);
    cpu_.setReg(0, static_cast<u32>(freq));
}

}  // namespace gba
