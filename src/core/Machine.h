// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Machine.h
//  Layer:  Game Boy core (shared with the Game Boy Advance core and the frontend)
//
//  What this file does
//  -------------------
//  Declares gb::Machine, the abstract interface every emulated console
//  implements (run one frame, set a button, get the frame, load/write the
//  battery save), and the System enum that says which console a ROM targets,
//  with helpers that turn it into the short tag ("GB", "GB/GBC", "GBC",
//  "GBA") and long name shown by the ROM browser and status bar.
//
//  How it fits in
//  --------------
//  gb::GameBoy (src/core/GameBoy.h) and the Game Boy Advance class in src/gba
//  both derive from Machine. MachineFactory (src/MachineFactory.cpp) reads
//  the ROM header and creates the right one; the frontend (EmulatorSession,
//  RomBrowser) only talks to this interface, so it never needs to know which
//  console it is driving. Implementation of the helpers: Machine.cpp.
//
//  Hardware notes
//  --------------
//  Both the Game Boy and the GBA refresh at about 59.73 Hz, so the frontend
//  can pace every machine with the same frame period. The Game Boy Color
//  compatibility levels come from the CGB flag at ROM header 0x0143 (Pan
//  Docs, "The Cartridge Header"): 0x80 = works on both, 0xC0 = CGB only.
//
//  Contributing
//  ------------
//  Adding a method here means implementing it in both cores. Keep the
//  interface small and frame-oriented. The frontend calls everything from a
//  single thread. Test with:
//      make && ./tboyemu --headless --frames 3600 cpu_instrs.gb
//  (serialOutput() is what --headless prints) and with a GBA ROM.
// ============================================================================
#ifndef TBOYEMU_CORE_MACHINE_H
#define TBOYEMU_CORE_MACHINE_H

#include <string>

#include "FrameBuffer.h"
#include "Types.h"

namespace gb {

// Which console a ROM is made for.
// Decided from the ROM header (GBA ROMs are recognized by MachineFactory;
// for Game Boy ROMs see GameBoy::system(), which reads the CGB flag).
enum class System {
    GameBoy,       // original Game Boy only
    GameBoyDual,   // Game Boy game with Game Boy Color enhancements
    GameBoyColor,  // Game Boy Color only
    GameBoyAdvance
};

// Short tag shown in the ROM list and searchable from the search box.
const char* systemTag(System system);   // "GB", "GB/GBC", "GBC", "GBA"
// Human readable console name.
const char* systemName(System system);  // "Game Boy", ...

// What the frontend sees of an emulated console, whichever it is.
// Owned by the frontend through a std::unique_ptr<Machine> created by
// MachineFactory; lives for as long as one game is being played. Not
// copyable (hardware components reference each other). Single-threaded.
class Machine : private NonCopyable {
public:
    virtual ~Machine() {}

    // Emulates until the next complete video frame is available in frame().
    virtual void runFrame() = 0;  // one video frame (59.73 Hz on every model)
    // Presses (pressed = true) or releases a button. Buttons a console does
    // not have (L/R on the Game Boy) are ignored.
    virtual void setButton(Button button, bool pressed) = 0;
    // The last completed frame (160x144 for Game Boy, 240x160 for GBA).
    // The reference stays valid for the lifetime of the Machine, but the
    // contents change on the next runFrame().
    virtual const FrameBuffer& frame() const = 0;

    // Which console this is (see System).
    virtual System system() const = 0;
    // true if the frame holds real colors (CGB / GBA) that should get LCD
    // color correction in the frontend.
    virtual bool colorScreen() const = 0;  // false: DMG palette, no LCD correction
    // Game title read from the ROM header.
    virtual std::string title() const = 0;
    // Short description of the cartridge hardware, e.g. "MBC3+RTC" or the
    // GBA save chip type; shown in the status bar.
    virtual std::string hardware() const = 0;  // mapper or save chip

    // Battery save persistence (.sav next to the ROM). Both return false when
    // the cartridge has no battery-backed memory or the file cannot be
    // read/written; they never throw for I/O errors.
    virtual bool loadSave(const std::string& path) = 0;
    virtual bool writeSave(const std::string& path) const = 0;

    // Bytes sent through the link port (used by test ROMs).
    // Blargg's test ROMs print their results through the serial port, so
    // --headless mode prints this string. Consoles without it return "".
    virtual std::string serialOutput() const { return std::string(); }
};

}  // namespace gb

#endif
