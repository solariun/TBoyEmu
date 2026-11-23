#include "NativeKeyboard.h"

#include <vector>

#if defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#elif defined(__linux__)
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#endif

namespace term {

#if defined(__APPLE__)
// ================================================================ macOS ====

namespace {

class MacKeyboard : public NativeKeyboard {
public:
    bool isDown(gb::Button button) const {
        // macOS virtual key codes (HIToolbox/Events.h), indexed by gb::Button.
        static const CGKeyCode keys[gb::kButtonCount][2] = {
            {124, 124},  // Right arrow
            {123, 123},  // Left arrow
            {126, 126},  // Up arrow
            {125, 125},  // Down arrow
            {7, 7},      // X -> A
            {6, 6},      // Z -> B
            {8, 8},      // C -> Select
            {49, 36},    // Space / Return -> Start
            {0, 0},      // A -> L
            {2, 2},      // D -> R
        };
        const CGKeyCode* k = keys[static_cast<int>(button)];
        return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, k[0]) ||
               CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, k[1]);
    }
    const char* name() const { return "macOS"; }
    std::string fixHint() const {
        return "allow your terminal app in System Settings > Privacy & Security > Input Monitoring, "
               "then restart it";
    }
    void requestPermission() {
        if (!CGPreflightListenEventAccess()) CGRequestListenEventAccess();
    }
};

}  // namespace

std::unique_ptr<NativeKeyboard> NativeKeyboard::create() {
    return std::unique_ptr<NativeKeyboard>(new MacKeyboard());
}

#elif defined(__linux__)
// ================================================================ Linux ====

namespace {

// Linux input event codes, indexed by gb::Button (two keys per button).
const int kEvdevKeys[gb::kButtonCount][2] = {
    {KEY_RIGHT, KEY_RIGHT}, {KEY_LEFT, KEY_LEFT}, {KEY_UP, KEY_UP},       {KEY_DOWN, KEY_DOWN},
    {KEY_X, KEY_X},         {KEY_Z, KEY_Z},       {KEY_C, KEY_C},         {KEY_SPACE, KEY_ENTER},
    {KEY_A, KEY_A},         {KEY_D, KEY_D},
};

// dlsym() returns void*; copying avoids the object->function pointer cast
// that ISO C++ (-Wpedantic) complains about.
template <typename Fn>
Fn loadSymbol(void* lib, const char* name) {
    void* p = dlsym(lib, name);
    Fn fn;
    std::memcpy(&fn, &p, sizeof(fn));
    return fn;
}

inline bool testBit(const unsigned char* bits, int bit) { return (bits[bit / 8] >> (bit % 8)) & 1; }

// Kernel input devices: EVIOCGKEY returns the current key bitmap, so no
// event stream has to be consumed.  Works on the console, X11 and Wayland.
class EvdevKeyboard : public NativeKeyboard {
public:
    ~EvdevKeyboard() {
        for (size_t i = 0; i < fds_.size(); ++i) close(fds_[i]);
    }

    // Opens every readable device that looks like a keyboard.
    bool open() {
        DIR* dir = opendir("/dev/input");
        if (!dir) return false;
        while (dirent* ent = readdir(dir)) {
            if (std::strncmp(ent->d_name, "event", 5) != 0) continue;
            std::string path = std::string("/dev/input/") + ent->d_name;
            int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            unsigned char keys[KEY_MAX / 8 + 1];
            std::memset(keys, 0, sizeof(keys));
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 && testBit(keys, KEY_X) &&
                testBit(keys, KEY_Z) && testBit(keys, KEY_SPACE)) {
                fds_.push_back(fd);
            } else {
                close(fd);
            }
        }
        closedir(dir);
        return !fds_.empty();
    }

    bool isDown(gb::Button button) const {
        const int* k = kEvdevKeys[static_cast<int>(button)];
        for (size_t i = 0; i < fds_.size(); ++i) {
            unsigned char state[KEY_MAX / 8 + 1];
            std::memset(state, 0, sizeof(state));
            if (ioctl(fds_[i], EVIOCGKEY(sizeof(state)), state) < 0) continue;
            if (testBit(state, k[0]) || testBit(state, k[1])) return true;
        }
        return false;
    }
    const char* name() const { return "evdev"; }
    std::string fixHint() const { return "check that /dev/input/event* is your real keyboard"; }

private:
    std::vector<int> fds_;
};

