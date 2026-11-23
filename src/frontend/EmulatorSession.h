#ifndef TBOYEMU_FRONTEND_EMULATORSESSION_H
#define TBOYEMU_FRONTEND_EMULATORSESSION_H

#include <memory>
#include <string>

#include "ColorMapper.h"
#include "Keyboard.h"
#include "Options.h"
#include "Terminal.h"

namespace term {

// Plays one ROM in the terminal at the original speed (59.73 fps): runs
// the emulated frame, draws it, and sleeps until the next frame deadline.
// When drawing falls behind, frames are emulated but not drawn, so game
// speed never drops.
class EmulatorSession {
public:
    EmulatorSession(Terminal& terminal, Keyboard& keyboard, const Options& options);

    // Returns when the user presses Esc.  Throws on ROM loading errors.
    void run(const std::string& romPath);

private:
    const ColorMapper& mapperFor(bool cgbGame);

    Terminal& terminal_;
    Keyboard& keyboard_;
    const Options& options_;
    std::unique_ptr<ColorMapper> cgbColors_;
    std::unique_ptr<ColorMapper> dmgColors_;
};

// Runs a ROM without a terminal for a fixed number of frames (automated
// tests and benchmarks); prints link-port output, can save a screenshot.
class HeadlessRunner {
public:
    explicit HeadlessRunner(const Options& options) : options_(options) {}
    int run();

private:
    const Options& options_;
};

}  // namespace term

#endif
