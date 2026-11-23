#include "Machine.h"

namespace gb {

const char* systemTag(System system) {
    switch (system) {
        case System::GameBoy: return "GB";
        case System::GameBoyDual: return "GB/GBC";
        case System::GameBoyColor: return "GBC";
        default: return "GBA";
    }
}

const char* systemName(System system) {
    switch (system) {
        case System::GameBoy: return "Game Boy";
        case System::GameBoyDual: return "Game Boy (Color enhanced)";
        case System::GameBoyColor: return "Game Boy Color";
        default: return "Game Boy Advance";
    }
}

}  // namespace gb
