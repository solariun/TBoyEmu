// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/NativeKeyboard.cpp
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Platform backends for NativeKeyboard (see NativeKeyboard.h) and the
//  systemKeyRepeat() function, selected at compile time with #if:
//    __APPLE__   MacKeyboard: CoreGraphics CGEventSourceKeyState; repeat
//                timing from the IOHIDSystem "HIDParameters" registry entry.
//    __linux__   EvdevKeyboard: EVIOCGKEY on /dev/input/event*;
//                X11Keyboard: XQueryKeymap via a dlopen()ed libX11;
//                repeat timing from XkbGetAutoRepeatRate (also via dlopen).
//    otherwise   no backend (create() returns null) and fixed defaults.
//
//  How it fits in
//  --------------
//  EmulatorSession calls NativeKeyboard::create() and systemKeyRepeat() when
//  a game starts and passes both to GameInput. Nothing here knows about the
//  terminal; GameInput decides when the OS state can be trusted.
//
//  Terminal concepts
//  -----------------
//  Terminals cannot report key releases (see NativeKeyboard.h), so these
//  backends ask the OS directly "is this key down right now?" once per
//  frame. They poll state instead of consuming an event stream, so no
//  extra thread or event loop is needed:
//    * macOS: CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, key)
//      reports the hardware-level state of a virtual key code. Since macOS
//      10.15 this needs the "Input Monitoring" permission for the app that
//      runs us (the terminal app); CGPreflightListenEventAccess() checks it
//      and CGRequestListenEventAccess() shows the system prompt.
//    * Linux evdev: every input device has a /dev/input/eventN node; the
//      EVIOCGKEY ioctl copies a bitmap with one bit per KEY_* code that is
//      currently pressed. Works on the text console, X11 and Wayland, but
//      the nodes are normally readable only by root and the "input" group.
//    * Linux X11: XQueryKeymap() fills 32 bytes = 256 bits, one per X
//      keycode; XKeysymToKeycode() translates key symbols (XK_Left, 'x', ...)
//      into those keycodes for the current keyboard layout.
//  libX11 is loaded with dlopen() so the binary has no link-time X11
//  dependency and builds without X11 headers (the needed constants are
//  written out by hand below).
//
//  Contributing
//  ------------
//  * Comments and code in the Linux block cannot be compiled on macOS and
//    vice versa; check both platforms when editing.
//  * Every key table is indexed by gb::Button (Right, Left, Up, Down, A, B,
//    Select, Start, L, R) with two alternative keys per button (the second
//    one is only different for Start = Space or Enter).
//  * Test: start a game with ./tboyemu --no-kitty and check that the status
//    bar changes from "keys: probing (...)" to "keys: exact (...)" after the
//    first key press, and that holding two keys works.
// ============================================================================
#include "NativeKeyboard.h"

#include <vector>

#if defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>
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

// Backend using CoreGraphics. Stateless: every isDown() call queries the
// window server directly. Without the Input Monitoring permission the
// queries simply report "not pressed", which GameInput detects.
class MacKeyboard : public NativeKeyboard {
public:
    bool isDown(gb::Button button) const {
        // macOS virtual key codes (HIToolbox/Events.h), indexed by gb::Button.
        // (kVK_RightArrow = 124, kVK_LeftArrow = 123, kVK_UpArrow = 126,
        //  kVK_DownArrow = 125, kVK_ANSI_X = 7, kVK_ANSI_Z = 6, kVK_ANSI_C = 8,
        //  kVK_Space = 49, kVK_Return = 36, kVK_ANSI_A = 0, kVK_ANSI_D = 2.)
        // These are physical key positions of the US ANSI layout.
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
        // HIDSystemState = the combined state of the hardware keyboards,
        // independent of which application has focus.
        return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, k[0]) ||
               CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, k[1]);
    }
    const char* name() const { return "macOS"; }
    std::string fixHint() const {
        return "allow your terminal app in System Settings > Privacy & Security > Input Monitoring, "
               "then restart it";
    }
    // Shows the macOS "Input Monitoring" prompt if access was not granted yet.
    // The permission only takes effect after the terminal app is restarted.
    void requestPermission() {
        if (!CGPreflightListenEventAccess()) CGRequestListenEventAccess();
    }
    bool permissionMissing() const { return !CGPreflightListenEventAccess(); }
};

// Reads a nanosecond value from IOHIDSystem's "HIDParameters" dictionary.
// On success stores it in `ms` (converted to milliseconds) and returns true.
// Uses the IOKit registry: IORegistryEntryCreateCFProperty returns a +1
// retained CoreFoundation object that must be CFRelease()d; values fetched
// from the dictionary are borrowed and need no release.
bool hidParameterMs(io_service_t service, CFStringRef key, int& ms) {
    bool ok = false;
    CFTypeRef params = IORegistryEntryCreateCFProperty(service, CFSTR("HIDParameters"), kCFAllocatorDefault, 0);
    CFTypeRef value = 0;
    if (params && CFGetTypeID(params) == CFDictionaryGetTypeID()) {
        value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(params), key);
    }
    long long ns = 0;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID() &&
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &ns) && ns > 0) {
        ms = static_cast<int>(ns / 1000000);
        ok = true;
    }
    if (params) CFRelease(params);
    return ok;
}

}  // namespace

