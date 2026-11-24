# TBoyEmu

A Game Boy, Game Boy Color and Game Boy Advance emulator written in C++11
that runs in a 256-color terminal at full speed (59.73 fps). There's no
audio; it handles video and keyboard input only.

## Build & run

The terminal graphics come from the [Graphicx](https://github.com/solariun/Graphicx)
library, included as a git submodule in `external/Graphicx`. Clone with
`--recursive` so you get it too:

```bash
git clone --recursive https://github.com/solariun/TBoyEmu.git
cd TBoyEmu
make
./tboyemu                     # list every game under roms/
./tboyemu "roms/Nintendo - Game Boy Advance/Metroid Fusion (USA).zip"
```

If you already cloned without `--recursive`, run
`git submodule update --init`. `make` tells you when the submodule is
missing. To pick up a newer Graphicx, run
`git submodule update --remote external/Graphicx`.

**Games** go in `roms/`, in any subfolders you like. The start screen scans
all of them and tags each game with its system, read from the ROM header:

| Tag      | Meaning |
|----------|---------|
| `GB`     | original Game Boy |
| `GB/GBC` | Game Boy game with Game Boy Color enhancements |
| `GBC`    | Game Boy Color only |
| `GBA`    | Game Boy Advance |

Type in the search box to filter by name, or by tag (`gba`, `gbc`, ...).

Requirements: a C++11 compiler and zlib (the zlib development package on Linux, e.g. `zlib1g-dev`). It builds on macOS and Linux with the same `make`. Linux needs no X11 headers, because libX11 is loaded at runtime only if it's there.
ROMs can be `.gb`, `.gbc`, `.gba` or `.zip` files.

**Terminal size:** each cell shows 2 pixels (`▀` with foreground and
background colors), so full size needs **160×72** cells for Game Boy games
and **240×80** for GBA games (one more row for the status bar). If the terminal is smaller, the picture is scaled down. Pass
`--scale 2` to let it grow on very large terminals.

## Display styles

| Option          | Look |
|-----------------|------|
| *(default)*     | `▀` half blocks with foreground and background colors: 2 pixels per cell, full color |
| `--ascii`       | plain ASCII art: one character per cell, chosen by brightness (` .:-=+*#%@`), no colors and no background, so your terminal's own colors are used |
| `--ascii-color` | the same ASCII characters, each in the color of its pixels (foreground only, still no background) |

In the ROM list, **Tab** switches the style for the next game:
FULL → ASCII-COLOR → ASCII-BW (shown at the top right). The options above
choose the starting style.

All three use the same layout (one cell = 1×2 pixels), scaling and frame
rate. `--truecolor` also applies to `--ascii-color`. The ASCII brightness
ramp assumes a dark terminal background, so bright areas get the dense
characters.

## Controls

| Key            | Button   |
|----------------|----------|
| Arrows         | D-pad    |
| X              | A        |
| Z              | B        |
| C              | Select   |
| Space / Enter  | Start    |
| A              | L (GBA)  |
| D              | R (GBA)  |
| P              | Pause    |
| S              | Save the screen as text (see below) |
| R              | Start / stop recording a movie (see below) |
| [ / ]          | Contrast −5% / +5% |
| < / >          | Brightness −5% / +5% (`,` / `.` work too) |
| Esc            | Back to the ROM list (Esc again to quit) |

In the ROM browser, type to search, use the arrows/PgUp/PgDn to move, and
press Enter to play.

**Holding keys** (a long press of X for a higher jump, or X to accelerate
while steering with the arrows): most terminals send only key *presses*, never releases, and
they auto-repeat only the last key you pressed. TBoyEmu picks the best key
source it can find. The status bar shows which one is active.

| Status bar `keys:`    | Source | Notes |
|-----------------------|--------|-------|
| `exact (kitty)`       | kitty keyboard protocol (kitty, Ghostty, WezTerm, foot, recent iTerm2) | presses and releases reported by the terminal |
| `exact (macOS)`       | macOS keyboard state (CoreGraphics) | Terminal.app, iTerm2, etc. May need permission (see below) |
| `exact (evdev)`       | Linux `/dev/input/event*` | works on the console, X11 and Wayland. Needs read access: `sudo usermod -aG input $USER`, then log in again |
| `exact (X11)`         | Linux X11 `XQueryKeymap` (libX11 loaded at runtime) | X11 sessions, no permission needed |
| `timed (repeat 500/83 ms)` | key presses only | uses your keyboard's auto-repeat timing (read from the OS): a held key stays pressed, released about 2 repeat intervals after you let go. A quick tap counts as a press as long as the repeat delay. Only one key can be held at a time |

The OS sources show `probing` until your first key press confirms they can
see the keyboard. If they can't (no permission, or a Wayland terminal using
the X11 source), TBoyEmu switches to `timed` and the status bar tells you
how to fix it. For games where the length of a press matters (jump height
in Mega Man, Mario, ...), use an `exact` source.

