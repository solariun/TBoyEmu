#ifndef TBOYEMU_FRONTEND_TERMINAL_H
#define TBOYEMU_FRONTEND_TERMINAL_H

#include <termios.h>

#include <string>

namespace term {

struct Size {
    int cols;
    int rows;
};

// RAII owner of the controlling terminal: raw mode, alternate screen,
// hidden cursor and (when available) the kitty keyboard protocol, which
// reports key *release* events.  Everything is restored by the destructor.
class Terminal {
public:
    explicit Terminal(bool allowKittyKeyboard);
    ~Terminal();

    Size size() const;
    bool kittyKeyboard() const { return kitty_; }

    void write(const std::string& data);
    void write(const char* data, size_t size);

    // Reads whatever input is available, waiting at most timeoutMs
    // (0 = don't wait, -1 = forever).  Returns the number of bytes appended.
    size_t read(std::string& out, int timeoutMs);

    // Signal flags (set asynchronously by the handlers).
    static bool consumeResize();
    static bool terminateRequested();

private:
    Terminal(const Terminal&);
    Terminal& operator=(const Terminal&);

    bool detectKittyKeyboard();

    termios saved_;
    bool kitty_;
};

}  // namespace term

#endif
