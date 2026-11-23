#ifndef TBOYEMU_FRONTEND_ROMBROWSER_H
#define TBOYEMU_FRONTEND_ROMBROWSER_H

#include <string>
#include <vector>

#include "../core/Machine.h"
#include "Keyboard.h"
#include "Terminal.h"

namespace term {

// Start screen: every ROM found under a folder (and its subfolders), each
// tagged with the console it is for (read from the ROM header), with
// type-to-filter search over names and system tags.
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
        gb::System system;
        bool known;  // header could be read
    };

    void scan(const std::string& directory, int depth, std::vector<std::string>& files);
    void applyFilter();
    void draw(const std::string& message);
    void drawProgress(size_t done, size_t total);

    Terminal& terminal_;
    Keyboard& keyboard_;
    std::string directory_;
    std::vector<Entry> entries_;
    std::vector<size_t> visible_;  // indexes into entries_
    std::string filter_;
    size_t selected_;  // index into visible_
    size_t scroll_;
    int counts_[4];    // ROMs per gb::System
};

}  // namespace term

#endif
