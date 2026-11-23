// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/util/RomLoader.cpp
//  Layer:  Utilities
//
//  What this file does
//  -------------------
//  Implementation of RomLoader (see RomLoader.h). Plain files are read directly. For .zip
//  archives a minimal random-access reader (ZipReader, private to this file) is used:
//   1. find the End Of Central Directory record (EOCD, signature 0x06054B50) at the end of
//      the file, possibly followed by an archive comment of up to 65535 bytes;
//   2. read the central directory it points to and walk its entries (signature
//      0x02014B50) until one has a ROM file extension;
//   3. read that entry's local file header (signature 0x04034B50) to find where the data
//      starts, then copy it (method 0, "stored") or decompress it with zlib raw inflate
//      (method 8, "deflate").
//  Only the bytes needed are read from disk, so peeking at a ROM header inside a large zip
//  is fast. Zip64 archives, encryption and other compression methods are not supported.
//  Field offsets follow the PKWARE APPNOTE zip specification; all values are little endian.
//
//  How it fits in
//  --------------
//  Called by MachineFactory (load / loadHeader) and the frontend (RomBrowser, EmulatorSession).
//  Depends on zlib (linked by the Makefile).
//
//  Contributing
//  ------------
//  - Every read is bounds-checked (readAt throws on truncation) because ROM folders often
//    contain damaged downloads; keep it that way so a bad file only shows an error message.
//  - Test with a stored and a deflated zip and a plain ROM:
//      make && ./tboyemu --headless --frames 60 --screenshot out.png game.zip
// ============================================================================
#include "RomLoader.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace util {

using gb::u8;
using gb::u16;
using gb::u32;

namespace {

// Little-endian 16/32-bit reads from a byte buffer (zip fields are little endian).
u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

// Minimal random-access zip reader.
// Keeps the archive open for its lifetime (RAII through std::ifstream) and reads slices
// on demand. Used once per load and then destroyed.
class ZipReader {
public:
    // Opens the archive and records its size; throws if it cannot be opened.
    explicit ZipReader(const std::string& path) : in_(path.c_str(), std::ios::binary) {
        if (!in_) throw std::runtime_error("cannot open " + path);
        in_.seekg(0, std::ios::end);
        size_ = static_cast<size_t>(in_.tellg());
    }

    // Finds the first ROM entry and returns its decompressed bytes (at most
    // `limit` of them).
    // Throws std::runtime_error if the archive is not a zip, is damaged, has no ROM entry,
    // or uses an unsupported compression method.
    RomImage readFirstRom(size_t limit) {
        // End Of Central Directory record, possibly followed by a comment.
        // The EOCD is 22 bytes plus a comment of at most 0xFFFF bytes, so it must be within
        // the last 0xFFFF + 22 bytes. Scan backwards for its signature.
        size_t tail = std::min(size_, static_cast<size_t>(0xFFFF + 22));
        std::vector<u8> end = readAt(size_ - tail, tail);
        size_t eocd = std::string::npos;
        for (size_t i = tail >= 22 ? tail - 22 + 1 : 0; i-- > 0;) {
            if (le32(&end[i]) == 0x06054B50) {
                eocd = i;
                break;
            }
        }
        if (eocd == std::string::npos) throw std::runtime_error("not a zip archive");
        // EOCD fields: +10 total number of entries, +12 directory size, +16 directory offset.
        u16 entries = le16(&end[eocd + 10]);
        u32 dirSize = le32(&end[eocd + 12]);
        u32 dirOffset = le32(&end[eocd + 16]);
        std::vector<u8> dir = readAt(dirOffset, dirSize);

        // Walk the central directory. Each entry is a 46-byte fixed header followed by the
        // file name, an extra field and a comment. Fields used: +10 compression method,
        // +20 compressed size, +24 uncompressed size, +28/+30/+32 name/extra/comment
        // lengths, +42 offset of the local file header.
        size_t p = 0;
        for (u16 n = 0; n < entries; ++n) {
            if (p + 46 > dir.size() || le32(&dir[p]) != 0x02014B50) throw std::runtime_error("corrupt zip directory");
            u16 method = le16(&dir[p + 10]);
            u32 compressed = le32(&dir[p + 20]);
            u32 uncompressed = le32(&dir[p + 24]);
            u16 nameLen = le16(&dir[p + 28]);
            u16 extraLen = le16(&dir[p + 30]);
            u16 commentLen = le16(&dir[p + 32]);
            u32 local = le32(&dir[p + 42]);
            if (p + 46 + nameLen > dir.size()) throw std::runtime_error("corrupt zip directory");
            std::string name(reinterpret_cast<const char*>(&dir[p + 46]), nameLen);
            p += 46u + nameLen + extraLen + commentLen;
            // Skip non-ROM entries (readme files, folders, ...).
            if (!RomLoader::isRomName(name)) continue;

            // Local file header: 30 bytes, then name (+26 length) and extra field (+28 length)
            // whose lengths may differ from the central directory copy, so they are re-read.
            std::vector<u8> header = readAt(local, 30);
            if (le32(&header[0]) != 0x04034B50) throw std::runtime_error("corrupt zip entry");
            size_t dataAt = local + 30u + le16(&header[26]) + le16(&header[28]);
            size_t want = std::min(static_cast<size_t>(uncompressed), limit);

            RomImage image;
            image.name = name;
            if (method == 0) {
                // Stored: the data is the file itself.
                image.data = readAt(dataAt, want);
            } else if (method == 8) {
                // Compressed input needed for `want` output bytes is unknown;
                // for a header peek a small slice is plenty.
                // (want + 64 KiB of compressed input is far more than needed to produce a
                // few hundred header bytes; for a full load the whole entry is read.)
                size_t input = want < uncompressed ? std::min<size_t>(compressed, want + 64 * 1024) : compressed;
                std::vector<u8> packed = readAt(dataAt, input);
                image.data = inflate(packed, want);
            } else {
                throw std::runtime_error("unsupported zip compression method");
            }
            return image;
        }
        throw std::runtime_error("no ROM file inside the zip archive");
    }

private:
    // Reads exactly `bytes` bytes at `offset`; throws if that range is outside the file or
    // the read fails.
    std::vector<u8> readAt(size_t offset, size_t bytes) {
        if (offset > size_ || bytes > size_ - offset) throw std::runtime_error("truncated zip archive");
        std::vector<u8> buf(bytes);
        in_.seekg(static_cast<std::streamoff>(offset));
        if (bytes) in_.read(reinterpret_cast<char*>(&buf[0]), static_cast<std::streamsize>(bytes));
        if (!in_) throw std::runtime_error("read error in zip archive");
        return buf;
    }

