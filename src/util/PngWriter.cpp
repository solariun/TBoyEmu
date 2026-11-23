// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/util/PngWriter.cpp
//  Layer:  Utilities
//
//  What this file does
//  -------------------
//  Implementation of PngWriter (see PngWriter.h). A PNG file is the 8-byte signature
//  followed by chunks; this writer emits only the three mandatory ones:
//   - IHDR: width, height, bit depth 8, color type 2 (RGB), compression 0, filter 0,
//     interlace 0;
//   - IDAT: the zlib-compressed image, one scanline after another, each prefixed by a
//     filter-type byte (always 0 = None);
//   - IEND: empty end marker.
//  Each chunk is: 4-byte big-endian length, 4-byte type, data, CRC32 over type + data.
//  Pixels are converted from RGB555 (5 bits per channel, red in bits 0-4) to 8 bits per
//  channel by bit replication ((v << 3) | (v >> 2)), so 31 maps to 255 and 0 to 0.
//  No color correction is applied: this is the raw emulator output.
//
//  How it fits in
//  --------------
//  Called by the headless runner for `--screenshot *.png`. Depends on zlib (compress,
//  compressBound, crc32).
//
//  Contributing
//  ------------
//  - PNG integers are big endian (putU32), unlike most of the emulator's data.
//  - Test by writing a screenshot in headless mode and opening it in an image viewer.
// ============================================================================
#include "PngWriter.h"

#include <zlib.h>

#include <fstream>
#include <vector>

namespace util {

namespace {

// Appends x as a 32-bit big-endian integer.
void putU32(std::vector<unsigned char>& v, unsigned long x) {
    v.push_back(static_cast<unsigned char>(x >> 24));
    v.push_back(static_cast<unsigned char>(x >> 16));
    v.push_back(static_cast<unsigned char>(x >> 8));
    v.push_back(static_cast<unsigned char>(x));
}

// Writes one PNG chunk: length, 4-character type, data, CRC32 of type + data (the CRC
// skips the 4 length bytes, hence &chunk[4]).
void writeChunk(std::ofstream& out, const char* type, const std::vector<unsigned char>& data) {
    std::vector<unsigned char> chunk;
    putU32(chunk, data.size());
    chunk.insert(chunk.end(), type, type + 4);
    chunk.insert(chunk.end(), data.begin(), data.end());
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, &chunk[4], static_cast<uInt>(chunk.size() - 4));
    putU32(chunk, crc);
    out.write(reinterpret_cast<const char*>(&chunk[0]), static_cast<std::streamsize>(chunk.size()));
}

}  // namespace

// Builds the raw image (filter byte + RGB triplets per row), compresses it in one go with
// zlib, then writes signature, IHDR, IDAT and IEND.
bool PngWriter::write(const std::string& path, const gb::FrameBuffer& frame) {
    const int w = frame.width();
    const int h = frame.height();
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<size_t>((w * 3 + 1) * h));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        for (int x = 0; x < w; ++x) {
            // RGB555 -> RGB888 by bit replication.
            gb::u16 c = frame.at(x, y);
            int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            raw.push_back(static_cast<unsigned char>((r << 3) | (r >> 2)));
            raw.push_back(static_cast<unsigned char>((g << 3) | (g >> 2)));
            raw.push_back(static_cast<unsigned char>((b << 3) | (b >> 2)));
        }
    }
    // compress() produces a zlib stream (header + deflate + Adler-32), which is exactly
    // what the IDAT payload must contain.
    uLongf packedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> packed(packedSize);
    if (compress(&packed[0], &packedSize, &raw[0], static_cast<uLong>(raw.size())) != Z_OK) return false;
    packed.resize(packedSize);

    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out) return false;
    // PNG signature: 0x89 "PNG" CR LF 0x1A LF.
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.write(reinterpret_cast<const char*>(sig), 8);
    std::vector<unsigned char> ihdr;
    putU32(ihdr, w);
    putU32(ihdr, h);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(2);  // RGB
    // Compression method 0 (deflate), filter method 0, no interlace.
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    writeChunk(out, "IHDR", ihdr);
    writeChunk(out, "IDAT", packed);
    writeChunk(out, "IEND", std::vector<unsigned char>());
    return static_cast<bool>(out);
}

}  // namespace util
