// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/GameInput.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Declares term::GameInput, which turns keyboard events into the pressed /
//  released state of the console buttons (D-pad, A, B, Select, Start, and
//  L/R on the GBA), and recognises the frontend hotkeys (Esc, P, S). Its
//  main job is to answer "is this button held right now?" as accurately as
//  the terminal allows, choosing between three key sources ("modes").
//
//  How it fits in
//  --------------
//  EmulatorSession::run() creates one GameInput per game. Every iteration of
//  the frame loop it feeds the KeyEvents from Keyboard::poll() to handle(),
//  calls update() to expire timed holds / sample the OS keyboard, then
//  applyTo() copies the button state into the gb::Machine (Game Boy or GBA).
//  description() and notice() feed the status bar ("keys: ...").
//
//  Terminal concepts: the three key modes
//  --------------------------------------
//  A plain terminal sends bytes when a key is pressed and when the OS
//  auto-repeats it, but never when it is released, and it auto-repeats only
//  the most recently pressed key. Games need held buttons (long jumps,
//  run + jump), so GameInput picks the best available source:
//    Kitty   The kitty keyboard protocol (enabled by Terminal) reports
//            Press, Repeat and Release events: exact, nothing to guess.
//    Native  NativeKeyboard asks the OS (macOS CoreGraphics, Linux evdev or
//            X11) which keys are physically down: exact, any number of keys.
//            Because that state is global, it is only trusted while the
//            terminal has focus and recently received keys ("armed").
//            The backend must first prove it works ("probing" in the status
//            bar): when the terminal reports a key, the OS must also see it.
//            After 3 misses it falls back to Timed.
//    Timed   Only presses are known. A press holds the button for the OS
//            auto-repeat delay (+80 ms), and each auto-repeat extends it by
//            about two repeat intervals; when the repeats stop, the button
//            is released. The repeat timing comes from the OS and is refined
//            from the repeats actually received.
//
//  Contributing
//  ------------
//  * pressed_ is the only state the game sees; every mode must end up
//    writing it (handle() for Kitty, update() for Native and Timed).
//  * FocusOut must release everything so no button stays stuck when the
//    user switches windows.
//  * Test each mode: a kitty-protocol terminal (keys: exact (kitty)), a plain
//    terminal (exact (macOS) / exact (evdev) / exact (X11)), and
//    ./tboyemu --no-kitty --no-native for the timed mode. Try a long jump
//    and holding two keys at once.
// ============================================================================
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
//   R = start / stop recording a .txt movie
//   [ / ] = contrast -5% / +5%   < / > = brightness -5% / +5%
//
// Key state comes from the best available source:
//   Kitty   the terminal reports presses *and* releases (exact)
//   Native  the OS keyboard state (exact, any number of keys held at once)
//   Timed   only presses are known: a press holds the button for a while,
//           refreshed by auto-repeat.  Holding two keys cannot work here,
//           because terminals only auto-repeat the most recent key.
// Native mode verifies itself: if the OS never sees keys the terminal
// reports (no permission, remote session...) it falls back to Timed.
//
// All arrays below are indexed by static_cast<int>(gb::Button), 0..9
// (Right, Left, Up, Down, A, B, Select, Start, L, R).
// Single-threaded; all times use the monotonic steady_clock.
class GameInput {
public:
    typedef std::chrono::steady_clock Clock;
    // The key source in use; can change from Native to Timed at run time.
    enum class Mode { Kitty, Native, Timed };

    // holdMs / repeatHoldMs: fixed timed-mode hold times, or 0 to derive
    // them from the keyboard's auto-repeat timing.
    // kittyKeyboard: Terminal::kittyKeyboard() (selects Kitty mode).
    // native: OS backend or null (selects Native mode when not kitty).
    // repeat: OS auto-repeat timing, the starting point for Timed mode.
    // If the native backend already knows it lacks permission, the OS prompt
    // is requested immediately and a notice is set for the status bar.
    GameInput(bool kittyKeyboard, std::unique_ptr<NativeKeyboard> native, KeyRepeatTiming repeat, int holdMs,
              int repeatHoldMs);

