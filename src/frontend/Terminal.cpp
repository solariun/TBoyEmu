// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/Terminal.cpp
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Implements term::Terminal (see Terminal.h for the interface): switching the
//  tty to raw mode with termios, entering the alternate screen, enabling
//  focus reporting, probing and enabling the kitty keyboard protocol,
//  installing signal handlers, and the raw write()/read() helpers built on
//  write(2), read(2) and poll(2).
//
//  How it fits in
//  --------------
//  Pure POSIX; no curses library is used. Every other frontend class sends
//  its escape sequences through Terminal::write() and receives key bytes
//  through Terminal::read() (wrapped by Keyboard::poll()).
//
//  Terminal concepts
//  -----------------
//  Escape sequences written here (ESC = byte 0x1B, CSI = "ESC ["):
//    CSI ? 1049 h / l   enter / leave the alternate screen (saves the cursor)
//    CSI ? 25 l / h     hide / show the text cursor
//    CSI ? 7 l / h      disable / enable auto-wrap at the right margin, so
//                       writing the last column never scrolls the screen
//    CSI ? 1004 h / l   enable / disable focus reporting (CSI I / CSI O)
//    CSI 0 m            SGR reset: default colors and attributes
//    CSI 2 J            erase the whole screen
//    CSI > 11 u         kitty keyboard: push flags 11 = 1 + 2 + 8 (see below)
//    CSI < u            kitty keyboard: pop the flags we pushed
//    CSI ? u            kitty keyboard: query current flags (reply CSI ? n u)
//    CSI c              Primary Device Attributes (reply CSI ? ... c)
//  Raw mode flags (termios): ICANON off = no line buffering, ECHO off = typed
//  keys are not printed, ISIG off = Ctrl+C arrives as byte 0x03 instead of
//  SIGINT, IXON off = Ctrl+S/Ctrl+Q are ordinary bytes, ICRNL off = Enter
//  arrives as '\r', OPOST off = "\n" is not translated to "\r\n" on output.
//  VMIN = VTIME = 0 makes read() non-blocking; waiting is done with poll().
//
//  Contributing
//  ------------
//  * Keep constructor and destructor symmetric (see Terminal.h).
//  * Signal handlers must only touch volatile sig_atomic_t flags: anything
//    else (allocating, writing to the terminal) is not async-signal-safe.
//  * kitty detection relies on the terminal answering Device Attributes; a
//    terminal that ignores CSI c costs the full 500 ms timeout at start-up.
//  * Check: c++ -std=c++11 -Wall -Wextra -Wpedantic -fsyntax-only -Isrc
//    src/frontend/Terminal.cpp, then run ./tboyemu and quit in every possible
//    way (Esc, Ctrl+C, kill) to make sure the shell is restored.
// ============================================================================
#include "Terminal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <chrono>
#include <stdexcept>

namespace term {

namespace {

// Flags set by the signal handlers and polled by the main loops.
// volatile sig_atomic_t is the only type that is safe to write from a handler.
volatile sig_atomic_t gResized = 0;    // 1 after SIGWINCH (window resized)
volatile sig_atomic_t gTerminate = 0;  // 1 after SIGTERM / SIGHUP / SIGINT

void onResize(int) { gResized = 1; }
void onTerminate(int) { gTerminate = 1; }

// Kitty keyboard protocol flags: 1 = disambiguate escape codes,
// 2 = report event types (press/repeat/release), 8 = report all keys as
// escape codes.
// 1 + 2 + 8 = 11, hence "CSI > 11 u" (push these flags on the terminal's
// flag stack). With flag 8 even plain letters arrive as "CSI codepoint;mods:event u",
// so letters also get release events. "CSI < u" pops them again on exit.
const char kKittyPush[] = "\x1b[>11u";
const char kKittyPop[] = "\x1b[<u";

}  // namespace

Terminal::Terminal(bool allowKittyKeyboard) : kitty_(false) {
    // Raw mode and escape sequences only make sense on a real terminal, not
    // when input or output is redirected to a file or pipe.
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        throw std::runtime_error("stdin/stdout must be a terminal");
    }
    // Remember the user's settings so the destructor can restore them.
    if (tcgetattr(STDIN_FILENO, &saved_) != 0) throw std::runtime_error("tcgetattr failed");

