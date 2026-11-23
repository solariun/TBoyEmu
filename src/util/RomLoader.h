#ifndef TBOYEMU_UTIL_ROMLOADER_H
#define TBOYEMU_UTIL_ROMLOADER_H

#include <string>
#include <vector>

#include "../core/Types.h"

namespace util {

// Loads a ROM image from a plain .gb/.gbc file or from inside a .zip
// archive (stored or deflated entries, via zlib).
class RomLoader {
public:
    // Throws std::runtime_error on failure.
    static std::vector<gb::u8> load(const std::string& path);

    static bool isRomFile(const std::string& path);  // .gb .gbc .sgb .zip
    static std::string savePathFor(const std::string& romPath);

private:
    static std::vector<gb::u8> readFile(const std::string& path);
    static std::vector<gb::u8> extractFromZip(const std::vector<gb::u8>& zip);
};

std::string toLower(std::string s);
bool endsWith(const std::string& s, const std::string& suffix);

}  // namespace util

#endif
