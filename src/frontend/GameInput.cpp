#include "GameInput.h"

#include <cstdio>

namespace term {

using gb::Button;

namespace {
const int kDirectionCount = 4;  // Right, Left, Up, Down come first in gb::Button
const int kNativeMissLimit = 3;
const std::chrono::milliseconds kTapHold(60);       // keeps very short taps visible
const std::chrono::milliseconds kIdleDisarm(1500);  // see update()
}  // namespace

GameInput::GameInput(bool kittyKeyboard, std::unique_ptr<NativeKeyboard> native, KeyRepeatTiming repeat,
                     int holdMs, int repeatHoldMs)
    : mode_(kittyKeyboard ? Mode::Kitty : (native ? Mode::Native : Mode::Timed)),
      native_(std::move(native)),
      fixedHoldMs_(holdMs),
      fixedRepeatHoldMs_(repeatHoldMs),
      repeatDelayMs_(repeat.delayMs),
      repeatIntervalMs_(repeat.intervalMs),
      nativeVerified_(false),
      nativeMisses_(0),
      focused_(true),
      armed_(false),
      exit_(false),
      pauseToggle_(false),
      screenshot_(false) {
    releaseAll();
    if (mode_ == Mode::Native && native_->permissionMissing()) {
        // Ask right away instead of after a few failed key checks.
        native_->requestPermission();
        notice_ = std::string("Long presses / several keys need OS key access: ") + native_->fixHint();
    }
}

GameInput::Clock::duration GameInput::initialHold() const {
    // Until the first auto-repeat arrives we cannot tell a tap from a hold,
    // so keep the button down for the whole repeat delay.
    double ms = fixedHoldMs_ > 0 ? fixedHoldMs_ : repeatDelayMs_ + 80;
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
}

GameInput::Clock::duration GameInput::repeatHold() const {
    // Tolerate one late/lost repeat before treating the key as released.
    double ms = fixedRepeatHoldMs_ > 0 ? fixedRepeatHoldMs_ : 2 * repeatIntervalMs_ + 40;
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
}

void GameInput::learnRepeat(int i, Clock::time_point now) {
    double gap = std::chrono::duration<double, std::milli>(now - lastPress_[i]).count();
    if (repeatCount_[i] == 0) {
        if (gap > repeatDelayMs_ * 0.5 && gap < repeatDelayMs_ * 2) repeatDelayMs_ = 0.8 * repeatDelayMs_ + 0.2 * gap;
    } else {
        if (gap > repeatIntervalMs_ * 0.5 && gap < repeatIntervalMs_ * 2) {
            repeatIntervalMs_ = 0.8 * repeatIntervalMs_ + 0.2 * gap;
        }
    }
}

void GameInput::releaseAll() {
    for (int i = 0; i < gb::kButtonCount; ++i) {
        pressed_[i] = false;
        timedDown_[i] = false;
        releaseAt_[i] = Clock::time_point();
        repeatCount_[i] = 0;
        lastPress_[i] = Clock::time_point();
    }
}

std::string GameInput::description() const {
    switch (mode_) {
        case Mode::Kitty: return "exact (kitty)";
        case Mode::Native:
            return std::string(nativeVerified_ ? "exact (" : "probing (") + native_->name() + ")";
        default: {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "timed (repeat %d/%d ms)", static_cast<int>(repeatDelayMs_ + 0.5),
                          static_cast<int>(repeatIntervalMs_ + 0.5));
            return buf;
        }
    }
}

bool GameInput::mapKey(const KeyEvent& e, Button& button) {
    switch (e.key) {
        case Key::Up: button = Button::Up; return true;
        case Key::Down: button = Button::Down; return true;
        case Key::Left: button = Button::Left; return true;
        case Key::Right: button = Button::Right; return true;
        case Key::Enter: button = Button::Start; return true;
        case Key::Char:
            switch (e.codepoint) {
                case 'x': button = Button::A; return true;
                case 'z': button = Button::B; return true;
                case 'c': button = Button::Select; return true;
                case ' ': button = Button::Start; return true;
                case 'a': button = Button::L; return true;
                case 'd': button = Button::R; return true;
                default: return false;
            }
        default: return false;
    }
}

void GameInput::timedPress(int i, Clock::time_point now) {
    // A new direction means the terminal stopped repeating the old one, and
    // opposite directions cannot be held together anyway.
    if (i < kDirectionCount && !timedDown_[i]) {
        for (int d = 0; d < kDirectionCount; ++d) {
            if (d != i) timedDown_[d] = false;
        }
    }
    if (timedDown_[i]) {
        learnRepeat(i, now);  // an auto-repeat of a key that is still held
        ++repeatCount_[i];
    } else {
        repeatCount_[i] = 0;
    }
    lastPress_[i] = now;
    Clock::time_point until = now + (repeatCount_[i] == 0 ? initialHold() : repeatHold());
    if (!timedDown_[i] || until > releaseAt_[i]) releaseAt_[i] = until;
    timedDown_[i] = true;
}

