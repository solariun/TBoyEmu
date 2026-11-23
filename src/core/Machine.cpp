// ============================================================================
//  TBoyEmu - Game Boy / Game Boy Color / Game Boy Advance emulator for
//  256-color terminals, written in C++11.
//  https://github.com/solariun/TBoyEmu
// ----------------------------------------------------------------------------
//  File:   src/core/Machine.cpp
//  Layer:  Game Boy core (shared with the Game Boy Advance core and the frontend)
//
//  What this file does
//  -------------------
//  Implements the two free helper functions declared in Machine.h that turn
//  a System value into text: systemTag() (short tag for the ROM list and the
//  search box) and systemName() (full console name). The Machine interface
//  itself is pure virtual and has no implementation here.
//
//  How it fits in
//  --------------
//  Used by the frontend (RomBrowser, EmulatorSession status bar) for both
//  Game Boy and Game Boy Advance games. See Machine.h for the interface.
//
//  Hardware notes
//  --------------
//  None: this is presentation only. The System value itself is derived from
//  the ROM header (Pan Docs, "The Cartridge Header", CGB flag at 0x0143).
//
//  Contributing
//  ------------
//  The tags must stay in sync with the table in README.md and with the
//  search filter in the ROM browser (users can type "gbc", "gba", ...).
//  Build check: c++ -std=c++11 -fsyntax-only -Isrc src/core/Machine.cpp
// ============================================================================
#include "Machine.h"

namespace gb {

// Short tag: "GB", "GB/GBC", "GBC" or "GBA". Anything that is not one of the
// three Game Boy variants is reported as GBA.
const char* systemTag(System system) {
    switch (system) {
        case System::GameBoy: return "GB";
        case System::GameBoyDual: return "GB/GBC";
        case System::GameBoyColor: return "GBC";
        default: return "GBA";
    }
}

// Full, human readable console name (same mapping as systemTag()).
const char* systemName(System system) {
    switch (system) {
        case System::GameBoy: return "Game Boy";
        case System::GameBoyDual: return "Game Boy (Color enhanced)";
        case System::GameBoyColor: return "Game Boy Color";
        default: return "Game Boy Advance";
    }
}

}  // namespace gb