- **macOS:** when a game starts, macOS asks to allow your terminal app
  under *System Settings → Privacy & Security → Input Monitoring*. Allow it,
  then quit and reopen the terminal app. The status bar then shows
  `keys: exact (macOS)`.
- **Ignored keys:** OS key state is ignored while the terminal window isn't
  focused (it uses focus reporting), and also over SSH.
- **Options:** `--no-native` and `--no-kitty` turn a source off.
  `--hold-ms` and `--repeat-hold-ms` tune timed mode.

**Screen capture (S):** saves the current frame to
`<rom name>-YYYYMMDD-HHMMSS.txt` in the current directory, in the active
display style. With `--ascii` it is a plain text file with no escape codes. The file holds
the escaped terminal content: 72 lines of 160 `▀` cells (80 lines of 240 for
GBA games) with ANSI color codes (xterm-256, or 24-bit with `--truecolor`). It's always full size,
whatever the size of your terminal window. View it again with:

```bash
cat "Tetris DX (World) (SGB Enhanced) (GB Compatible)-20261123-194313.txt"
```

**Movies (R):** press R in a game to start recording and R again to stop.
While it records, a blue `[RECORDING]` tag sits at the bottom of the
screen. Every emulated frame goes to `<rom name>-YYYYMMDD-HHMMSS-movie.txt`
in the current directory, as terminal escape sequences in the active
display style. Only the cells that changed are stored, so a minute of
ASCII is a few hundred KB, and a minute of full-color GBA can reach tens
of MB. The movie is always recorded at native size, independent of your
window. Play it back at the original 59.7 fps with:

```bash
./tboyemu --play "Tetris DX (World) (SGB Enhanced) (GB Compatible)-20261124-115153-movie.txt"
```

`cat` also works, but shows it as fast as the terminal can draw. Each frame
is wrapped in a synchronized-update block (`CSI ?2026h … CSI ?2026l`),
which is how the player finds the frames.

Battery saves go next to the ROM as `<rom>.sav`. They're written on exit and
every 30 s. Game Boy saves use the VBA/BGB format with the RTC trailer. GBA
saves are raw SRAM, Flash or EEPROM dumps, the same format other emulators
use.

## Architecture

Both consoles implement one interface, `gb::Machine` (run a frame, set
buttons, get the frame, save). `MachineFactory` reads the ROM header and
creates the right one. Each piece of hardware is its own class, owned by
the console class and wired to the others by reference.

Game Boy / Game Boy Color (`src/core`):

```
GameBoy
 ├─ Cartridge ── Mbc (RomOnly, Mbc1, Mbc2, Mbc3+RTC, Mbc5)
 ├─ InterruptController   IF / IE
 ├─ Timer                 DIV/TIMA with falling-edge detector
 ├─ Joypad                P1 matrix
 ├─ Serial                link port (no cable)
 ├─ Apu                   register stub (no sound)
 ├─ Ppu                   LCD timing, STAT, VRAM/OAM, CGB palettes, renderer
 ├─ Bus                   memory map, WRAM banks, KEY1 speed switch
 │   ├─ OamDma
 │   └─ Hdma              CGB general / H-Blank VRAM DMA
 └─ Cpu                   SM83, M-cycle timed (every access ticks the Bus)
```

