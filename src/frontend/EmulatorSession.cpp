// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/frontend/EmulatorSession.cpp
//  Layer:  Terminal frontend
//
//  What this file does
//  -------------------
//  Implements EmulatorSession::run(), the real-time game loop, and
//  HeadlessRunner::run(), the terminal-less test/benchmark runner (see
//  EmulatorSession.h). Also the status bar text and the screen capture file
//  name.
//
//  How it fits in
//  --------------
//  run() wires everything together for one game:
//    emu::createMachine()   loads the ROM, picks Game Boy or GBA core
//    TerminalRenderer       draws frames (with the right ColorMapper)
//    NativeKeyboard         OS key state, only if no kitty protocol / no SSH
//    GameInput              keys -> buttons
//    AnsiArt                S key: save the screen as a .txt file
//  and calls Machine::writeSave() to keep the battery save up to date.
//
//  Terminal concepts: the frame loop
//  ---------------------------------
//  One iteration = one emulated frame:
//    1. input:    poll the keyboard without waiting, handle hotkeys,
//                 GameInput::update(), push buttons into the machine
//    2. emulate:  Machine::runFrame() (skipped while paused)
//    3. draw:     TerminalRenderer::present(), unless we are late
//    4. stats:    once a second recompute emulated / drawn fps; every 30 s
//                 write the battery save
//    5. wait:     until the next deadline (next += kFrameTime), reading
//                 the keyboard while waiting
//  Late = the current time is already past the *following* frame's
//  deadline. Then the frame is not drawn (frame skip), but never more than
//  6 in a row, so the picture still updates at least ~8.5 times a second.
//  After a long stall (> 250 ms, e.g. the process was suspended) the
//  deadline is reset to "now" instead of trying to catch up with a burst
//  of frames.
//
//  Contributing
//  ------------
//  * The deadline must advance by exactly kFrameTime per iteration;
//    computing it from "now" would accumulate drift and slow the game down.
//  * Test: play a game and watch "fps (drawn ...)" in the status bar; it
//    should show ~59.7 emulated fps even if fewer are drawn. Headless:
//    ./tboyemu --headless --frames 3600 cpu_instrs.gb
// ============================================================================
#include "EmulatorSession.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "../MachineFactory.h"
#include "../util/PngWriter.h"
#include "../util/RomLoader.h"
#include "AnsiArt.h"
#include "GameInput.h"
#include "Recorder.h"
#include "TerminalRenderer.h"

namespace term {

namespace {

// Monotonic clock: unaffected by changes of the wall-clock time.
typedef std::chrono::steady_clock Clock;

// 70224 dots at 4.194304 MHz = 16.742706 ms per frame (59.7275 Hz).  The
// GBA (280896 cycles at 16.777216 MHz) has exactly the same frame rate.
// (Computed in integer nanoseconds: 1e9 * 70224 / 4194304 = 16742706 ns.)
const Clock::duration kFrameTime = std::chrono::duration_cast<Clock::duration>(
    std::chrono::nanoseconds(1000000000LL * gb::kDotsPerFrame / gb::kCpuClockHz));

// Builds the status bar line: title, system tag and hardware model,
// emulated fps and drawn fps, the key source (GameInput::description()),
// the controls, and "** PAUSED **". If GameInput has a notice (OS key
// access missing) that is shown instead, since it needs the user's action.
std::string formatStatus(const gb::Machine& machine, double emuFps, double drawFps,
                         const GameInput& input, bool paused) {
    if (!input.notice().empty()) return " " + input.notice() + "  (or use a kitty-protocol terminal)";
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  " %s [%s %s] | %5.1f fps (drawn %4.1f) | keys: %s | arrows X=A Z=B C=Sel Space=Start%s "
                  "P=pause S=save screen Esc=menu %s",
                  machine.title().c_str(), gb::systemTag(machine.system()), machine.hardware().c_str(), emuFps,
                  drawFps, input.description().c_str(),
                  machine.system() == gb::System::GameBoyAdvance ? " A=L D=R" : "", paused ? " ** PAUSED **" : "");
    return buf;
}

// "<rom name>-YYYYMMDD-HHMMSS.txt" in the current directory, never
// overwriting an existing file.
// The ROM's directory and extension are stripped; if the name is taken
// (two captures in the same second), "-2", "-3", ... is appended.
std::string screenshotPath(const std::string& romPath, const std::string& suffix = "") {
    std::string base = romPath.substr(romPath.find_last_of('/') == std::string::npos ? 0 : romPath.find_last_of('/') + 1);
    base = base.substr(0, base.find_last_of('.'));
    char stamp[32];
    std::time_t now = std::time(0);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    std::string path = base + "-" + stamp + suffix + ".txt";
    for (int n = 2; std::ifstream(path.c_str()).good(); ++n) {
        path = base + "-" + stamp + "-" + std::to_string(n) + suffix + ".txt";
    }
    return path;
}

}  // namespace

