// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/NativeKeyboard.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Declares the interface for reading the *physical* keyboard state from the
//  operating system (which keys are held down right now), bypassing the
//  terminal, plus systemKeyRepeat(), which reads the OS key auto-repeat
//  timing. The platform backends live in NativeKeyboard.cpp.
//
//  How it fits in
//  --------------
//  EmulatorSession::run() calls NativeKeyboard::create() only when the
//  terminal has no kitty keyboard protocol, --no-native was not given and
//  the session is not over SSH. The result is handed to GameInput, which
//  uses it in its "Native" mode. systemKeyRepeat() is always called and
//  feeds GameInput's "Timed" fallback mode.
//
//  Terminal concepts
//  -----------------
//  Why this exists: a classic terminal turns key presses into bytes and has
//  no way to report that a key was released. While a key is held, the OS
//  auto-repeat sends the same bytes again (after a delay, typically
//  ~500 ms, then every ~30-80 ms), but only for the most recently pressed
//  key. So through the terminal alone a game cannot know how long a button
//  is held, nor that two keys are held together (run + jump). The OS, on
//  the other hand, knows the exact state of every key:
//    macOS  CGEventSourceKeyState() from CoreGraphics; reading global key
//           state requires the "Input Monitoring" privacy permission.
//    Linux  evdev: the EVIOCGKEY ioctl on /dev/input/event* returns the
//           bitmap of pressed keys (needs read access, usually the "input"
//           group). Fallback: X11 XQueryKeymap() through a libX11 loaded at
//           run time with dlopen(), which works without special permission
//           but only sees keys typed into X11 windows.
//  The OS state is global (it also sees keys typed into other windows), so
//  GameInput only trusts it while the terminal has focus and is receiving
//  key bytes.
//
//  Contributing
//  ------------
//  * Backends are indexed by gb::Button order (Right, Left, Up, Down, A, B,
//    Select, Start, L, R); keep their key tables in that order.
//  * A backend may "open" fine and still never see keys (permission denied,
//    Wayland). GameInput detects this and falls back; do not try to be
//    clever about it here.
//  * Only the macOS block compiles on macOS and only the Linux block on
//    Linux: test changes on both systems.
// ============================================================================
#ifndef TBOYEMU_FRONTEND_NATIVEKEYBOARD_H
#define TBOYEMU_FRONTEND_NATIVEKEYBOARD_H

#include <memory>
#include <string>

#include "../core/Types.h"

namespace term {

// Keyboard auto-repeat settings: a held key starts repeating after
// delayMs, then repeats every intervalMs.
// Both in milliseconds. Used by GameInput's timed mode to guess when a key
// was released (no repeat arrived in time -> released).
struct KeyRepeatTiming {
    int delayMs;
    int intervalMs;
};

// Reads them from the OS (macOS IOHIDSystem, Linux X11); sensible defaults
// otherwise.
// Defaults: 500/83 ms on macOS, 500/33 ms elsewhere.
KeyRepeatTiming systemKeyRepeat();

// Reads the *physical* keyboard state from the operating system, bypassing
// the terminal.  Terminals without the kitty protocol never report key
// releases and only auto-repeat the most recent key, so "hold X to
// accelerate and steer with the arrows" cannot work through them; the OS
// knows the exact state of every key.
//
// Backends:
//   macOS  CoreGraphics CGEventSourceKeyState (may need the terminal app to
//          have "Input Monitoring" permission)
//   Linux  evdev /dev/input/event* (needs read access: the "input" group),
//          else X11 XQueryKeymap (libX11 loaded at runtime, X11 sessions)
//
// Ownership: created by create(), owned by GameInput through a unique_ptr.
// isDown() is polled once or more per frame, so it must be cheap.
class NativeKeyboard {
public:
    virtual ~NativeKeyboard() {}

    // Best backend for this machine, or null if none can be opened.
    // macOS: always returns the CoreGraphics backend (even without
    // permission). Linux: evdev if any keyboard device is readable, else X11
    // if $DISPLAY is set and libX11 loads, else null. Other OSes: null.
    static std::unique_ptr<NativeKeyboard> create();

    // True if any physical key mapped to `button` is held down right now.
    virtual bool isDown(gb::Button button) const = 0;
    // Short backend name for the status bar: "macOS", "evdev" or "X11".
    virtual const char* name() const = 0;
    // Short advice shown when the backend turns out not to see key presses.
    virtual std::string fixHint() const = 0;
    // Ask the OS for access (macOS shows its permission prompt).
    virtual void requestPermission() {}
    // True when it is already known that the OS will refuse access.
    virtual bool permissionMissing() const { return false; }
};

}  // namespace term

#endif
