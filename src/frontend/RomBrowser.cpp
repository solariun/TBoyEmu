#include "RomBrowser.h"

#include <dirent.h>

#include <algorithm>

#include "../util/RomLoader.h"

namespace term {

RomBrowser::RomBrowser(Terminal& terminal, Keyboard& keyboard, const std::string& directory)
    : terminal_(terminal), keyboard_(keyboard), directory_(directory), selected_(0), scroll_(0) {
    DIR* dir = opendir(directory.c_str());
    if (dir) {
        while (dirent* ent = readdir(dir)) {
            std::string file = ent->d_name;
            if (file.empty() || file[0] == '.' || !util::RomLoader::isRomFile(file)) continue;
            Entry e;
            e.path = directory + "/" + file;
            e.name = file.substr(0, file.find_last_of('.'));
            entries_.push_back(e);
        }
        closedir(dir);
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return util::toLower(a.name) < util::toLower(b.name);
    });
    applyFilter();
}

void RomBrowser::applyFilter() {
    visible_.clear();
    std::string needle = util::toLower(filter_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (needle.empty() || util::toLower(entries_[i].name).find(needle) != std::string::npos) {
            visible_.push_back(i);
        }
    }
    if (selected_ >= visible_.size()) selected_ = visible_.empty() ? 0 : visible_.size() - 1;
}

void RomBrowser::draw(const std::string& message) {
    Size s = terminal_.size();
    int listRows = std::max(1, s.rows - 5);
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + static_cast<size_t>(listRows)) scroll_ = selected_ - static_cast<size_t>(listRows) + 1;
    size_t width = static_cast<size_t>(std::max(10, s.cols - 4));

    std::string out = "\x1b[?2026h\x1b[0m\x1b[2J\x1b[1;1H";
    out += "\x1b[1;38;5;231;48;5;25m TBoyEmu \x1b[0m  Game Boy Color for the terminal - ";
    out += std::to_string(visible_.size()) + "/" + std::to_string(entries_.size()) + " ROMs";
    out += "\x1b[2;1H\x1b[38;5;250m Search: \x1b[0m" + filter_ + "\x1b[38;5;244m_\x1b[0m";

    for (int r = 0; r < listRows; ++r) {
        size_t i = scroll_ + static_cast<size_t>(r);
        if (i >= visible_.size()) break;
        out += "\x1b[" + std::to_string(r + 4) + ";1H";
        std::string name = entries_[visible_[i]].name.substr(0, width);
        if (i == selected_) {
            out += "\x1b[1;38;5;16;48;5;220m > " + name + std::string(width - name.size(), ' ') + "\x1b[0m";
        } else {
            out += "   " + name;
        }
    }
    if (visible_.empty()) out += "\x1b[4;1H   (no matching ROMs)";

    out += "\x1b[" + std::to_string(s.rows) + ";1H\x1b[38;5;244m";
    if (!message.empty()) {
        out += "\x1b[38;5;203m" + message.substr(0, static_cast<size_t>(s.cols));
    } else {
        out += " Up/Down/PgUp/PgDn: move   Enter: play   type: search   Esc: quit";
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
