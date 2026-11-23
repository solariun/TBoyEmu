#ifndef TBOYEMU_CORE_SERIAL_H
#define TBOYEMU_CORE_SERIAL_H

#include <string>

#include "InterruptController.h"
#include "Types.h"

namespace gb {

// Link port (SB/SC).  No cable is attached: transfers driven by the
// internal clock complete after the proper delay and shift in 0xFF.
// Outgoing bytes are recorded so test ROMs (which print through the link
// port) can be checked in headless mode.
class Serial : private NonCopyable {
public:
    explicit Serial(InterruptController& ic);

    void tick(int cycles);
    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);

    const std::string& output() const { return output_; }

private:
    InterruptController& ic_;
    u8 sb_;
    u8 sc_;
    int remaining_;  // cycles until the running transfer completes
    std::string output_;
};

}  // namespace gb

#endif
