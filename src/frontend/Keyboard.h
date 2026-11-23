#ifndef TBOYEMU_FRONTEND_KEYBOARD_H
#define TBOYEMU_FRONTEND_KEYBOARD_H

#include <cstdint>
#include <string>
#include <vector>

#include "Terminal.h"

namespace term {

enum class Key {
    Unknown,
    Char,  // printable character, see KeyEvent::codepoint
    Up,
    Down,
    Left,
    Right,
    Enter,
    Escape,
    Backspace,
    Tab,
    Home,
    End,
    PageUp,
    PageDown,
    CtrlC,
    FocusIn,   // terminal window gained focus (DECSET 1004)
    FocusOut
};

enum class KeyAction { Press, Repeat, Release };

struct KeyEvent {
    Key key;
    std::uint32_t codepoint;  // lower-case for letters
    KeyAction action;
};

// Turns raw terminal bytes into key events.  Understands legacy VT
// sequences and the kitty keyboard protocol (CSI ... u with event types).
class KeyDecoder {
public:
    void feed(const std::string& bytes) { buffer_ += bytes; }
    // Decodes as many complete events as possible.
    void decode(std::vector<KeyEvent>& out);
    // A lone ESC byte that did not grow into a sequence is the Esc key.
    bool hasLoneEscape() const { return buffer_ == "\x1b"; }
    void flushLoneEscape(std::vector<KeyEvent>& out);

private:
    // Returns bytes consumed, 0 if the sequence is incomplete.
    size_t decodeEscape(size_t at, std::vector<KeyEvent>& out);
    static void push(std::vector<KeyEvent>& out, Key key, std::uint32_t cp, KeyAction action);

    std::string buffer_;
};

// Convenience: Terminal + KeyDecoder with the lone-ESC timeout handled.
class Keyboard {
public:
    explicit Keyboard(Terminal& terminal) : terminal_(terminal) {}

    // Collects events, waiting at most timeoutMs for the first byte.
    void poll(std::vector<KeyEvent>& out, int timeoutMs);

private:
    Terminal& terminal_;
    KeyDecoder decoder_;
};

}  // namespace term

#endif
