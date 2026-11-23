#include "RomLoader.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace util {

using gb::u8;
using gb::u16;
using gb::u32;

namespace {

u16 le16(const std::vector<u8>& d, size_t at) {
    if (at + 2 > d.size()) throw std::runtime_error("corrupt zip archive");
    return static_cast<u16>(d[at] | (d[at + 1] << 8));
}

u32 le32(const std::vector<u8>& d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("corrupt zip archive");
    return static_cast<u32>(d[at]) | (static_cast<u32>(d[at + 1]) << 8) |
           (static_cast<u32>(d[at + 2]) << 16) | (static_cast<u32>(d[at + 3]) << 24);
}

bool hasRomExtension(const std::string& name) {
    std::string n = toLower(name);
    return endsWith(n, ".gb") || endsWith(n, ".gbc") || endsWith(n, ".sgb");
}

std::vector<u8> inflateRaw(const u8* data, size_t size, size_t expected) {
    std::vector<u8> out(expected);
    z_stream zs;
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;
    zs.next_in = const_cast<Bytef*>(data);
    zs.avail_in = static_cast<uInt>(size);
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) throw std::runtime_error("zlib init failed");
    zs.next_out = out.empty() ? Z_NULL : &out[0];
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) throw std::runtime_error("failed to decompress ROM from zip");
    out.resize(zs.total_out);
    return out;
}

}  // namespace

std::string toLower(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool RomLoader::isRomFile(const std::string& path) {
    return hasRomExtension(path) || endsWith(toLower(path), ".zip");
}

std::string RomLoader::savePathFor(const std::string& romPath) {
    size_t slash = romPath.find_last_of('/');
    size_t dot = romPath.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return romPath.substr(0, dot) + ".sav";
    }
    return romPath + ".sav";
}

std::vector<u8> RomLoader::readFile(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<u8> RomLoader::load(const std::string& path) {
    std::vector<u8> data = readFile(path);
    if (endsWith(toLower(path), ".zip")) return extractFromZip(data);
    return data;
}

std::vector<u8> RomLoader::extractFromZip(const std::vector<u8>& zip) {
    // Locate the End Of Central Directory record (scan backwards past an
    // optional archive comment).
    if (zip.size() < 22) throw std::runtime_error("not a zip archive");
    size_t eocd = std::string::npos;
    size_t lowest = zip.size() > 0xFFFF + 22 ? zip.size() - (0xFFFF + 22) : 0;
    for (size_t i = zip.size() - 22 + 1; i-- > lowest;) {
        if (le32(zip, i) == 0x06054B50) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) throw std::runtime_error("zip directory not found");

    u16 entries = le16(zip, eocd + 10);
    size_t cursor = le32(zip, eocd + 16);
    for (u16 n = 0; n < entries; ++n) {
        if (le32(zip, cursor) != 0x02014B50) throw std::runtime_error("corrupt zip directory");
        u16 method = le16(zip, cursor + 10);
        u32 compressed = le32(zip, cursor + 20);
        u32 uncompressed = le32(zip, cursor + 24);
        u16 nameLen = le16(zip, cursor + 28);
        u16 extraLen = le16(zip, cursor + 30);
        u16 commentLen = le16(zip, cursor + 32);
        u32 localOffset = le32(zip, cursor + 42);
        if (cursor + 46 + nameLen > zip.size()) throw std::runtime_error("corrupt zip directory");
        std::string name(zip.begin() + static_cast<long>(cursor + 46),
                         zip.begin() + static_cast<long>(cursor + 46 + nameLen));
        cursor += 46u + nameLen + extraLen + commentLen;

        if (!hasRomExtension(name)) continue;

        if (le32(zip, localOffset) != 0x04034B50) throw std::runtime_error("corrupt zip entry");
        size_t dataAt = localOffset + 30u + le16(zip, localOffset + 26) + le16(zip, localOffset + 28);
        if (dataAt + compressed > zip.size()) throw std::runtime_error("truncated zip entry");
        if (method == 0) {
            return std::vector<u8>(zip.begin() + static_cast<long>(dataAt),
                                   zip.begin() + static_cast<long>(dataAt + compressed));
        }
        if (method == 8) return inflateRaw(&zip[dataAt], compressed, uncompressed);
        throw std::runtime_error("unsupported zip compression method");
    }
    throw std::runtime_error("no .gb/.gbc file inside the zip archive");
}

}  // namespace util
