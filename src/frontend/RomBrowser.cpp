#include "RomBrowser.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>

#include "../MachineFactory.h"
#include "../util/RomLoader.h"

namespace term {

namespace {

// Colors of the system tags (xterm-256 palette).
const char* tagStyle(gb::System system) {
    switch (system) {
        case gb::System::GameBoy: return "\x1b[38;5;16;48;5;107m";       // olive green
        case gb::System::GameBoyDual: return "\x1b[38;5;16;48;5;73m";    // teal
        case gb::System::GameBoyColor: return "\x1b[38;5;231;48;5;97m";  // purple
        default: return "\x1b[38;5;231;48;5;61m";                        // indigo (GBA)
    }
}

std::string padRight(const std::string& s, size_t width) {
    return s.size() >= width ? s.substr(0, width) : s + std::string(width - s.size(), ' ');
}

}  // namespace

RomBrowser::RomBrowser(Terminal& terminal, Keyboard& keyboard, const std::string& directory)
    : terminal_(terminal), keyboard_(keyboard), directory_(directory), selected_(0), scroll_(0) {
    for (int i = 0; i < 4; ++i) counts_[i] = 0;
    std::vector<std::string> files;
    scan(directory, 0, files);

    // Read every ROM header to find out which console it is for.
    for (size_t i = 0; i < files.size(); ++i) {
        if (i % 16 == 0) drawProgress(i, files.size());
        Entry e;
        e.path = files[i];
        std::string file = files[i].substr(files[i].find_last_of('/') + 1);
        e.name = file.substr(0, file.find_last_of('.'));
        try {
            e.system = emu::probeSystem(e.path);
            e.known = true;
            ++counts_[static_cast<int>(e.system)];
        } catch (const std::exception&) {
            e.system = gb::System::GameBoy;
            e.known = false;
        }
        entries_.push_back(e);
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return util::toLower(a.name) < util::toLower(b.name);
    });
    applyFilter();
}

void RomBrowser::scan(const std::string& directory, int depth, std::vector<std::string>& files) {
    if (depth > 4) return;
    DIR* dir = opendir(directory.c_str());
    if (!dir) return;
    std::vector<std::string> subdirs;
    while (dirent* ent = readdir(dir)) {
        std::string name = ent->d_name;
        if (name.empty() || name[0] == '.') continue;
        std::string path = directory + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            subdirs.push_back(path);
        } else if (util::RomLoader::isRomFile(name)) {
            files.push_back(path);
        }
    }
    closedir(dir);
    for (size_t i = 0; i < subdirs.size(); ++i) scan(subdirs[i], depth + 1, files);
}

void RomBrowser::drawProgress(size_t done, size_t total) {
    std::string out = "\x1b[0m\x1b[2J\x1b[1;1H\x1b[1;38;5;231;48;5;25m TBoyEmu \x1b[0m  Reading ROM headers in " +
                      directory_ + "/ ... " + std::to_string(done) + "/" + std::to_string(total);
    terminal_.write(out);
}

void RomBrowser::applyFilter() {
    visible_.clear();
    std::string needle = util::toLower(filter_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        std::string tag = util::toLower(gb::systemTag(e.system));
        bool match = needle.empty() || util::toLower(e.name).find(needle) != std::string::npos ||
                     (e.known && tag.find(needle) == 0);
        if (match) visible_.push_back(i);
    }
    if (selected_ >= visible_.size()) selected_ = visible_.empty() ? 0 : visible_.size() - 1;
}