KeyRepeatTiming systemKeyRepeat() {
    // "Delay until repeat" and "Key repeat rate" from System Settings >
    // Keyboard, as stored by the HID system (HIDInitialKeyRepeat and
    // HIDKeyRepeat, in nanoseconds). Fall back to the macOS defaults.
    KeyRepeatTiming t = {500, 83};  // macOS defaults
    io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("IOHIDSystem"));
    if (service) {
        int ms;
        if (hidParameterMs(service, CFSTR("HIDInitialKeyRepeat"), ms)) t.delayMs = ms;
        if (hidParameterMs(service, CFSTR("HIDKeyRepeat"), ms)) t.intervalMs = ms;
        IOObjectRelease(service);
    }
    return t;
}

// On macOS the backend is always created; whether it can actually see keys
// (permission) is checked later through permissionMissing() and GameInput.
std::unique_ptr<NativeKeyboard> NativeKeyboard::create() {
    return std::unique_ptr<NativeKeyboard>(new MacKeyboard());
}

#elif defined(__linux__)
// ================================================================ Linux ====

namespace {

// Linux input event codes, indexed by gb::Button (two keys per button).
// KEY_* constants come from <linux/input.h> (input-event-codes.h).
const int kEvdevKeys[gb::kButtonCount][2] = {
    {KEY_RIGHT, KEY_RIGHT}, {KEY_LEFT, KEY_LEFT}, {KEY_UP, KEY_UP},       {KEY_DOWN, KEY_DOWN},
    {KEY_X, KEY_X},         {KEY_Z, KEY_Z},       {KEY_C, KEY_C},         {KEY_SPACE, KEY_ENTER},
    {KEY_A, KEY_A},         {KEY_D, KEY_D},
};

// dlsym() returns void*; copying avoids the object->function pointer cast
// that ISO C++ (-Wpedantic) complains about.
// Returns a null function pointer if the symbol does not exist.
template <typename Fn>
Fn loadSymbol(void* lib, const char* name) {
    void* p = dlsym(lib, name);
    Fn fn;
    std::memcpy(&fn, &p, sizeof(fn));
    return fn;
}

// Tests bit `bit` in a little-endian byte bitmap, the layout used by the
// evdev EVIOCGBIT / EVIOCGKEY ioctls.
inline bool testBit(const unsigned char* bits, int bit) { return (bits[bit / 8] >> (bit % 8)) & 1; }

// Kernel input devices: EVIOCGKEY returns the current key bitmap, so no
// event stream has to be consumed.  Works on the console, X11 and Wayland.
// Owns one file descriptor per keyboard-like device; closed in the
// destructor. Several keyboards (laptop + USB) are OR-ed together.
class EvdevKeyboard : public NativeKeyboard {
public:
    ~EvdevKeyboard() {
        for (size_t i = 0; i < fds_.size(); ++i) close(fds_[i]);
    }

