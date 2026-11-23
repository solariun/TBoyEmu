// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/Keyboard.cpp
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Implements KeyDecoder and Keyboard (see Keyboard.h). The decoder is a
//  small hand-written parser for the byte stream a terminal produces:
//  plain characters and control bytes, SS3 sequences (ESC O x), CSI sequences
//  (ESC [ params final) including the kitty keyboard protocol's "CSI ... u"
//  form, and focus in/out reports. Terminal replies to queries (Device
//  Attributes etc.) are recognised and dropped.
//
//  How it fits in
//  --------------
//  Keyboard::poll() reads bytes with Terminal::read(), feeds them to the
//  KeyDecoder and returns KeyEvents to RomBrowser and EmulatorSession.
//
//  Terminal concepts
//  -----------------
//  CSI sequence grammar (ECMA-48): ESC '[' then parameter bytes 0x30-0x3F
//  (digits, ';', ':', '<', '=', '>', '?'), intermediate bytes 0x20-0x2F, and
//  one final byte 0x40-0x7E that names the sequence ('A' = Up, '~' = keypad
//  style key, 'u' = kitty key event...).
//  Parameters used here, written "a:b:c;d:e":
//    first field  a = key number (legacy '~' keys: 5 = PgUp, 6 = PgDn...;
//                     kitty 'u': the Unicode code point of the key)
//    second field d = 1 + modifier bits (1 = Shift, 2 = Alt, 4 = Ctrl, ...)
//                 e = kitty event type: 1 = press, 2 = repeat, 3 = release
//  Legacy arrow keys with modifiers look like "CSI 1;5C" (Ctrl+Right); with
//  kitty event types a released Up arrow is "CSI 1;1:3A".
//
//  Contributing
//  ------------
//  * Never block inside the decoder: return 0 for "incomplete" and let the
//    caller feed more bytes later.
//  * Unknown sequences must be consumed (not left in the buffer), otherwise
//    decoding stalls forever.
//  * Test with a kitty-protocol terminal and a plain one; `cat -v` in a shell
//    shows what bytes a key sends.
// ============================================================================
#include "Keyboard.h"

#include <cstdlib>