void RomBrowser::draw(const std::string& message) {
    Size s = terminal_.size();
    int listRows = std::max(1, s.rows - 5);
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + static_cast<size_t>(listRows)) scroll_ = selected_ - static_cast<size_t>(listRows) + 1;
    size_t width = static_cast<size_t>(std::max(10, s.cols - 13));

    std::string out = "\x1b[?2026h\x1b[0m\x1b[2J\x1b[1;1H";
    out += "\x1b[1;38;5;231;48;5;25m TBoyEmu \x1b[0m  " + std::to_string(visible_.size()) + "/" +
           std::to_string(entries_.size()) + " games:  ";
    for (int i = 0; i < 4; ++i) {
        gb::System sys = static_cast<gb::System>(i);
        out += std::string(tagStyle(sys)) + " " + gb::systemTag(sys) + " \x1b[0m " + std::to_string(counts_[i]) + "  ";
    }
    out += "\x1b[2;1H\x1b[38;5;250m Search: \x1b[0m" + filter_ + "\x1b[38;5;244m_\x1b[0m";

    for (int r = 0; r < listRows; ++r) {
        size_t i = scroll_ + static_cast<size_t>(r);
        if (i >= visible_.size()) break;
        const Entry& e = entries_[visible_[i]];
        out += "\x1b[" + std::to_string(r + 4) + ";1H";
        out += (i == selected_) ? "\x1b[1;38;5;220m > \x1b[0m" : "   ";
        if (e.known) {
            out += std::string(tagStyle(e.system)) + " " + padRight(gb::systemTag(e.system), 6) + " \x1b[0m ";
        } else {
            out += "\x1b[38;5;231;48;5;124m  ???   \x1b[0m ";
        }
        std::string name = padRight(e.name, width);
        if (i == selected_) {
            out += "\x1b[1;38;5;16;48;5;220m" + name + "\x1b[0m";
        } else {
            out += name;
        }
    }
    if (visible_.empty()) out += "\x1b[4;1H   (no matching ROMs)";

    out += "\x1b[" + std::to_string(s.rows) + ";1H\x1b[38;5;244m";
    if (!message.empty()) {
        out += "\x1b[38;5;203m" + message.substr(0, static_cast<size_t>(s.cols));
    } else {
        out += " Up/Down/PgUp/PgDn: move   Enter: play   type: search (name or gb/gbc/gba)   Esc: quit";
    }
    out += "\x1b[0m\x1b[?2026l";
    terminal_.write(out);
}

bool RomBrowser::choose(std::string& path, std::string& message) {
    std::vector<KeyEvent> events;
    bool dirty = true;
    for (;;) {
        if (Terminal::terminateRequested()) return false;
        if (Terminal::consumeResize()) dirty = true;
        if (dirty) {
            draw(message);
            dirty = false;
        }
        events.clear();
        keyboard_.poll(events, 200);
        for (size_t n = 0; n < events.size(); ++n) {
            const KeyEvent& e = events[n];
            if (e.action == KeyAction::Release || e.key == Key::FocusIn || e.key == Key::FocusOut) continue;
            dirty = true;
            message.clear();
            Size s = terminal_.size();
            size_t page = static_cast<size_t>(std::max(1, s.rows - 6));
            switch (e.key) {
                case Key::Up: if (selected_ > 0) --selected_; break;
                case Key::Down: if (selected_ + 1 < visible_.size()) ++selected_; break;
                case Key::PageUp: selected_ = selected_ > page ? selected_ - page : 0; break;
                case Key::PageDown:
                    selected_ = std::min(visible_.empty() ? 0 : visible_.size() - 1, selected_ + page);
                    break;
                case Key::Home: selected_ = 0; break;
                case Key::End: selected_ = visible_.empty() ? 0 : visible_.size() - 1; break;
                case Key::Enter:
                    if (!visible_.empty()) {
                        path = entries_[visible_[selected_]].path;
                        return true;
                    }
                    break;
                case Key::Escape:
                    if (filter_.empty()) return false;
                    filter_.clear();
                    applyFilter();
                    break;
                case Key::CtrlC: return false;
                case Key::Backspace:
                    if (!filter_.empty()) {
                        filter_.erase(filter_.size() - 1);
                        applyFilter();
                    }
                    break;
                case Key::Char:
                    if (e.codepoint < 127 && filter_.size() < 40) {
                        filter_.push_back(static_cast<char>(e.codepoint));
                        selected_ = 0;
                        applyFilter();
                    }
                    break;
                default: break;
            }
        }
    }
}

}  // namespace term
