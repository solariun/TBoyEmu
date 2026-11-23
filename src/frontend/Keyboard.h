// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/Keyboard.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Declares the key event types (Key, KeyAction, KeyEvent) and two classes:
//  KeyDecoder, a pure parser that turns the raw bytes a terminal sends into
//  key events, and Keyboard, a thin wrapper that reads bytes from a Terminal,
//  feeds the decoder and resolves the "is this ESC the Esc key or the start of
//  an escape sequence?" ambiguity with a short timeout.
//
//  How it fits in
//  --------------
//  main.cpp creates one Keyboard next to the Terminal. RomBrowser::choose()
//  and EmulatorSession::run() call Keyboard::poll() to collect events.
//  In the game, the events go to GameInput, which maps them to buttons.
//
//  Terminal concepts
//  -----------------
//  A terminal does not send "key codes"; it sends text. Printable keys send
//  their character, Enter sends '\r', Backspace sends 0x7F, and special keys
//  send escape sequences that start with ESC (0x1B):
//    ESC [ A / B / C / D      arrow keys (CSI form, "normal cursor keys")
//    ESC O A / B / C / D      arrow keys (SS3 form, "application cursor keys")
//    ESC [ 5 ~ / ESC [ 6 ~    Page Up / Page Down
//    ESC [ I / ESC [ O        focus in / focus out (DECSET 1004)
//    ESC [ cp ; mods : ev u   kitty keyboard protocol key event
//  The Esc key itself sends a lone ESC byte, which looks exactly like the
//  beginning of a sequence; the only way to tell them apart is time: if no
//  more bytes follow quickly, it was the Esc key.
//  Without the kitty protocol, a terminal only sends bytes on key press and
//  on OS auto-repeat; key releases are never reported (the VT100-era protocol
//  simply has no message for them). Every such event is decoded as Press.
//
//  Contributing
//  ------------
//  * KeyDecoder must accept input split at any byte boundary: an incomplete
//    sequence stays in the buffer until the rest arrives.
//  * Letters are always reported in lower case so mappings ignore Shift and
//    Caps Lock.
//  * Test interactively: run ./tboyemu in a kitty-protocol terminal and in a
//    plain one (or with --no-kitty) and check the arrows, PgUp/PgDn, Esc
//    and Ctrl+C in the ROM browser.
// ============================================================================
#ifndef TBOYEMU_FRONTEND_KEYBOARD_H
#define TBOYEMU_FRONTEND_KEYBOARD_H

#include <cstdint>
#include <string>
#include <vector>

#include "Terminal.h"

namespace term {

// Logical keys the frontend cares about. Everything else is dropped by the
// decoder (function keys, modifiers alone, non-ASCII text in legacy mode...).
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
    CtrlC,     // byte 0x03 (raw mode turns off SIGINT), or kitty Ctrl+C
    FocusIn,   // terminal window gained focus (DECSET 1004)
    FocusOut   // terminal window lost focus (DECSET 1004)
};

// What happened to the key. Press and Repeat look the same to legacy
// terminals (both are decoded as Press); only the kitty keyboard protocol
// reports Repeat (OS auto-repeat) and Release separately.
enum class KeyAction { Press, Repeat, Release };

// One decoded keyboard event.
struct KeyEvent {
    Key key;
    std::uint32_t codepoint;  // lower-case for letters
                              // (Unicode code point for Key::Char; 13 for Enter,
                              //  27 for Escape, 9 for Tab, 127 for Backspace,
                              //  0 for arrows / navigation / focus keys)
    KeyAction action;
};

// Turns raw terminal bytes into key events.  Understands legacy VT
// sequences and the kitty keyboard protocol (CSI ... u with event types).
//
// Usage: feed() bytes as they arrive, then decode() to extract all complete
// events. Bytes of an unfinished sequence stay buffered between calls.
// Owned by value inside Keyboard; holds no external resources.
class KeyDecoder {
public:
    // Appends raw input bytes to the internal buffer (no decoding yet).
    void feed(const std::string& bytes) { buffer_ += bytes; }
    // Decodes as many complete events as possible.
    // Events are appended to `out`; consumed bytes are removed from the buffer.
    // A trailing lone ESC or a partial sequence is left for the next call.
    void decode(std::vector<KeyEvent>& out);
    // A lone ESC byte that did not grow into a sequence is the Esc key.
    // True when the buffer contains exactly one ESC and nothing else.
    bool hasLoneEscape() const { return buffer_ == "\x1b"; }
    // If hasLoneEscape(), emits an Escape Press event and clears the buffer.
    // Called by Keyboard once the timeout says no sequence is coming.
    void flushLoneEscape(std::vector<KeyEvent>& out);

private:
    // Returns bytes consumed, 0 if the sequence is incomplete.
    // `at` is the index of the ESC byte in buffer_; at + 1 must exist.
    size_t decodeEscape(size_t at, std::vector<KeyEvent>& out);
    // Builds a KeyEvent and appends it to `out`.
    static void push(std::vector<KeyEvent>& out, Key key, std::uint32_t cp, KeyAction action);

    std::string buffer_;  // bytes received but not decoded yet
};

// Convenience: Terminal + KeyDecoder with the lone-ESC timeout handled.
// Created once by main.cpp and shared (by reference) by RomBrowser and
// EmulatorSession, so bytes left over from one screen reach the next.
class Keyboard {
public:
    explicit Keyboard(Terminal& terminal) : terminal_(terminal) {}

    // Collects events, waiting at most timeoutMs for the first byte.
    // timeoutMs = 0 polls without waiting (used once per emulated frame).
    // If a lone ESC is pending, it may wait up to 30 ms more to see whether a
    // sequence follows.
    void poll(std::vector<KeyEvent>& out, int timeoutMs);

private:
    Terminal& terminal_;   // byte source; must outlive this object
    KeyDecoder decoder_;   // keeps partial sequences between polls
};

}  // namespace term

#endif
