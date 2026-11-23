#ifndef TBOYEMU_FRONTEND_GAMEINPUT_H
#define TBOYEMU_FRONTEND_GAMEINPUT_H

#include <chrono>
#include <memory>
#include <string>

#include "../core/Machine.h"
#include "Keyboard.h"
#include "NativeKeyboard.h"

namespace term {

// Maps the keyboard to Game Boy buttons.
//
//   Arrows = D-pad   X = A   Z = B   C = Select   Space/Enter = Start
//   A = L   D = R (Game Boy Advance shoulder buttons)
//   Esc = leave the game   P = pause   S = save the screen as a .txt file
//
// Key state comes from the best available source:
//   Kitty   the terminal reports presses *and* releases (exact)
//   Native  the OS keyboard state (exact, any number of keys held at once)
//   Timed   only presses are known: a press holds the button for a while,
//           refreshed by auto-repeat.  Holding two keys cannot work here,
//           because terminals only auto-repeat the most recent key.
// Native mode verifies itself: if the OS never sees keys the terminal
// reports (no permission, remote session...) it falls back to Timed.
class GameInput {
public:
    typedef std::chrono::steady_clock Clock;
    enum class Mode { Kitty, Native, Timed };

    GameInput(bool kittyKeyboard, std::unique_ptr<NativeKeyboard> native, int holdMs, int repeatHoldMs);

    void handle(const KeyEvent& event, Clock::time_point now);
    void update(Clock::time_point now);
    void applyTo(gb::Machine& machine) const;
    bool isPressed(gb::Button button) const { return pressed_[static_cast<int>(button)]; }
    void releaseAll();

    bool exitRequested() const { return exit_; }
    bool consumePauseToggle();
    bool consumeScreenshotRequest();  // S key

    Mode mode() const { return mode_; }
    std::string description() const;  // for the status bar
    const std::string& notice() const { return notice_; }

private:
    static bool mapKey(const KeyEvent& event, gb::Button& button);
    void timedPress(int button, Clock::time_point now);
    void nativePress(int button, Clock::time_point now);
    void fallBackToTimed();

    Mode mode_;
    std::unique_ptr<NativeKeyboard> native_;
    Clock::duration hold_;
    Clock::duration repeatHold_;

    bool pressed_[gb::kButtonCount];         // what the game sees
    bool timedDown_[gb::kButtonCount];       // press-only bookkeeping
    Clock::time_point releaseAt_[gb::kButtonCount];

    // Native mode
    bool nativeVerified_;
    int nativeMisses_;
    bool focused_;
    bool armed_;  // terminal input seen recently -> this window is in use
    Clock::time_point lastTerminalKey_;

    bool exit_;
    bool pauseToggle_;
    bool screenshot_;
    std::string notice_;
};

}  // namespace term

#endif
