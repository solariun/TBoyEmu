#ifndef TBOYEMU_UTIL_ROMLOADER_H
#define TBOYEMU_UTIL_ROMLOADER_H

#include <cstddef>
#include <string>
#include <vector>

#include "../core/Types.h"

namespace util {

// A ROM image and the name of the file it came from (the entry name when
// it was inside a zip archive).
struct RomImage {
    std::vector<gb::u8> data;
    std::string name;
};

// Loads ROM images from plain .gb/.gbc/.gba files or from inside .zip
// archives (stored or deflated entries, via zlib).  The zip reader seeks
// instead of loading the whole archive, so peeking at a header is cheap.
class RomLoader {
public:
    // Throws std::runtime_error on failure.
    static RomImage load(const std::string& path);
    // Only the first `bytes` bytes (header inspection for the ROM list).
    static RomImage loadHeader(const std::string& path, size_t bytes);

    static bool isRomFile(const std::string& path);  // .gb .gbc .sgb .gba .zip
    static bool isRomName(const std::string& name);  // .gb .gbc .sgb .gba
    static std::string savePathFor(const std::string& romPath);

private:
    static RomImage read(const std::string& path, size_t limit);
};

std::string toLower(std::string s);
bool endsWith(const std::string& s, const std::string& suffix);

}  // namespace util

#endif