void GameInput::nativePress(int i, Clock::time_point now) {
    if (native_->isDown(static_cast<Button>(i))) {
        if (!nativeVerified_) {
            // From now on the OS state is authoritative: drop timed holds.
            nativeVerified_ = true;
            notice_.clear();
            for (int b = 0; b < gb::kButtonCount; ++b) {
                timedDown_[b] = false;
                releaseAt_[b] = Clock::time_point();
            }
        }
        nativeMisses_ = 0;
        return;
    }
    // The terminal says the key was pressed but the OS doesn't see it: a
    // very short tap (keep it visible for a moment) or a backend that
    // cannot read the keyboard (count it, give up after a few).
    if (nativeVerified_) {
        releaseAt_[i] = now + kTapHold;
    } else if (++nativeMisses_ >= kNativeMissLimit) {
        fallBackToTimed();
    }
}

void GameInput::fallBackToTimed() {
    mode_ = Mode::Timed;
    native_->requestPermission();
    notice_ = std::string("Long presses / several keys need OS key access: ") + native_->fixHint();
}

void GameInput::handle(const KeyEvent& e, Clock::time_point now) {
    if (e.key == Key::FocusIn) {
        focused_ = true;
        return;
    }
    if (e.key == Key::FocusOut) {
        focused_ = false;
        armed_ = false;
        releaseAll();
        return;
    }
    if (e.key == Key::Escape || e.key == Key::CtrlC) {
        if (e.action == KeyAction::Press) exit_ = true;
        return;
    }
    if (e.key == Key::Char && e.codepoint == 'p') {
        if (e.action == KeyAction::Press) pauseToggle_ = true;
        return;
    }
    if (e.key == Key::Char && e.codepoint == 's') {
        if (e.action == KeyAction::Press) screenshot_ = true;  // ignore auto-repeat
        return;
    }
    Button button;
    if (!mapKey(e, button)) return;
    const int i = static_cast<int>(button);

    switch (mode_) {
        case Mode::Kitty:
            pressed_[i] = e.action != KeyAction::Release;
            break;
        case Mode::Native:
            armed_ = true;
            focused_ = true;  // typing here means this window has the focus
            lastTerminalKey_ = now;
            if (!nativeVerified_) timedPress(i, now);  // until the backend proves it works
            nativePress(i, now);
            break;
        case Mode::Timed:
            if (e.action == KeyAction::Release) {
                timedDown_[i] = false;
            } else {
                timedPress(i, now);
            }
            break;
    }
}

void GameInput::update(Clock::time_point now) {
    if (mode_ == Mode::Kitty) return;

    if (mode_ == Mode::Timed) {
        for (int i = 0; i < gb::kButtonCount; ++i) {
            if (timedDown_[i] && now >= releaseAt_[i]) timedDown_[i] = false;
            pressed_[i] = timedDown_[i];
        }
        return;
    }

    // Native: the OS state is global, so it is only trusted while this
    // terminal is being used (focused and receiving keys).  Holding keys
    // keeps it armed; after a quiet period with nothing held it disarms, so
    // typing in another window cannot control the game.
    bool anyDown = false;
    for (int i = 0; i < gb::kButtonCount; ++i) {
        bool down = armed_ && focused_ && native_->isDown(static_cast<Button>(i));
        anyDown = anyDown || down;
        if (nativeVerified_) {
            pressed_[i] = down || now < releaseAt_[i];
        } else {
            if (timedDown_[i] && now >= releaseAt_[i]) timedDown_[i] = false;
            pressed_[i] = down || timedDown_[i];
        }
    }
    if (armed_ && !anyDown && now - lastTerminalKey_ > kIdleDisarm) armed_ = false;
}

void GameInput::applyTo(gb::Machine& machine) const {
    for (int i = 0; i < gb::kButtonCount; ++i) machine.setButton(static_cast<Button>(i), pressed_[i]);
}

bool GameInput::consumePauseToggle() {
    bool t = pauseToggle_;
    pauseToggle_ = false;
    return t;
}

}  // namespace term

namespace term {

bool GameInput::consumeScreenshotRequest() {
    bool s = screenshot_;
    screenshot_ = false;
    return s;
}

}  // namespace term