    // Opens every readable device that looks like a keyboard.
    // "Looks like a keyboard" = EVIOCGBIT(EV_KEY) says it can produce X, Z
    // and Space (this skips mice, power buttons, lid switches...). Devices
    // we may not read (no "input" group membership) are silently skipped.
    // Returns false if no device could be opened.
    bool open() {
        DIR* dir = opendir("/dev/input");
        if (!dir) return false;
        while (dirent* ent = readdir(dir)) {
            if (std::strncmp(ent->d_name, "event", 5) != 0) continue;
            std::string path = std::string("/dev/input/") + ent->d_name;
            // Non-blocking and close-on-exec; we never read events, only
            // issue ioctls, so the device's event queue is left untouched.
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

    // Asks every opened device for its current key bitmap (EVIOCGKEY) and
    // checks the two keys mapped to `button`.
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
    // If evdev opened but never sees the keys, the device found is probably
    // not the keyboard being typed on (e.g. a remote or virtual keyboard).
    std::string fixHint() const { return "check that /dev/input/event* is your real keyboard"; }

private:
    std::vector<int> fds_;  // open /dev/input/eventN descriptors (keyboards only)
};

// X11 XQueryKeymap through a runtime-loaded libX11, so the binary neither
// links against X11 nor needs its headers.  Only sees keys typed into X11
// windows (not native Wayland terminals).
// Owns the dlopen() handle and the X display connection; both are released
// in the destructor.
class X11Keyboard : public NativeKeyboard {
public:
    X11Keyboard() : lib_(0), display_(0), queryKeymap_(0), closeDisplay_(0) {
        std::memset(codes_, 0, sizeof(codes_));
    }
    ~X11Keyboard() {
        if (display_ && closeDisplay_) closeDisplay_(display_);
        if (lib_) dlclose(lib_);
    }

    // Loads libX11, resolves the four functions needed, connects to the
    // X server named by $DISPLAY and translates the key symbols into this
    // server's keycodes. Returns false at the first failure.
    bool open() {
        if (!std::getenv("DISPLAY")) return false;
        lib_ = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (!lib_) lib_ = dlopen("libX11.so", RTLD_LAZY | RTLD_LOCAL);
        if (!lib_) return false;
        // Function pointer types matching the Xlib prototypes, with Display*
        // as void* and KeyCode as unsigned char (we have no X11 headers).
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
        // 0xFF53 XK_Right, 0xFF51 XK_Left, 0xFF52 XK_Up, 0xFF54 XK_Down,
        // 0xFF0D XK_Return; Latin-1 letters and space use their ASCII value.
        static const unsigned long syms[gb::kButtonCount][2] = {
            {0xFF53, 0xFF53}, {0xFF51, 0xFF51}, {0xFF52, 0xFF52}, {0xFF54, 0xFF54},
            {'x', 'x'},       {'z', 'z'},       {'c', 'c'},       {' ', 0xFF0D},
            {'a', 'a'},       {'d', 'd'},
        };
        // Keycode 0 means "no key produces this symbol"; isDown() skips it.
        for (int b = 0; b < gb::kButtonCount; ++b) {
            for (int j = 0; j < 2; ++j) codes_[b][j] = toKeycode(display_, syms[b][j]);
        }
        return true;
    }

    // XQueryKeymap: 32 bytes, bit (keycode % 8) of byte (keycode / 8) is set
    // while that key is down. One round trip to the X server per call.
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
    // The X11 backend failing usually means a Wayland terminal (X11 cannot
    // see its keys); the advice is to enable the evdev backend instead,
    // which create() prefers once /dev/input is readable.
    std::string fixHint() const {
        return "add yourself to the 'input' group (sudo usermod -aG input $USER) and log in again";
    }

private:
    // Xlib signatures: int XQueryKeymap(Display*, char[32]);
    //                  int XCloseDisplay(Display*).
    typedef int (*QueryFn)(void*, char*);
    typedef int (*CloseFn)(void*);
    void* lib_;                  // dlopen() handle of libX11
    void* display_;              // Display* connection to the X server
    QueryFn queryKeymap_;
    CloseFn closeDisplay_;
    unsigned char codes_[gb::kButtonCount][2];  // X keycodes per button, 0 = none
};

}  // namespace

KeyRepeatTiming systemKeyRepeat() {
    // Under X11, XkbGetAutoRepeatRate() returns the current delay and
    // interval in milliseconds. Without $DISPLAY or libX11 (console, pure
    // Wayland) the common desktop defaults are used.
    KeyRepeatTiming t = {500, 33};  // GNOME / KDE defaults
    if (!std::getenv("DISPLAY")) return t;
    void* lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return t;
    typedef void* (*OpenFn)(const char*);
    typedef int (*CloseFn)(void*);
    typedef int (*RateFn)(void*, unsigned, unsigned*, unsigned*);
    OpenFn openDisplay = loadSymbol<OpenFn>(lib, "XOpenDisplay");
    CloseFn closeDisplay = loadSymbol<CloseFn>(lib, "XCloseDisplay");
    RateFn getRate = loadSymbol<RateFn>(lib, "XkbGetAutoRepeatRate");
    if (openDisplay && closeDisplay && getRate) {
        if (void* display = openDisplay(0)) {
            unsigned delay = 0, interval = 0;
            if (getRate(display, 0x0100 /* XkbUseCoreKbd */, &delay, &interval) && delay && interval) {
                t.delayMs = static_cast<int>(delay);
                t.intervalMs = static_cast<int>(interval);
            }
            closeDisplay(display);
        }
    }
    dlclose(lib);
    return t;
}

// Prefer evdev (exact, works everywhere when permitted), then X11, else none.
std::unique_ptr<NativeKeyboard> NativeKeyboard::create() {
    std::unique_ptr<EvdevKeyboard> evdev(new EvdevKeyboard());
    if (evdev->open()) return std::unique_ptr<NativeKeyboard>(evdev.release());
    std::unique_ptr<X11Keyboard> x11(new X11Keyboard());
    if (x11->open()) return std::unique_ptr<NativeKeyboard>(x11.release());
    return std::unique_ptr<NativeKeyboard>();
}

#else
// ========================================================= other systems ====
// No OS backend: GameInput uses the kitty protocol or the timed mode.

std::unique_ptr<NativeKeyboard> NativeKeyboard::create() { return std::unique_ptr<NativeKeyboard>(); }

KeyRepeatTiming systemKeyRepeat() {
    KeyRepeatTiming t = {500, 33};
    return t;
}

#endif

}  // namespace term