    // Raw mode (the same flags as cfmakeraw / the classic "kilo" editor):
    //   c_iflag: no break-to-SIGINT, no CR->NL translation, no parity check,
    //            no 8th-bit stripping, no XON/XOFF flow control (Ctrl+S/Q).
    //   c_oflag: no output post-processing ("\n" stays "\n").
    //   c_cflag: 8-bit characters.
    //   c_lflag: no echo, no line buffering, no Ctrl+V literal-next, and no
    //            signals from Ctrl+C / Ctrl+Z (they arrive as plain bytes).
    //   VMIN = 0, VTIME = 0: read() returns immediately with what is there.
    termios raw = saved_;
    raw.c_iflag &= static_cast<tcflag_t>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    // TCSAFLUSH: apply after pending output is written, discard unread input.
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    // Signal handlers. sa_flags = 0 (no SA_RESTART) so a blocking poll() is
    // interrupted by the signal and the loops notice the flag quickly.
    // SIGINT can still arrive from `kill -INT`, even though ISIG is off.
    struct sigaction sa;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = onResize;
    sigaction(SIGWINCH, &sa, 0);
    sa.sa_handler = onTerminate;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGHUP, &sa, 0);
    sigaction(SIGINT, &sa, 0);

    // Alternate screen, hide cursor, no line wrap, focus reporting, clear.
    //   CSI ?1049h  alternate screen      CSI ?25l  hide cursor
    //   CSI ?7l     no auto-wrap          CSI ?1004h  focus in/out reports
    //   CSI 0m      reset colors          CSI 2J    erase screen
    write("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1004h\x1b[0m\x1b[2J");

    // Kitty keyboard protocol: only enabled if the terminal proves it
    // understands it; otherwise "CSI > 11 u" might be printed as garbage.
    if (allowKittyKeyboard && detectKittyKeyboard()) {
        write(kKittyPush);
        kitty_ = true;
    }
}

Terminal::~Terminal() {
    // Undo the constructor in reverse order: pop kitty flags, reset colors,
    // clear, stop focus reports, re-enable wrap, show the cursor, leave the
    // alternate screen (the shell's previous contents reappear), then
    // restore the original termios settings (echo, line buffering, signals).
    if (kitty_) write(kKittyPop);
    write("\x1b[0m\x1b[2J\x1b[?1004l\x1b[?7h\x1b[?25h\x1b[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
}

bool Terminal::detectKittyKeyboard() {
    // Query the protocol state, followed by Primary Device Attributes which
    // every terminal answers.  If the "CSI ? flags u" reply arrives before
    // the DA reply, the protocol is supported.
    // This is the detection method recommended by the kitty documentation:
    // a terminal without the protocol ignores "CSI ? u" and only answers
    // "CSI c" (with "CSI ? <attributes> c"), so we never wait forever.
    write("\x1b[?u\x1b[c");
    std::string reply;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        read(reply, 50);
        // Scan every "ESC [ ?" in what has arrived so far. Skip the digits and
        // ';' that follow; the final byte tells which reply it is:
        //   'u' -> kitty flags reply   -> supported
        //   'c' -> Device Attributes   -> came first, so not supported
        // Any keys the user typed meanwhile are simply discarded.
        size_t da = reply.find("\x1b[?");
        while (da != std::string::npos) {
            size_t end = da + 3;
            while (end < reply.size() && ((reply[end] >= '0' && reply[end] <= '9') || reply[end] == ';')) {
                ++end;
            }
            if (end >= reply.size()) break;  // incomplete, keep reading
            if (reply[end] == 'u') return true;
            if (reply[end] == 'c') return false;
            da = reply.find("\x1b[?", end);
        }
    }
    // No answer within 500 ms: assume a plain terminal.
    return false;
}

Size Terminal::size() const {
    // TIOCGWINSZ asks the tty driver for the window size in cells. It can fail
    // or report 0x0 (e.g. some serial consoles); then assume the classic 80x24.
    winsize ws;
    Size s = {80, 24};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        s.cols = ws.ws_col;
        s.rows = ws.ws_row;
    }
    return s;
}

void Terminal::write(const std::string& data) { write(data.data(), data.size()); }

void Terminal::write(const char* data, size_t size) {
    // write(2) may write fewer bytes than asked (a frame can be 100+ KB), or
    // be interrupted by a signal (EINTR, e.g. SIGWINCH): loop until done.
    // EAGAIN is retried too in case stdout was left non-blocking.
    while (size > 0) {
        ssize_t n = ::write(STDOUT_FILENO, data, size);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
}

size_t Terminal::read(std::string& out, int timeoutMs) {
    // Wait (up to timeoutMs) until stdin has at least one byte. poll() returns
    // 0 on timeout and -1 when interrupted by a signal; both mean "no input".
    pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ready = poll(&pfd, 1, timeoutMs);
    if (ready <= 0 || !(pfd.revents & POLLIN)) return 0;
    // Drain everything available right now. In raw mode with VMIN = 0 read()
    // never blocks; a short read means the input queue is empty.
    char buf[4096];
    size_t total = 0;
    for (;;) {
        ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        total += static_cast<size_t>(n);
        if (static_cast<size_t>(n) < sizeof(buf)) break;
    }
    return total;
}

bool Terminal::consumeResize() {
    // Read-and-clear: each SIGWINCH is reported once.
    if (!gResized) return false;
    gResized = 0;
    return true;
}

bool Terminal::terminateRequested() { return gTerminate != 0; }

}  // namespace term