    // Decompresses raw deflate data (zip entries have no zlib header, hence the negative
    // window bits passed to inflateInit2) into at most `want` bytes. Stops early when the
    // output is full or the input runs out, which is how a header peek works with a
    // truncated input slice. Returns the bytes actually produced.
    static std::vector<u8> inflate(std::vector<u8>& packed, size_t want) {
        std::vector<u8> out(want);
        z_stream zs;
        zs.zalloc = Z_NULL;
        zs.zfree = Z_NULL;
        zs.opaque = Z_NULL;
        zs.next_in = packed.empty() ? Z_NULL : &packed[0];
        zs.avail_in = static_cast<uInt>(packed.size());
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) throw std::runtime_error("zlib init failed");
        zs.next_out = out.empty() ? Z_NULL : &out[0];
        zs.avail_out = static_cast<uInt>(out.size());
        int rc = ::inflate(&zs, Z_SYNC_FLUSH);
        while (rc == Z_OK && zs.avail_out > 0 && zs.avail_in > 0) rc = ::inflate(&zs, Z_SYNC_FLUSH);
        inflateEnd(&zs);
        // Z_BUF_ERROR only means "no progress possible" (output full or input exhausted),
        // which is expected for a partial read; anything else is real corruption.
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            throw std::runtime_error("failed to decompress ROM from zip");
        }
        out.resize(zs.total_out);
        return out;
    }

    // Open archive and its total size in bytes.
    std::ifstream in_;
    size_t size_;
};

}  // namespace

// Lower-cases with std::tolower (ASCII letters in the default "C" locale); the cast to
// unsigned char avoids undefined behavior for bytes >= 0x80.
std::string toLower(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Case-insensitive check of the ROM extensions (".sgb" = Super Game Boy enhanced dumps,
// which are ordinary Game Boy ROMs).
bool RomLoader::isRomName(const std::string& name) {
    std::string n = toLower(name);
    return endsWith(n, ".gb") || endsWith(n, ".gbc") || endsWith(n, ".sgb") || endsWith(n, ".gba");
}

bool RomLoader::isRomFile(const std::string& path) {
    return isRomName(path) || endsWith(toLower(path), ".zip");
}

// Only a dot in the last path component counts as an extension, so "dir.v2/game" becomes
// "dir.v2/game.sav" rather than "dir.sav".
std::string RomLoader::savePathFor(const std::string& romPath) {
    size_t slash = romPath.find_last_of('/');
    size_t dot = romPath.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return romPath.substr(0, dot) + ".sav";
    }
    return romPath + ".sav";
}

// Zip archives go through ZipReader; any other file is read as a raw ROM image (at most
// `limit` bytes). The image name is the file name without its directory.
RomImage RomLoader::read(const std::string& path, size_t limit) {
    if (endsWith(toLower(path), ".zip")) {
        ZipReader zip(path);
        return zip.readFirstRom(limit);
    }
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    in.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(in.tellg());
    in.seekg(0);
    RomImage image;
    image.name = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
    image.data.resize(std::min(size, limit));
    if (!image.data.empty()) in.read(reinterpret_cast<char*>(&image.data[0]), static_cast<std::streamsize>(image.data.size()));
    return image;
}

// Full load: no size limit.
RomImage RomLoader::load(const std::string& path) { return read(path, std::numeric_limits<size_t>::max()); }

RomImage RomLoader::loadHeader(const std::string& path, size_t bytes) { return read(path, bytes); }

}  // namespace util
