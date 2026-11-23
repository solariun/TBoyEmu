#ifndef TBOYEMU_FRONTEND_ROMBROWSER_H
#define TBOYEMU_FRONTEND_ROMBROWSER_H

#include <string>
#include <vector>

#include "Keyboard.h"
#include "Terminal.h"

namespace term {

// Full screen list of the ROMs in a directory with type-to-filter search.
class RomBrowser {
public:
    RomBrowser(Terminal& terminal, Keyboard& keyboard, const std::string& directory);

    bool empty() const { return entries_.empty(); }
    // Blocks until the user picks a ROM (true) or quits (false).
    bool choose(std::string& path, std::string& message);

private:
    struct Entry {
        std::string path;
        std::string name;
    };

    void applyFilter();
    void draw(const std::string& message);

    Terminal& terminal_;
    Keyboard& keyboard_;
    std::string directory_;
    std::vector<Entry> entries_;
    std::vector<size_t> visible_;  // indexes into entries_
    std::string filter_;
    size_t selected_;  // index into visible_
    size_t scroll_;
};

}  // namespace term

#endif