EmulatorSession::EmulatorSession(Terminal& terminal, Keyboard& keyboard, const Options& options)
    : terminal_(terminal), keyboard_(keyboard), options_(options) {}

const ColorMapper& EmulatorSession::mapperFor(const gb::Machine& machine) {
    // The LCD correction curve is for the Game Boy Color screen.  Original
    // Game Boy games use a hand-picked palette and GBA colors are shown as is.
    bool correct = options_.colorCorrection && machine.colorScreen() &&
                   machine.system() != gb::System::GameBoyAdvance;
    // Two cached slots; each table takes a noticeable moment to build, so it
    // is built only the first time it is needed and reused by later games.
    std::unique_ptr<ColorMapper>& slot = correct ? correctedColors_ : plainColors_;
    if (!slot) slot.reset(new ColorMapper(correct));
    return *slot;
}

void EmulatorSession::run(const std::string& romPath) {
    // Load the ROM (and its .sav, if any) into the right console. Throws on
    // unreadable / unsupported files; main.cpp shows the message.
    std::unique_ptr<gb::Machine> gameboy = emu::createMachine(romPath);
    const std::string savePath = util::RomLoader::savePathFor(romPath);

    TerminalRenderer renderer(terminal_,
                              options_.trueColor ? ColorMode::TrueColor : ColorMode::Xterm256,
                              options_.maxScale, options_.style);
    renderer.setColorMapper(&mapperFor(*gameboy));
    Recorder recorder;  // R key: movie recording (see Recorder.h)
    // The OS keyboard state is only meaningful when we run on the machine
    // whose keyboard is being used (not over SSH).
    // It is also unnecessary when the kitty protocol already gives exact
    // key releases, and can be disabled with --no-native.
    std::unique_ptr<NativeKeyboard> native;
    bool remote = std::getenv("SSH_CONNECTION") || std::getenv("SSH_TTY");
    if (!terminal_.kittyKeyboard() && options_.nativeKeyboard && !remote) native = NativeKeyboard::create();
    GameInput input(terminal_.kittyKeyboard(), std::move(native), systemKeyRepeat(), options_.holdMs,
                    options_.repeatHoldMs);

    // Loop state.
    const ColorMode colorMode = options_.trueColor ? ColorMode::TrueColor : ColorMode::Xterm256;
    std::vector<KeyEvent> events;   // reused buffer for Keyboard::poll()
    bool paused = false;
    int framesSinceDraw = 0;        // consecutive skipped frames (max 6)
    int emulatedCount = 0, drawnCount = 0;  // frames in the current 1 s window
    double emuFps = 0, drawFps = 0;         // last measured rates
    std::string flash;  // temporary status bar message
    Clock::time_point flashUntil;
    std::string status;
    // Recomputes the status text; shows `flash` (e.g. "Screen saved to ...")
    // until flashUntil, then the normal status line again.
    auto refreshStatus = [&](Clock::time_point when) {
        status = when < flashUntil ? flash : formatStatus(*gameboy, emuFps, drawFps, input, paused);
    };
    refreshStatus(Clock::now());
    Clock::time_point statsStart = Clock::now();
    Clock::time_point lastSave = statsStart;
    Clock::time_point next = statsStart;  // deadline at which this frame should start

    for (;;) {
        // Signals: quit on SIGTERM/SIGHUP/SIGINT, re-layout on SIGWINCH.
        if (Terminal::terminateRequested()) break;
        if (Terminal::consumeResize()) renderer.relayout();

        // ---- input ----
        // Non-blocking poll (timeout 0): whatever arrived since the wait loop.
        events.clear();
        keyboard_.poll(events, 0);
        Clock::time_point now = Clock::now();
        for (size_t i = 0; i < events.size(); ++i) input.handle(events[i], now);
        if (input.exitRequested()) break;
        if (input.consumePauseToggle()) {
            paused = !paused;
            refreshStatus(now);
        }
        if (input.consumeRecordToggle()) {
            // R: start or stop recording every frame into a .txt movie.
            if (!recorder.active()) {
                std::string path = screenshotPath(romPath, "-movie");
                if (recorder.start(path, colorMode, options_.style, &mapperFor(*gameboy))) {
                    flash = " Recording to " + path + "  (R stops)";
                } else {
                    flash = " Could not create " + path;
                }
            } else {
                recorder.stop();
                char seconds[32];
                std::snprintf(seconds, sizeof(seconds), "%.1f", recorder.frames() / 59.7275);
                flash = " Movie saved to " + recorder.path() + " (" + seconds + " s)  play it with: ./tboyemu --play \"" +
                        recorder.path() + "\"";
            }
            flashUntil = now + std::chrono::seconds(4);
            renderer.setIndicator(recorder.active() ? "[RECORDING]" : "");
            refreshStatus(now);
        }
        if (input.consumeScreenshotRequest()) {
            // Save the current frame as escaped text and say so for 3 s.
            std::string path = screenshotPath(romPath);
            bool ok = AnsiArt::save(path, gameboy->frame(), mapperFor(*gameboy), colorMode, options_.style);
            flash = ok ? " Screen saved to " + path + "  (view it with: cat \"" + path + "\")"
                       : " Could not write " + path;
            flashUntil = now + std::chrono::seconds(3);
            refreshStatus(now);
        }
        input.update(now);
        input.applyTo(*gameboy);

        // ---- emulate one frame ----
        // While paused the last frame keeps being shown (and redrawn, which
        // costs almost nothing thanks to the renderer's diff).
        if (!paused) {
            gameboy->runFrame();
            ++emulatedCount;
            if (recorder.active()) recorder.addFrame(gameboy->frame());  // every emulated frame
        }

        // ---- draw, unless we are running late ----
        // Late = already past the deadline of the *next* frame. Skipping the
        // draw lets emulation catch up; every 7th frame is drawn regardless
        // so the picture never freezes.
        now = Clock::now();
        bool late = now > next + kFrameTime;
        if (!late || framesSinceDraw >= 6) {
            renderer.present(gameboy->frame(), status);
            framesSinceDraw = 0;
            ++drawnCount;
        } else {
            ++framesSinceDraw;
        }

        // ---- statistics / periodic battery save ----
        // fps = frames counted over (at least) the last second.
        now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - statsStart).count();
        if (elapsed >= 1.0) {
            emuFps = emulatedCount / elapsed;
            drawFps = drawnCount / elapsed;
            emulatedCount = drawnCount = 0;
            statsStart = now;
            refreshStatus(now);
        }
        // Battery-backed RAM is flushed to <rom>.sav every 30 s, so a crash
        // or killed terminal loses at most 30 s of progress.
        if (now - lastSave > std::chrono::seconds(30)) {
            gameboy->writeSave(savePath);
            lastSave = now;
        }

        // ---- wait for the next frame deadline, staying responsive ----
        // Fixed-step pacing: the deadline advances by exactly one frame
        // (59.7275 Hz), independent of how long this iteration took.
        next += kFrameTime;
        if (now > next + std::chrono::milliseconds(250)) next = now;  // resync after a stall
        for (;;) {
            now = Clock::now();
            if (now >= next) break;
            long waitMs = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count());
            if (waitMs >= 2) {
                // Sleep inside poll() (1 ms short of the deadline, since
                // poll's timeout can overshoot) and handle keys as they
                // arrive, so their timestamps are precise.
                events.clear();
                keyboard_.poll(events, static_cast<int>(waitMs - 1));
                Clock::time_point t = Clock::now();
                for (size_t i = 0; i < events.size(); ++i) input.handle(events[i], t);
            } else {
                // Less than 2 ms left: short sleeps for better precision.
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        }
    }

    // Leaving the game (Esc or signal): finish a running movie and do the
    // final battery save.
    recorder.stop();
    gameboy->writeSave(savePath);
}