namespace term {

namespace {

// Parses "a:b:c" into up to three numbers (missing => default).
// `values` receives `count` numbers; empty sub-fields keep `def`.
// Example: parseField("1:3", v, 2, 1) -> v = {1, 3}; ("", v, 2, 1) -> {1, 1}.
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

// kitty event type sub-parameter -> KeyAction. 1 (or absent, which defaults
// to 1) = press, 2 = repeat, 3 = release.
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
        // ESC: start of a sequence, or the Esc key on its own.
        if (c == 0x1B) {
            if (i + 1 >= buffer_.size()) break;  // maybe the start of a sequence
            size_t used = decodeEscape(i, out);
            if (used == 0) break;  // incomplete sequence: wait for more bytes
            i += used;
            continue;
        }
        // Single-byte keys. In raw mode Ctrl+C is the byte 0x03 (ETX), Enter
        // is '\r' (ICRNL is off), Backspace is DEL 0x7F (or BS 0x08 on some
        // terminals). Printable ASCII becomes Key::Char, letters lower-cased.
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
    // Keep only the undecoded tail (a partial sequence or a lone ESC).
    buffer_.erase(0, i);
}

size_t KeyDecoder::decodeEscape(size_t at, std::vector<KeyEvent>& out) {
    char next = buffer_[at + 1];

    if (next == 'O') {  // SS3: application cursor keys
        // ESC O <letter>: sent for arrows/Home/End when the terminal is in
        // "application cursor keys" mode (DECCKM). Always 3 bytes.
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
        // Only the ESC is consumed; the following byte is decoded normally.
        push(out, Key::Escape, 27, KeyAction::Press);
        return 1;
    }

    // CSI: parameters 0x30-0x3F, intermediates 0x20-0x2F, final 0x40-0x7E.
    // Find the final byte; everything between "ESC [" and it is parameters.
    size_t end = at + 2;
    while (end < buffer_.size()) {
        unsigned char b = static_cast<unsigned char>(buffer_[end]);
        if (b >= 0x40 && b <= 0x7E) break;
        ++end;
    }
    if (end >= buffer_.size()) {
        // No final byte yet. Wait for more input, unless the "sequence" is
        // already absurdly long (corrupt input): then throw it away so the
        // buffer cannot grow forever.
        if (buffer_.size() - at > 64) return buffer_.size() - at;  // garbage, drop
        return 0;
    }
    const size_t used = end - at + 1;
    const char final = buffer_[end];
    std::string params = buffer_.substr(at + 2, end - at - 2);
    // A private-marker prefix (? > < =) means a reply to one of our queries,
    // e.g. the late answer to "CSI ? u" / "CSI c" from Terminal's kitty probe.
    if (!params.empty() && (params[0] == '?' || params[0] == '>' || params[0] == '<' || params[0] == '=')) {
        return used;  // terminal reply (device attributes etc.), not a key
    }

    // Split "first;second" and parse each into its ':' sub-fields.
    //   keyField[0]  key number / code point        (default 1)
    //   modField[0]  1 + modifier bits              (default 1 = none)
    //   modField[1]  kitty event type 1/2/3         (default 1 = press)
    std::string first = params.substr(0, params.find(';'));
    std::string second = params.find(';') == std::string::npos ? std::string()
                                                                 : params.substr(params.find(';') + 1);
    unsigned keyField[3];
    unsigned modField[2];
    parseField(first, keyField, 3, 1);
    parseField(second, modField, 2, 1);
    // Modifier bits: 1 = Shift, 2 = Alt, 4 = Ctrl, 8 = Super, ...
    const unsigned mods = modField[0] > 0 ? modField[0] - 1 : 0;
    const KeyAction action = actionFromEvent(modField[1]);

    switch (final) {
        // Cursor keys: CSI A/B/C/D, Home/End as CSI H/F.
        case 'A': push(out, Key::Up, 0, action); break;
        case 'B': push(out, Key::Down, 0, action); break;
        case 'C': push(out, Key::Right, 0, action); break;
        case 'D': push(out, Key::Left, 0, action); break;
        case 'H': push(out, Key::Home, 0, action); break;
        case 'F': push(out, Key::End, 0, action); break;
        // Focus reporting (DECSET 1004): parameterless CSI I / CSI O.
        case 'I': if (params.empty()) push(out, Key::FocusIn, 0, KeyAction::Press); break;
        case 'O': if (params.empty()) push(out, Key::FocusOut, 0, KeyAction::Press); break;
        // "VT220-style" keys: CSI <number> ~. 1/7 = Home, 4/8 = End
        // (different terminals use different numbers), 5 = PgUp, 6 = PgDn.
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
            // CSI <code point> ; <1 + mods> : <event type> u
            // With flag 8 ("report all keys as escape codes") every key comes
            // this way, including plain letters, Enter, Tab and Backspace.
            // Code points >= 57344 (the Unicode Private Use Area) are kitty's
            // functional keys: F-keys, keypad, and modifier keys alone.
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
                // Ctrl+C: with the protocol on it is no longer byte 0x03.
                push(out, Key::CtrlC, 0, action);
            } else if (cp >= 32 && cp < 57344) {  // skip functional/modifier keys
                if (cp >= 'A' && cp <= 'Z') cp += 32;
                push(out, Key::Char, cp, action);
            }
            break;
        }
        default:
            // Any other CSI sequence (F-keys, mouse, unknown replies) is
            // consumed and ignored.
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
        // A terminal writes a whole sequence at once, so its remaining bytes
        // arrive within a few ms; 30 ms of silence means the user pressed Esc.
        // (Over slow SSH links a split sequence could be misread as Esc.)
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
