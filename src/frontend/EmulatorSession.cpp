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
#include "TerminalRenderer.h"

namespace term {

namespace {

typedef std::chrono::steady_clock Clock;

// 70224 dots at 4.194304 MHz = 16.742706 ms per frame (59.7275 Hz).  The
// GBA (280896 cycles at 16.777216 MHz) has exactly the same frame rate.
const Clock::duration kFrameTime = std::chrono::duration_cast<Clock::duration>(
    std::chrono::nanoseconds(1000000000LL * gb::kDotsPerFrame / gb::kCpuClockHz));

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
std::string screenshotPath(const std::string& romPath) {
    std::string base = romPath.substr(romPath.find_last_of('/') == std::string::npos ? 0 : romPath.find_last_of('/') + 1);
    base = base.substr(0, base.find_last_of('.'));
    char stamp[32];
    std::time_t now = std::time(0);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    std::string path = base + "-" + stamp + ".txt";
    for (int n = 2; std::ifstream(path.c_str()).good(); ++n) {
        path = base + "-" + stamp + "-" + std::to_string(n) + ".txt";
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
    std::unique_ptr<ColorMapper>& slot = correct ? correctedColors_ : plainColors_;
    if (!slot) slot.reset(new ColorMapper(correct));
    return *slot;
}

void EmulatorSession::run(const std::string& romPath) {
    std::unique_ptr<gb::Machine> gameboy = emu::createMachine(romPath);
    const std::string savePath = util::RomLoader::savePathFor(romPath);

    TerminalRenderer renderer(terminal_,
                              options_.trueColor ? ColorMode::TrueColor : ColorMode::Xterm256,
                              options_.maxScale);
    renderer.setColorMapper(&mapperFor(*gameboy));
    // The OS keyboard state is only meaningful when we run on the machine
    // whose keyboard is being used (not over SSH).
    std::unique_ptr<NativeKeyboard> native;
    bool remote = std::getenv("SSH_CONNECTION") || std::getenv("SSH_TTY");
    if (!terminal_.kittyKeyboard() && options_.nativeKeyboard && !remote) native = NativeKeyboard::create();
    GameInput input(terminal_.kittyKeyboard(), std::move(native), systemKeyRepeat(), options_.holdMs,
                    options_.repeatHoldMs);

    const ColorMode colorMode = options_.trueColor ? ColorMode::TrueColor : ColorMode::Xterm256;
    std::vector<KeyEvent> events;
    bool paused = false;
    int framesSinceDraw = 0;
    int emulatedCount = 0, drawnCount = 0;
    double emuFps = 0, drawFps = 0;
    std::string flash;  // temporary status bar message
    Clock::time_point flashUntil;
    std::string status;
    auto refreshStatus = [&](Clock::time_point when) {
        status = when < flashUntil ? flash : formatStatus(*gameboy, emuFps, drawFps, input, paused);
    };
    refreshStatus(Clock::now());
    Clock::time_point statsStart = Clock::now();
    Clock::time_point lastSave = statsStart;
    Clock::time_point next = statsStart;

    for (;;) {
        if (Terminal::terminateRequested()) break;
        if (Terminal::consumeResize()) renderer.relayout();

        // ---- input ----
        events.clear();
        keyboard_.poll(events, 0);
        Clock::time_point now = Clock::now();
        for (size_t i = 0; i < events.size(); ++i) input.handle(events[i], now);
        if (input.exitRequested()) break;
        if (input.consumePauseToggle()) {
            paused = !paused;
            refreshStatus(now);
        }
        if (input.consumeScreenshotRequest()) {
            std::string path = screenshotPath(romPath);
            bool ok = AnsiArt::save(path, gameboy->frame(), mapperFor(*gameboy), colorMode);
            flash = ok ? " Screen saved to " + path + "  (view it with: cat \"" + path + "\")"
                       : " Could not write " + path;
            flashUntil = now + std::chrono::seconds(3);
            refreshStatus(now);
        }
        input.update(now);
        input.applyTo(*gameboy);

        // ---- emulate one frame ----
        if (!paused) {
            gameboy->runFrame();
            ++emulatedCount;
        }

        // ---- draw, unless we are running late ----
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
        now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - statsStart).count();
        if (elapsed >= 1.0) {
            emuFps = emulatedCount / elapsed;
            drawFps = drawnCount / elapsed;
            emulatedCount = drawnCount = 0;
            statsStart = now;
            refreshStatus(now);
        }
        if (now - lastSave > std::chrono::seconds(30)) {
            gameboy->writeSave(savePath);
            lastSave = now;
        }

        // ---- wait for the next frame deadline, staying responsive ----
        next += kFrameTime;
        if (now > next + std::chrono::milliseconds(250)) next = now;  // resync after a stall
        for (;;) {
            now = Clock::now();
            if (now >= next) break;
            long waitMs = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count());
            if (waitMs >= 2) {
                events.clear();
                keyboard_.poll(events, static_cast<int>(waitMs - 1));
                Clock::time_point t = Clock::now();
                for (size_t i = 0; i < events.size(); ++i) input.handle(events[i], t);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        }
    }

    gameboy->writeSave(savePath);
}

// ------------------------------------------------------------ headless ----

namespace {

struct ScriptedPress {
    int frame;
    gb::Button button;
    int length;
};

bool buttonFromName(const std::string& name, gb::Button& b) {
    static const char* names[] = {"right", "left", "up", "down", "a", "b", "select", "start"};
    for (int i = 0; i < gb::kButtonCount; ++i) {
        if (name == names[i]) {
            b = static_cast<gb::Button>(i);
            return true;
        }
    }
    return false;
}

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
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("%d frames in %.3f s = %.0f fps (%.1fx real time)\n", options_.frames, seconds,
                options_.frames / seconds, options_.frames / seconds / 59.7275);
    if (!gameboy->serialOutput().empty()) std::printf("serial output:\n%s\n", gameboy->serialOutput().c_str());
    if (!options_.screenshot.empty()) {
        bool asText = util::endsWith(util::toLower(options_.screenshot), ".txt");
        bool ok;
        if (asText) {
            ColorMapper colors(options_.colorCorrection && gameboy->colorScreen() &&
                               gameboy->system() != gb::System::GameBoyAdvance);
            ok = AnsiArt::save(options_.screenshot, gameboy->frame(), colors,
                               options_.trueColor ? ColorMode::TrueColor : ColorMode::Xterm256);
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
