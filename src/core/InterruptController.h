#ifndef TBOYEMU_CORE_INTERRUPTCONTROLLER_H
#define TBOYEMU_CORE_INTERRUPTCONTROLLER_H

#include "Types.h"

namespace gb {

// IF (0xFF0F) and IE (0xFFFF): every component that raises interrupts
// talks to this object instead of poking at memory.
class InterruptController : private NonCopyable {
public:
    InterruptController() : if_(0xE1), ie_(0) {}

    void request(u8 mask) { if_ |= (mask & 0x1F); }
    void acknowledge(u8 mask) { if_ &= static_cast<u8>(~mask); }
    u8 pending() const { return if_ & ie_ & 0x1F; }

    u8 readIF() const { return static_cast<u8>(if_ | 0xE0); }
    void writeIF(u8 v) { if_ = v & 0x1F; }
    u8 readIE() const { return ie_; }
    void writeIE(u8 v) { ie_ = v; }

private:
    u8 if_;
    u8 ie_;
};

}  // namespace gb

#endif