// X11 XQueryKeymap through a runtime-loaded libX11, so the binary neither
// links against X11 nor needs its headers.  Only sees keys typed into X11
// windows (not native Wayland terminals).
class X11Keyboard : public NativeKeyboard {
public:
    X11Keyboard() : lib_(0), display_(0), queryKeymap_(0), closeDisplay_(0) {
        std::memset(codes_, 0, sizeof(codes_));
    }
    ~X11Keyboard() {
        if (display_ && closeDisplay_) closeDisplay_(display_);
        if (lib_) dlclose(lib_);
    }

    bool open() {
        if (!std::getenv("DISPLAY")) return false;
        lib_ = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (!lib_) lib_ = dlopen("libX11.so", RTLD_LAZY | RTLD_LOCAL);
        if (!lib_) return false;
        typedef void* (*OpenFn)(const char*);
        typedef unsigned char (*KeysymToKeycodeFn)(void*, unsigned long);
        OpenFn openDisplay = loadSymbol<OpenFn>(lib_, "XOpenDisplay");
        KeysymToKeycodeFn toKeycode = loadSymbol<KeysymToKeycodeFn>(lib_, "XKeysymToKeycode");
        queryKeymap_ = loadSymbol<QueryFn>(lib_, "XQueryKeymap");
        closeDisplay_ = loadSymbol<CloseFn>(lib_, "XCloseDisplay");
        if (!openDisplay || !toKeycode || !queryKeymap_ || !closeDisplay_) return false;
        display_ = openDisplay(0);
        if (!display_) return false;
        // KeySyms (X11/keysymdef.h), indexed by gb::Button.
        static const unsigned long syms[gb::kButtonCount][2] = {
            {0xFF53, 0xFF53}, {0xFF51, 0xFF51}, {0xFF52, 0xFF52}, {0xFF54, 0xFF54},
            {'x', 'x'},       {'z', 'z'},       {'c', 'c'},       {' ', 0xFF0D},
            {'a', 'a'},       {'d', 'd'},
        };
        for (int b = 0; b < gb::kButtonCount; ++b) {
            for (int j = 0; j < 2; ++j) codes_[b][j] = toKeycode(display_, syms[b][j]);
        }
        return true;
    }

    bool isDown(gb::Button button) const {
        char keymap[32];
        queryKeymap_(display_, keymap);
        const unsigned char* c = codes_[static_cast<int>(button)];
        for (int j = 0; j < 2; ++j) {
            if (c[j] && ((keymap[c[j] / 8] >> (c[j] % 8)) & 1)) return true;
        }
        return false;
    }
    const char* name() const { return "X11"; }
    std::string fixHint() const {
        return "add yourself to the 'input' group (sudo usermod -aG input $USER) and log in again";
    }

private:
    typedef int (*QueryFn)(void*, char*);
    typedef int (*CloseFn)(void*);
    void* lib_;
    void* display_;
    QueryFn queryKeymap_;
    CloseFn closeDisplay_;
    unsigned char codes_[gb::kButtonCount][2];
};

}  // namespace

std::unique_ptr<NativeKeyboard> NativeKeyboard::create() {
    std::unique_ptr<EvdevKeyboard> evdev(new EvdevKeyboard());
    if (evdev->open()) return std::unique_ptr<NativeKeyboard>(evdev.release());
    std::unique_ptr<X11Keyboard> x11(new X11Keyboard());
    if (x11->open()) return std::unique_ptr<NativeKeyboard>(x11.release());
    return std::unique_ptr<NativeKeyboard>();
}

#else
// ========================================================= other systems ====

std::unique_ptr<NativeKeyboard> NativeKeyboard::create() { return std::unique_ptr<NativeKeyboard>(); }

#endif

}  // namespace term
