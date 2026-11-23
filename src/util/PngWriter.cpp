#include "PngWriter.h"

#include <zlib.h>

#include <fstream>
#include <vector>

namespace util {

namespace {

void putU32(std::vector<unsigned char>& v, unsigned long x) {
    v.push_back(static_cast<unsigned char>(x >> 24));
    v.push_back(static_cast<unsigned char>(x >> 16));
    v.push_back(static_cast<unsigned char>(x >> 8));
    v.push_back(static_cast<unsigned char>(x));
}

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

bool PngWriter::write(const std::string& path, const gb::FrameBuffer& frame) {
    const int w = frame.width();
    const int h = frame.height();
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<size_t>((w * 3 + 1) * h));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        for (int x = 0; x < w; ++x) {
            gb::u16 c = frame.at(x, y);
            int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            raw.push_back(static_cast<unsigned char>((r << 3) | (r >> 2)));
            raw.push_back(static_cast<unsigned char>((g << 3) | (g >> 2)));
            raw.push_back(static_cast<unsigned char>((b << 3) | (b >> 2)));
        }
    }
    uLongf packedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> packed(packedSize);
    if (compress(&packed[0], &packedSize, &raw[0], static_cast<uLong>(raw.size())) != Z_OK) return false;
    packed.resize(packedSize);

    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out) return false;
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.write(reinterpret_cast<const char*>(sig), 8);
    std::vector<unsigned char> ihdr;
    putU32(ihdr, w);
    putU32(ihdr, h);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(2);  // RGB
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    writeChunk(out, "IHDR", ihdr);
    writeChunk(out, "IDAT", packed);
    writeChunk(out, "IEND", std::vector<unsigned char>());
    return static_cast<bool>(out);
}

}  // namespace util