// ------------------------------------------------------------ headless ----

namespace {

// One entry of --input "frame:button:length,...": hold `button` from frame
// `frame` for `length` frames (default 5).
struct ScriptedPress {
    int frame;
    gb::Button button;
    int length;
};

// Button name (lower case) -> gb::Button. Returns false for unknown names.
// names[] follows the gb::Button order and must have gb::kButtonCount entries
// ("l" and "r" are the GBA shoulder buttons).
bool buttonFromName(const std::string& name, gb::Button& b) {
    static const char* names[gb::kButtonCount] = {"right", "left", "up", "down", "a",
                                                  "b",     "select", "start", "l", "r"};
    for (int i = 0; i < gb::kButtonCount; ++i) {
        if (name == names[i]) {
            b = static_cast<gb::Button>(i);
            return true;
        }
    }
    return false;
}

// Parses the --input script, e.g. "60:start:5,120:a:5". Malformed items
// and unknown buttons are skipped silently.
std::vector<ScriptedPress> parseScript(const std::string& script) {
    std::vector<ScriptedPress> presses;
    std::stringstream ss(script);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t c1 = item.find(':');
        if (c1 == std::string::npos) continue;
        size_t c2 = item.find(':', c1 + 1);
        ScriptedPress p;
        p.frame = std::atoi(item.substr(0, c1).c_str());
        std::string name = util::toLower(item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1));
        p.length = c2 == std::string::npos ? 5 : std::atoi(item.substr(c2 + 1).c_str());
        if (buttonFromName(name, p.button)) presses.push_back(p);
    }
    return presses;
}

}  // namespace

