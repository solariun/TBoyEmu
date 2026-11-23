#ifndef TBOYEMU_CORE_MACHINE_H
#define TBOYEMU_CORE_MACHINE_H

#include <string>

#include "FrameBuffer.h"
#include "Types.h"

namespace gb {

// Which console a ROM is made for.
enum class System {
    GameBoy,       // original Game Boy only
    GameBoyDual,   // Game Boy game with Game Boy Color enhancements
    GameBoyColor,  // Game Boy Color only
    GameBoyAdvance
};

const char* systemTag(System system);   // "GB", "GB/GBC", "GBC", "GBA"
const char* systemName(System system);  // "Game Boy", ...

// What the frontend sees of an emulated console, whichever it is.
class Machine : private NonCopyable {
public:
    virtual ~Machine() {}

    virtual void runFrame() = 0;  // one video frame (59.73 Hz on every model)
    virtual void setButton(Button button, bool pressed) = 0;
    virtual const FrameBuffer& frame() const = 0;

    virtual System system() const = 0;
    virtual bool colorScreen() const = 0;  // false: DMG palette, no LCD correction
    virtual std::string title() const = 0;
    virtual std::string hardware() const = 0;  // mapper or save chip

    virtual bool loadSave(const std::string& path) = 0;
    virtual bool writeSave(const std::string& path) const = 0;

    // Bytes sent through the link port (used by test ROMs).
    virtual std::string serialOutput() const { return std::string(); }
};

}  // namespace gb

#endif
