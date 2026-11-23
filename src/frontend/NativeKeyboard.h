#ifndef TBOYEMU_FRONTEND_NATIVEKEYBOARD_H
#define TBOYEMU_FRONTEND_NATIVEKEYBOARD_H

#include <memory>
#include <string>

#include "../core/Types.h"

namespace term {

// Keyboard auto-repeat settings: a held key starts repeating after
// delayMs, then repeats every intervalMs.
struct KeyRepeatTiming {
    int delayMs;
    int intervalMs;
};

// Reads them from the OS (macOS IOHIDSystem, Linux X11); sensible defaults
// otherwise.
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
class NativeKeyboard {
public:
    virtual ~NativeKeyboard() {}

    // Best backend for this machine, or null if none can be opened.
    static std::unique_ptr<NativeKeyboard> create();

    virtual bool isDown(gb::Button button) const = 0;
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
