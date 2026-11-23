#include "Bios.h"

#include <cmath>

namespace gba {

namespace {
const double kPi = 3.14159265358979323846;
}

Bios::Bios(Cpu& cpu, Bus& bus, Interrupts& interrupts)
    : cpu_(cpu), bus_(bus), interrupts_(interrupts), waiting_(false) {}

void Bios::swi(u32 number) {
    switch (number) {
        case 0x00: softReset(); break;
        case 0x01: registerRamReset(cpu_.reg(0)); break;
        case 0x02: cpu_.halt(); break;                                   // Halt
        case 0x03: cpu_.halt(); break;                                   // Stop
        case 0x04: intrWait(cpu_.reg(0) != 0, static_cast<u16>(cpu_.reg(1))); break;
        case 0x05: intrWait(true, irq::VBlank); break;                   // VBlankIntrWait
        case 0x06: divide(static_cast<s32>(cpu_.reg(0)), static_cast<s32>(cpu_.reg(1))); break;
        case 0x07: divide(static_cast<s32>(cpu_.reg(1)), static_cast<s32>(cpu_.reg(0))); break;  // DivArm
        case 0x08: {                                                     // Sqrt
            u32 v = cpu_.reg(0);
            u32 root = static_cast<u32>(std::sqrt(static_cast<double>(v)));
            while (static_cast<u64>(root) * root > v) --root;
            while (static_cast<u64>(root + 1) * (root + 1) <= v) ++root;
            cpu_.setReg(0, root);
            break;
        }
        case 0x09: arcTan(); break;
        case 0x0A: arcTan2(); break;
        case 0x0B: cpuSet(); break;
        case 0x0C: cpuFastSet(); break;
        case 0x0D: cpu_.setReg(0, 0xBAAE187F); break;                    // GetBiosChecksum
        case 0x0E: bgAffineSet(); break;
        case 0x0F: objAffineSet(); break;
        case 0x10: bitUnpack(); break;
        case 0x11: lz77(false); break;
        case 0x12: lz77(true); break;
        case 0x13: huffman(); break;
        case 0x14: runLength(false); break;
        case 0x15: runLength(true); break;
        case 0x16: diffUnfilter(false, false); break;
        case 0x17: diffUnfilter(false, true); break;
        case 0x18: diffUnfilter(true, false); break;
        case 0x1F: midiKey2Freq(); break;
        case 0x25: cpu_.setReg(0, 1); break;                             // MultiBoot: fails
        default: break;  // sound driver calls etc.: no audio, nothing to do
    }
    bus_.setBiosLatch(0xE3A02004);  // value seen after a BIOS call
}

void Bios::softReset() {
    u8 flag = bus_.read8(0x03007FFA);
    for (u32 a = 0x03007E00; a < 0x03008000; a += 4) bus_.write32(a, 0);
    cpu_.reset();
    if (flag) cpu_.branchTo(0x02000000);
}

void Bios::registerRamReset(u32 flags) {
    if (flags & 0x01) for (u32 a = 0x02000000; a < 0x02040000; a += 4) bus_.write32(a, 0);
    if (flags & 0x02) for (u32 a = 0x03000000; a < 0x03007E00; a += 4) bus_.write32(a, 0);
    if (flags & 0x04) for (u32 a = 0x05000000; a < 0x05000400; a += 4) bus_.write32(a, 0);
    if (flags & 0x08) for (u32 a = 0x06000000; a < 0x06018000; a += 4) bus_.write32(a, 0);
    if (flags & 0x10) for (u32 a = 0x07000000; a < 0x07000400; a += 4) bus_.write32(a, 0);
    if (flags & 0x80) {
        bus_.write16(0x04000000, 0x0080);
        for (u32 a = 0x04000008; a < 0x04000060; a += 2) bus_.write16(a, 0);
        bus_.write16(0x04000020, 0x100);
        bus_.write16(0x04000026, 0x100);
        bus_.write16(0x04000030, 0x100);
        bus_.write16(0x04000036, 0x100);
    }
}

// IntrWait: the user IRQ handler ORs the handled interrupts into
// 0x03007FF8.  While none of the wanted ones arrived, the CPU halts and the
// SWI is executed again after the interrupt returns.
void Bios::intrWait(bool discardOld, u16 flags) {
    interrupts_.setIme(true);
    u16 biosFlags = bus_.read16(0x03007FF8);
    if (discardOld && !waiting_) {
        biosFlags = static_cast<u16>(biosFlags & ~flags);
        bus_.write16(0x03007FF8, biosFlags);
    }
    if (biosFlags & flags) {
        bus_.write16(0x03007FF8, static_cast<u16>(biosFlags & ~flags));
        waiting_ = false;
        return;
    }
    waiting_ = true;
    cpu_.setReg(1, flags);
    cpu_.halt();
    cpu_.repeatInstruction();
}

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

void Bios::arcTan2() {
    double x = static_cast<s16>(cpu_.reg(0));
    double y = static_cast<s16>(cpu_.reg(1));
    double angle = std::atan2(y, x);
    if (angle < 0) angle += 2 * kPi;
    cpu_.setReg(0, static_cast<u32>(angle / (2 * kPi) * 65536.0) & 0xFFFF);
}

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
        double a = c * sx, b = -s * sx, cc = s * sy, d = c * sy;
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
        if (data.size() & 1) bus_.write16(dst + static_cast<u32>(data.size() - 1), data.back());
    } else {
        for (size_t i = 0; i < data.size(); ++i) bus_.write8(dst + static_cast<u32>(i), data[i]);
    }
}

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
                    out.push_back(disp <= out.size() ? out[out.size() - disp] : 0);
                }
            } else {
                out.push_back(bus_.read8(src++));
            }
        }
    }
    writeOutput(dst, out, vram);
}

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

void Bios::midiKey2Freq() {
    u32 wave = cpu_.reg(0);
    double key = static_cast<double>(cpu_.reg(1)) + static_cast<double>(cpu_.reg(2)) / 256.0;
    double freq = bus_.read32(wave + 4) / std::pow(2.0, (180.0 - key) / 12.0);
    cpu_.setReg(0, static_cast<u32>(freq));
}

}  // namespace gba
