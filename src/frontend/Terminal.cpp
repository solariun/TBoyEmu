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

volatile sig_atomic_t gResized = 0;
volatile sig_atomic_t gTerminate = 0;

void onResize(int) { gResized = 1; }
void onTerminate(int) { gTerminate = 1; }

// Kitty keyboard protocol flags: 1 = disambiguate escape codes,
// 2 = report event types (press/repeat/release), 8 = report all keys as
// escape codes.
const char kKittyPush[] = "\x1b[>11u";
const char kKittyPop[] = "\x1b[<u";

}  // namespace

Terminal::Terminal(bool allowKittyKeyboard) : kitty_(false) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        throw std::runtime_error("stdin/stdout must be a terminal");
    }
    if (tcgetattr(STDIN_FILENO, &saved_) != 0) throw std::runtime_error("tcgetattr failed");

    termios raw = saved_;
    raw.c_iflag &= static_cast<tcflag_t>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    struct sigaction sa;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = onResize;
    sigaction(SIGWINCH, &sa, 0);
    sa.sa_handler = onTerminate;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGHUP, &sa, 0);
    sigaction(SIGINT, &sa, 0);

    // Alternate screen, hide cursor, disable line wrap, clear.
    // Alternate screen, hide cursor, no line wrap, focus reporting, clear.
    write("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1004h\x1b[0m\x1b[2J");

    if (allowKittyKeyboard && detectKittyKeyboard()) {
        write(kKittyPush);
        kitty_ = true;
    }
}

Terminal::~Terminal() {
    if (kitty_) write(kKittyPop);
    write("\x1b[0m\x1b[2J\x1b[?1004l\x1b[?7h\x1b[?25h\x1b[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
}

bool Terminal::detectKittyKeyboard() {
    // Query the protocol state, followed by Primary Device Attributes which
    // every terminal answers.  If the "CSI ? flags u" reply arrives before
    // the DA reply, the protocol is supported.
    write("\x1b[?u\x1b[c");
    std::string reply;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        read(reply, 50);
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
    return false;
}

Size Terminal::size() const {
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
    pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ready = poll(&pfd, 1, timeoutMs);
    if (ready <= 0 || !(pfd.revents & POLLIN)) return 0;
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
    if (!gResized) return false;
    gResized = 0;
    return true;
}

bool Terminal::terminateRequested() { return gTerminate != 0; }

}  // namespace term