int HeadlessRunner::run() {
    std::unique_ptr<gb::Machine> gameboy = emu::createMachine(options_.path);
    std::printf("title: %s  system: %s  hardware: %s\n", gameboy->title().c_str(),
                gb::systemName(gameboy->system()), gameboy->hardware().c_str());

    // Run as fast as possible (no pacing), applying the scripted presses.
    std::vector<ScriptedPress> script = parseScript(options_.inputScript);
    Clock::time_point start = Clock::now();
    for (int f = 0; f < options_.frames; ++f) {
        bool held[gb::kButtonCount] = {false};
        for (size_t i = 0; i < script.size(); ++i) {
            if (f >= script[i].frame && f < script[i].frame + script[i].length) {
                held[static_cast<int>(script[i].button)] = true;
            }
        }
        for (int b = 0; b < gb::kButtonCount; ++b) gameboy->setButton(static_cast<gb::Button>(b), held[b]);
        gameboy->runFrame();
    }
    // Speed report; 59.7275 is the real console frame rate.
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("%d frames in %.3f s = %.0f fps (%.1fx real time)\n", options_.frames, seconds,
                options_.frames / seconds, options_.frames / seconds / 59.7275);
    // Test ROMs such as Blargg's cpu_instrs print their results to the
    // serial (link) port.
    if (!gameboy->serialOutput().empty()) std::printf("serial output:\n%s\n", gameboy->serialOutput().c_str());
    if (!options_.screenshot.empty()) {
        // The file extension picks the format: .txt = ANSI art, else PNG.
        bool asText = util::endsWith(util::toLower(options_.screenshot), ".txt");
        bool ok;
        if (asText) {
            ColorMapper colors(options_.colorCorrection && gameboy->colorScreen() &&
                               gameboy->system() != gb::System::GameBoyAdvance);
            ok = AnsiArt::save(options_.screenshot, gameboy->frame(), colors,
                               options_.trueColor ? ColorMode::TrueColor : ColorMode::Xterm256, options_.style);
        } else {
            ok = util::PngWriter::write(options_.screenshot, gameboy->frame());
        }
        if (!ok) {
            std::fprintf(stderr, "could not write %s\n", options_.screenshot.c_str());
            return 1;
        }
        std::printf("screenshot: %s\n", options_.screenshot.c_str());
    }
    return 0;
}

}  // namespace term
