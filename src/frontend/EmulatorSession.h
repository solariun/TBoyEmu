// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/EmulatorSession.h
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Declares the two ways of running a ROM:
//    EmulatorSession  interactive play in the terminal: the real-time frame
//                     loop (input -> emulate -> draw -> wait), status bar,
//                     pause, screen capture and periodic battery saves.
//    HeadlessRunner   no terminal: runs a fixed number of frames as fast as
//                     possible, for tests and benchmarks (--headless).
//
//  How it fits in
//  --------------
//  main.cpp builds one EmulatorSession (with the shared Terminal, Keyboard
//  and Options) and calls run() for every ROM picked in the RomBrowser, or
//  once for a ROM given on the command line; HeadlessRunner is used instead
//  when --headless is given. Both create the console through
//  emu::createMachine() (MachineFactory), which returns a gb::Machine for
//  either a Game Boy / Color or a Game Boy Advance game.
//
//  Terminal concepts
//  -----------------
//  * Frame pacing: the consoles produce 59.7275 frames per second (one
//    frame every ~16.74 ms). The loop keeps a "next deadline" that advances
//    by exactly one frame time, so speed does not drift even though sleeps
//    are imprecise.
//  * Frame skipping: drawing in a terminal can be slower than emulating.
//    When the loop is late, a frame is emulated but not drawn (at most 6 in
//    a row), so the game always runs at the correct speed and only the
//    displayed frame rate drops ("drawn" fps in the status bar).
//  * While waiting for the deadline the loop keeps reading the keyboard, so
//    key events get accurate timestamps (important for timed key mode).
//
//  Contributing
//  ------------
//  * Keep all per-frame work inside the loop cheap; anything slow here
//    lowers the drawn frame rate or makes the game skip frames.
//  * Test interactively with any ROM, and headless:
//      ./tboyemu --headless --frames 3600 cpu_instrs.gb
//      ./tboyemu --headless --frames 600 --input 300:start:5 --screenshot shot.png game.zip
// ============================================================================
#ifndef TBOYEMU_FRONTEND_EMULATORSESSION_H
#define TBOYEMU_FRONTEND_EMULATORSESSION_H

#include <memory>
#include <string>

#include "../core/Machine.h"
#include "ColorMapper.h"
#include "Keyboard.h"
#include "Options.h"
#include "Terminal.h"

namespace term {

// Plays one ROM in the terminal at the original speed (59.73 fps): runs
// the emulated frame, draws it, and sleeps until the next frame deadline.
// When drawing falls behind, frames are emulated but not drawn, so game
// speed never drops.
//
// Lifetime: one instance for the whole program (owned by main.cpp); each
// run() call creates its own Machine, TerminalRenderer and GameInput.
// The ColorMapper tables are cached across runs because building them is
// expensive.
class EmulatorSession {
public:
    // All three references must outlive the session.
    EmulatorSession(Terminal& terminal, Keyboard& keyboard, const Options& options);

    // Returns when the user presses Esc.  Throws on ROM loading errors.
    // Also returns on SIGTERM / SIGHUP / SIGINT. Writes the battery save
    // (<rom>.sav) every 30 s and on exit.
    void run(const std::string& romPath);

private:
    // Returns the cached ColorMapper suitable for this console (with CGB
    // color correction or without), creating it on first use.
    const ColorMapper& mapperFor(const gb::Machine& machine);

    Terminal& terminal_;
    Keyboard& keyboard_;
    const Options& options_;
    // Picture adjustments, changed live with [ ] < > and kept for the next
    // game (start values: --contrast / --brightness).
    double contrast_;
    double brightness_;
    std::unique_ptr<ColorMapper> correctedColors_;  // CGB color correction on
    std::unique_ptr<ColorMapper> plainColors_;      // no correction (GB, GBA,
                                                    // or --no-color-correction)
};

// Runs a ROM without a terminal for a fixed number of frames (automated
// tests and benchmarks); prints link-port output, can save a screenshot.
// Options used: path, frames, inputScript (--input), screenshot (.png or
// .txt), trueColor and colorCorrection (for .txt screenshots).
class HeadlessRunner {
public:
    explicit HeadlessRunner(const Options& options) : options_(options) {}
    // Returns the process exit code: 0 on success, 1 if the screenshot could
    // not be written. Throws on ROM loading errors.
    int run();

private:
    const Options& options_;
};

}  // namespace term

#endif