Game Boy Advance (`src/gba`):

```
Gba
 ├─ Backup                SRAM, Flash 64K/128K, EEPROM 512B/8K (detected from the ROM)
 ├─ Gpio                  cartridge real time clock (Pokemon R/S/E, Boktai)
 ├─ Interrupts            IE / IF / IME
 ├─ Ppu                   modes 0-5, text/affine BGs, sprites, windows, blending, mosaic
 ├─ Timers / Keypad       4 cascading timers, KEYINPUT/KEYCNT
 ├─ Bus                   memory map with wait states, I/O registers
 │   └─ Dma               4 channels: immediate, H-Blank, V-Blank
 ├─ Cpu                   ARM7TDMI: ARM + Thumb, banked modes, IRQs
 └─ Bios                  high-level emulation of the BIOS calls (no BIOS file needed)
```

`src/frontend` contains the terminal layer: `Terminal` (raw mode, RAII
restore), `Keyboard`/`KeyDecoder` (VT and kitty sequences), `NativeKeyboard`
(the OS key-state interface, with macOS, evdev and X11 backends), `GameInput`
(keys → buttons), `ColorMapper` (RGB555 frame → `gfx::Image`, with the CGB
LCD color correction), `TerminalRenderer` (window layout, status bar and
indicator around a `gfx::Screen`), `AnsiArt` (S captures via
`gfx::render()`), `Recorder` (movies via a `gfx::Screen`, plus `--play`),
`RomBrowser` and `EmulatorSession`. `EmulatorSession` runs the frame-paced
loop and skips drawing rather than slowing down. `src/util` has the zip/ROM
loader and a PNG writer.

**Graphicx** (`external/Graphicx`, a separate repository) does the actual
picture drawing:
- **Styles:** half blocks, colored ASCII and plain ASCII.
- **Colors:** xterm-256 matching or 24-bit truecolor.
- **Resizing:** Nearest for pixel art.
- **Diffing:** frame-to-frame diffs, so only changed cells are sent.

TBoyEmu compiles only its core file (`src/graphicx.cpp`). Changes to how
pictures are drawn belong in Graphicx, so its tools (`gfx-convert`,
`gfx-play`) and other users benefit too.

## Testing

```bash
./tboyemu --headless --frames 3600 cpu_instrs.gb          # prints serial output
./tboyemu --headless --frames 600 --input 300:start:5 --screenshot shot.png game.zip
./tboyemu --headless --frames 600 --screenshot shot.txt game.zip   # ANSI text
```

Results:

- Blargg `cpu_instrs`: all 11 tests pass.
- Blargg `instr_timing`: passes.
- `cgb-acid2` and `dmg-acid2`: render correctly.
- jsmolka `gba-tests`: arm, thumb, memory, bios, sram, flash64 and flash128
  all pass.
- Headless speed: roughly 1500–3000 fps for Game Boy games, and 6–18× real
  time for GBA games.

## Not supported

- Sound.
- Link cable.
- MBC7 (Kirby Tilt 'n' Tumble, accelerometer), HuC3, MBC6 and the Pocket
  Camera.
- GBA: sound, link cable, the solar sensor (Boktai) and tilt or gyro
  cartridges. Sprite timing limits and prefetch timing are approximated.
- Pixel-FIFO-exact mid-scanline effects (the renderer draws one scanline at
  a time).

## License

TBoyEmu is released under the [MIT License](LICENSE). Graphicx, the
graphics library in `external/Graphicx`, is MIT too. ROMs aren't part of
this project: use only games you're allowed to use.