    // Processes one key event received at time `now`: hotkeys set flags,
    // game keys update the button state according to the mode.
    void handle(const KeyEvent& event, Clock::time_point now);
    // Advances time: releases expired timed holds and samples the OS key
    // state (Native). Call once per frame, after handle() for that frame.
    void update(Clock::time_point now);
    // Copies the current button state into the emulated console.
    void applyTo(gb::Machine& machine) const;
    bool isPressed(gb::Button button) const { return pressed_[static_cast<int>(button)]; }
    // Releases every button and forgets all timed state (e.g. on focus loss).
    void releaseAll();

    // True once Esc or Ctrl+C was pressed (stays true).
    bool exitRequested() const { return exit_; }
    // True once per P press (the flag is cleared by the call).
    bool consumePauseToggle();
    bool consumeScreenshotRequest();  // S key
    bool consumeRecordToggle();       // R key: start / stop recording a movie
                                      // (true once per press; cleared by the call)
    // Picture adjustments since the last call, in 5% steps (negative = less):
    // [ / ] contrast, < / > brightness (also , / . : in kitty mode a shifted
    // key arrives as its base key, so < comes as , and > as .).
    void consumeAdjustments(int& contrastSteps, int& brightnessSteps);

    Mode mode() const { return mode_; }
    std::string description() const;  // for the status bar
                                      // e.g. "exact (kitty)", "probing (macOS)",
                                      // "timed (repeat 500/83 ms)"
    // Advice shown instead of the status bar when Native mode failed (how to
    // grant OS key access); empty when there is nothing to say.
    const std::string& notice() const { return notice_; }

private:
    // Translates a key event to a console button. Returns false for keys
    // that are not game buttons.
    static bool mapKey(const KeyEvent& event, gb::Button& button);
    // Timed-mode bookkeeping for a press or auto-repeat of button index i.
    void timedPress(int button, Clock::time_point now);
    // How long a fresh press is held (before any auto-repeat is seen).
    Clock::duration initialHold() const;
    // How long each auto-repeat extends the hold.
    Clock::duration repeatHold() const;
    // Refines repeatDelayMs_ / repeatIntervalMs_ from the gap since the last
    // press of the same button.
    void learnRepeat(int button, Clock::time_point now);
    // Native mode: checks that the OS also sees the key the terminal reported.
    void nativePress(int button, Clock::time_point now);
    // Gives up on the OS backend: switch to Timed and set notice_.
    void fallBackToTimed();

    Mode mode_;
    std::unique_ptr<NativeKeyboard> native_;  // null unless Native was possible
    // Timed mode.  A held key produces a first auto-repeat after
    // repeatDelayMs_ and then one every repeatIntervalMs_; the button stays
    // down across those gaps, so holding a key really holds the button
    // (long press = higher jump).  Both values start from the OS settings
    // and are refined from the repeats actually received.
    int fixedHoldMs_;         // --hold-ms, 0 = derive from repeatDelayMs_
    int fixedRepeatHoldMs_;   // --repeat-hold-ms, 0 = derive from repeatIntervalMs_
    double repeatDelayMs_;    // learned auto-repeat delay (ms)
    double repeatIntervalMs_; // learned auto-repeat interval (ms)
    int repeatCount_[gb::kButtonCount];         // repeats seen since the key went down (0 = fresh press)
    Clock::time_point lastPress_[gb::kButtonCount];  // time of the last press/repeat event

    bool pressed_[gb::kButtonCount];         // what the game sees
    bool timedDown_[gb::kButtonCount];       // press-only bookkeeping
    Clock::time_point releaseAt_[gb::kButtonCount];  // timed: auto-release time;
                                                     // native: tap visibility end

    // Native mode
    bool nativeVerified_;  // the OS has seen at least one reported key: trusted
    int nativeMisses_;     // reported keys the OS did not see (before verified)
    bool focused_;         // terminal window has focus (focus reporting)
    bool armed_;  // terminal input seen recently -> this window is in use
    Clock::time_point lastTerminalKey_;  // last game key received from the terminal

    // Requests from hotkeys, consumed by EmulatorSession.
    bool exit_;
    bool pauseToggle_;
    bool screenshot_;
    bool recordToggle_;
    int contrastSteps_;
    int brightnessSteps_;
    std::string notice_;  // see notice()
};

}  // namespace term

#endif
