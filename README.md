# TBoyEmu

A Game Boy / Game Boy Color emulator written in C++11 that runs in a
256-color terminal at full speed (59.73 fps). There's no audio; it handles
video and keyboard input only.

## Build & run

```bash
make
./tboyemu                     # ROM browser for "Nintendo - Game Boy Color/"
./tboyemu "Nintendo - Game Boy Color/Tetris DX (World) (SGB Enhanced) (GB Compatible).zip"
```

Requirements: a C++11 compiler and zlib (the zlib development package on Linux, e.g. `zlib1g-dev`). It builds on macOS and Linux with the same `make`. Linux needs no X11 headers, because libX11 is loaded at runtime only if it's there.
ROMs can be `.gb`, `.gbc` or `.zip` files.

**Terminal size:** each cell shows 2 pixels (`▀` with foreground and
background colors), so full size needs **160×72** cells (73 with the status
bar). If the terminal is smaller, the picture is scaled down. Pass
`--scale 2` to let it grow on very large terminals.

## Controls

| Key            | Game Boy |
|----------------|----------|
| Arrows         | D-pad    |
| X              | A        |
| Z              | B        |
| C              | Select   |
| Space / Enter  | Start    |
| P              | Pause    |
| S              | Save the screen as text (see below) |
| Esc            | Back to the ROM list (Esc again to quit) |

In the ROM browser, type to search, use the arrows/PgUp/PgDn to move, and
press Enter to play.

**Holding several keys** (for example, X to accelerate while steering with
the arrows): most terminals send only key *presses*, never releases, and
they auto-repeat only the last key you pressed. TBoyEmu picks the best key
source it can find. The status bar shows which one is active.

| Status bar `keys:`    | Source | Notes |
|-----------------------|--------|-------|
| `exact (kitty)`       | kitty keyboard protocol (kitty, Ghostty, WezTerm, foot, recent iTerm2) | presses and releases reported by the terminal |
| `exact (macOS)`       | macOS keyboard state (CoreGraphics) | Terminal.app, iTerm2, etc. May need permission (see below) |
| `exact (evdev)`       | Linux `/dev/input/event*` | works on the console, X11 and Wayland. Needs read access: `sudo usermod -aG input $USER`, then log in again |
| `exact (X11)`         | Linux X11 `XQueryKeymap` (libX11 loaded at runtime) | X11 sessions, no permission needed |
| `timed`               | key presses only | a press holds the button for 150 ms, extended by auto-repeat. Holding two keys at once doesn't work here |

The OS sources show `probing` until your first key press confirms they can
see the keyboard. If they can't (no permission, or a Wayland terminal using
the X11 source), TBoyEmu switches to `timed` and the status bar tells you
how to fix it.

- **macOS:** the system asks to allow your terminal app under
  *System Settings → Privacy & Security → Input Monitoring*. Allow it, then
  restart the terminal app.
- **Ignored keys:** OS key state is ignored while the terminal window isn't
  focused (it uses focus reporting), and also over SSH.
- **Options:** `--no-native` and `--no-kitty` turn a source off.
  `--hold-ms` and `--repeat-hold-ms` tune timed mode.

**Screen capture (S):** saves the current frame to
`<rom name>-YYYYMMDD-HHMMSS.txt` in the current directory. The file holds
the escaped terminal content: 72 lines of 160 `▀` cells with ANSI color
codes (xterm-256, or 24-bit with `--truecolor`). It's always full size,
whatever the size of your terminal window. View it again with:

```bash
cat "Tetris DX (World) (SGB Enhanced) (GB Compatible)-20261123-194313.txt"
```

Battery saves go next to the ROM as `<rom>.sav`, in the VBA/BGB format
with the RTC trailer. They're written on exit and every 30 s.

## Architecture

Each piece of hardware is a class in `src/core`. `GameBoy` owns all of them
and wires them together by reference:

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

`src/frontend` contains the terminal layer: `Terminal` (raw mode, RAII
restore), `Keyboard`/`KeyDecoder` (VT and kitty sequences), `NativeKeyboard`
(the OS key-state interface, with macOS, evdev and X11 backends), `GameInput`
(keys → buttons), `ColorMapper` (RGB555 → xterm-256 with CGB LCD color
correction), `TerminalRenderer` (half-block, diffed, synchronized output),
`AnsiArt` (frame → escaped `.txt`), `RomBrowser` and `EmulatorSession`, which runs the frame-paced loop and skips
drawing rather than slowing down. `src/util` has the zip/ROM loader and a
PNG writer.

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
- Headless speed: roughly 1500–3000 fps (25–50× real time).

## Not supported

- Sound.
- Link cable.
- MBC7 (Kirby Tilt 'n' Tumble, accelerometer), HuC3, MBC6 and the Pocket
  Camera.
- Pixel-FIFO-exact mid-scanline effects (the renderer draws one scanline at
  a time).
