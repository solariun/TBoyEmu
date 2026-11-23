#include "Keyboard.h"

#include <cstdlib>

namespace term {

namespace {

// Parses "a:b:c" into up to three numbers (missing => default).
void parseField(const std::string& field, unsigned* values, int count, unsigned def) {
    for (int i = 0; i < count; ++i) values[i] = def;
    size_t start = 0;
    for (int i = 0; i < count && start <= field.size(); ++i) {
        size_t colon = field.find(':', start);
        std::string part = field.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!part.empty()) values[i] = static_cast<unsigned>(std::strtoul(part.c_str(), 0, 10));
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
}

KeyAction actionFromEvent(unsigned event) {
    if (event == 2) return KeyAction::Repeat;
    if (event == 3) return KeyAction::Release;
    return KeyAction::Press;
}

}  // namespace

void KeyDecoder::push(std::vector<KeyEvent>& out, Key key, std::uint32_t cp, KeyAction action) {
    KeyEvent e;
    e.key = key;
    e.codepoint = cp;
    e.action = action;
    out.push_back(e);
}

void KeyDecoder::flushLoneEscape(std::vector<KeyEvent>& out) {
    if (hasLoneEscape()) {
        push(out, Key::Escape, 27, KeyAction::Press);
        buffer_.clear();
    }
}

void KeyDecoder::decode(std::vector<KeyEvent>& out) {
    size_t i = 0;
    while (i < buffer_.size()) {
        unsigned char c = static_cast<unsigned char>(buffer_[i]);
        if (c == 0x1B) {
            if (i + 1 >= buffer_.size()) break;  // maybe the start of a sequence
            size_t used = decodeEscape(i, out);
            if (used == 0) break;  // incomplete sequence: wait for more bytes
            i += used;
            continue;
        }
        if (c == 0x03) {
            push(out, Key::CtrlC, 0, KeyAction::Press);
        } else if (c == '\r' || c == '\n') {
            push(out, Key::Enter, 13, KeyAction::Press);
        } else if (c == 0x7F || c == 0x08) {
            push(out, Key::Backspace, 127, KeyAction::Press);
        } else if (c == '\t') {
            push(out, Key::Tab, 9, KeyAction::Press);
        } else if (c >= 0x20 && c < 0x7F) {
            std::uint32_t cp = (c >= 'A' && c <= 'Z') ? c + 32u : c;
            push(out, Key::Char, cp, KeyAction::Press);
        }
        // Other control bytes and UTF-8 text are ignored.
        ++i;
    }
    buffer_.erase(0, i);
}

size_t KeyDecoder::decodeEscape(size_t at, std::vector<KeyEvent>& out) {
    char next = buffer_[at + 1];

    if (next == 'O') {  // SS3: application cursor keys
        if (at + 2 >= buffer_.size()) return 0;
        switch (buffer_[at + 2]) {
            case 'A': push(out, Key::Up, 0, KeyAction::Press); break;
            case 'B': push(out, Key::Down, 0, KeyAction::Press); break;
            case 'C': push(out, Key::Right, 0, KeyAction::Press); break;
            case 'D': push(out, Key::Left, 0, KeyAction::Press); break;
            case 'H': push(out, Key::Home, 0, KeyAction::Press); break;
            case 'F': push(out, Key::End, 0, KeyAction::Press); break;
            default: break;
        }
        return 3;
    }

    if (next != '[') {
        // ESC followed by an ordinary byte: an Esc key press (or Alt+key).
        push(out, Key::Escape, 27, KeyAction::Press);
        return 1;
    }

    // CSI: parameters 0x30-0x3F, intermediates 0x20-0x2F, final 0x40-0x7E.
    size_t end = at + 2;
    while (end < buffer_.size()) {
        unsigned char b = static_cast<unsigned char>(buffer_[end]);
        if (b >= 0x40 && b <= 0x7E) break;
        ++end;
    }
    if (end >= buffer_.size()) {
        if (buffer_.size() - at > 64) return buffer_.size() - at;  // garbage, drop
        return 0;
    }
    const size_t used = end - at + 1;
    const char final = buffer_[end];
    std::string params = buffer_.substr(at + 2, end - at - 2);
    if (!params.empty() && (params[0] == '?' || params[0] == '>' || params[0] == '<' || params[0] == '=')) {
        return used;  // terminal reply (device attributes etc.), not a key
    }

    std::string first = params.substr(0, params.find(';'));
    std::string second = params.find(';') == std::string::npos ? std::string()
                                                                 : params.substr(params.find(';') + 1);
    unsigned keyField[3];
    unsigned modField[2];
    parseField(first, keyField, 3, 1);
    parseField(second, modField, 2, 1);
    const unsigned mods = modField[0] > 0 ? modField[0] - 1 : 0;
    const KeyAction action = actionFromEvent(modField[1]);

    switch (final) {
        case 'A': push(out, Key::Up, 0, action); break;
        case 'B': push(out, Key::Down, 0, action); break;
        case 'C': push(out, Key::Right, 0, action); break;
        case 'D': push(out, Key::Left, 0, action); break;
        case 'H': push(out, Key::Home, 0, action); break;
        case 'F': push(out, Key::End, 0, action); break;
        case 'I': if (params.empty()) push(out, Key::FocusIn, 0, KeyAction::Press); break;
        case 'O': if (params.empty()) push(out, Key::FocusOut, 0, KeyAction::Press); break;
        case '~':
            switch (keyField[0]) {
                case 1: case 7: push(out, Key::Home, 0, action); break;
                case 4: case 8: push(out, Key::End, 0, action); break;
                case 5: push(out, Key::PageUp, 0, action); break;
                case 6: push(out, Key::PageDown, 0, action); break;
                default: break;
            }
            break;
        case 'u': {  // kitty keyboard protocol
            unsigned cp = keyField[0];
            if (cp == 27) {
                push(out, Key::Escape, cp, action);
            } else if (cp == 13 || cp == 57414) {  // Enter / keypad Enter
                push(out, Key::Enter, 13, action);
            } else if (cp == 9) {
                push(out, Key::Tab, cp, action);
            } else if (cp == 127 || cp == 8) {
                push(out, Key::Backspace, 127, action);
            } else if ((mods & 4) && (cp == 'c' || cp == 'C')) {
                push(out, Key::CtrlC, 0, action);
            } else if (cp >= 32 && cp < 57344) {  // skip functional/modifier keys
                if (cp >= 'A' && cp <= 'Z') cp += 32;
                push(out, Key::Char, cp, action);
            }
            break;
        }
        default:
            break;
    }
    return used;
}

void Keyboard::poll(std::vector<KeyEvent>& out, int timeoutMs) {
    std::string bytes;
    terminal_.read(bytes, timeoutMs);
    decoder_.feed(bytes);
    decoder_.decode(out);
    if (decoder_.hasLoneEscape()) {
        // Distinguish the Esc key from the beginning of an escape sequence.
        std::string more;
        terminal_.read(more, 30);
        if (more.empty()) {
            decoder_.flushLoneEscape(out);
        } else {
            decoder_.feed(more);
            decoder_.decode(out);
        }
    }
}

}  // namespace term
