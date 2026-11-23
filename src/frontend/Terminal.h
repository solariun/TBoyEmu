// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/Terminal.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Declares term::Terminal, the one object that "owns" the user's terminal
//  while TBoyEmu runs. Constructing it switches the terminal into the state a
//  full-screen game needs (raw mode, alternate screen, hidden cursor, no line
//  wrap, focus reporting, optionally the kitty keyboard protocol); destroying
//  it puts everything back exactly as it was. It also offers the low-level
//  byte I/O used by everything else in src/frontend: write() escape sequences
//  and pixels to stdout, read() raw key bytes from stdin with a timeout.
//
//  How it fits in
//  --------------
//  main.cpp creates exactly one Terminal on the stack (runInteractive()) and
//  passes it by reference to Keyboard (input decoding), TerminalRenderer
//  (drawing frames), RomBrowser (start screen) and EmulatorSession (the game
//  loop). Nothing else touches stdin/stdout directly while it is alive.
//  Headless mode (--headless) never creates a Terminal at all.
//
//  Terminal concepts
//  -----------------
//  * Cooked vs raw mode: by default the kernel's tty driver ("line
//    discipline") buffers input until Enter, echoes typed characters, and
//    turns Ctrl+C into SIGINT. A game needs every key byte immediately and
//    without echo, so the constructor clears ICANON, ECHO, ISIG, ... with
//    tcsetattr(). This is "raw mode". The previous settings are saved in
//    saved_ and restored by the destructor.
//  * Alternate screen (DECSET 1049, "ESC [ ? 1049 h"): a second screen buffer
//    without scrollback. Drawing there leaves the user's shell history intact;
//    "ESC [ ? 1049 l" switches back and the old contents reappear.
//  * Escape sequences: the program talks to the terminal by writing bytes
//    such as ESC (0x1B) followed by '[' - together called CSI, the Control
//    Sequence Introducer - then numeric parameters and a final letter.
//  * Focus reporting (DECSET 1004): the terminal sends "CSI I" when its
//    window gains keyboard focus and "CSI O" when it loses it. GameInput uses
//    this to ignore the OS keyboard state while another window is in front.
//  * Kitty keyboard protocol: an opt-in extension (kitty, Ghostty, WezTerm,
//    foot, recent iTerm2) that reports key presses, repeats AND releases as
//    "CSI ... u" sequences. Plain terminals only send bytes when a key is
//    pressed or auto-repeats, never when it is released. See Terminal.cpp.
//  * Signals: SIGWINCH tells us the window was resized; SIGTERM / SIGHUP /
//    SIGINT ask us to quit. Handlers only set flags, which the main loops poll.
//
//  Contributing
//  ------------
//  * Every mode switched on in the constructor must be switched off in the
//    destructor, in reverse order; otherwise the user's shell is left broken
//    (no echo, invisible cursor...). If that happens, `reset` repairs it.
//  * The class is non-copyable on purpose: two owners would restore the
//    terminal twice.
//  * Everything is single-threaded; the static signal flags are the only
//    state touched asynchronously (from signal handlers).
//  * Test by running ./tboyemu in several terminals (Terminal.app, iTerm2,
//    kitty, xterm...) and checking that the shell is intact after Esc,
//    after Ctrl+C and after closing with `kill <pid>`.
// ============================================================================
#ifndef TBOYEMU_FRONTEND_TERMINAL_H
#define TBOYEMU_FRONTEND_TERMINAL_H

#include <termios.h>

#include <string>

namespace term {

// Terminal window size in character cells (not pixels).
// cols = number of columns (characters per line), rows = number of lines.
struct Size {
    int cols;
    int rows;
};

// RAII owner of the controlling terminal: raw mode, alternate screen,
// hidden cursor and (when available) the kitty keyboard protocol, which
// reports key *release* events.  Everything is restored by the destructor.
//
// Ownership / lifetime: one instance, created by main.cpp before any drawing
// and destroyed when the program leaves runInteractive() (also on exceptions,
// thanks to RAII). All other frontend classes hold a reference to it.
// Threading: single-threaded; only the static signal flags are asynchronous.
class Terminal {
public:
    // Puts the terminal into game mode (raw input, alternate screen, hidden
    // cursor, no auto-wrap, focus reporting) and installs the signal handlers.
    // allowKittyKeyboard: when true, probes for the kitty keyboard protocol
    // and enables it if the terminal answers (the --no-kitty option passes
    // false). Throws std::runtime_error if stdin/stdout is not a terminal.
    explicit Terminal(bool allowKittyKeyboard);
    // Restores everything: pops the kitty mode, leaves the alternate screen,
    // shows the cursor again and reapplies the saved termios settings.
    ~Terminal();

    // Current window size in cells, from the TIOCGWINSZ ioctl. Falls back to
    // 80x24 if the size cannot be read. Cheap enough to call every frame.
    Size size() const;
    // True when the kitty keyboard protocol was detected and enabled, i.e.
    // key events carry Press / Repeat / Release information.
    bool kittyKeyboard() const { return kitty_; }

    // Writes bytes to stdout, retrying until everything is written (the
    // terminal may accept only part of a large frame at a time). Errors other
    // than EINTR / EAGAIN silently drop the rest of the data.
    void write(const std::string& data);
    void write(const char* data, size_t size);

    // Reads whatever input is available, waiting at most timeoutMs
    // (0 = don't wait, -1 = forever).  Returns the number of bytes appended.
    // Bytes are raw: key presses, escape sequences and terminal replies all
    // arrive here; KeyDecoder (Keyboard.h) turns them into key events.
    size_t read(std::string& out, int timeoutMs);

    // Signal flags (set asynchronously by the handlers).
    // consumeResize(): true once after each SIGWINCH (window resized); the
    // flag is cleared by the call, so callers must relayout when it is true.
    static bool consumeResize();
    // terminateRequested(): true after SIGTERM, SIGHUP or SIGINT; stays true.
    static bool terminateRequested();

private:
    // Declared but not defined: the pre-C++11-style way to forbid copying.
    Terminal(const Terminal&);
    Terminal& operator=(const Terminal&);

    // Asks the terminal whether it speaks the kitty keyboard protocol.
    // Returns true if it does. Waits up to 500 ms for the reply.
    bool detectKittyKeyboard();

    termios saved_;  // terminal settings before we switched to raw mode
    bool kitty_;     // kitty keyboard protocol enabled (must be popped on exit)
};

}  // namespace term

#endif
