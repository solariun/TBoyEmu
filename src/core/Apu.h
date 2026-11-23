#ifndef TBOYEMU_CORE_APU_H
#define TBOYEMU_CORE_APU_H

#include "Types.h"

namespace gb {

// Sound is not produced (terminal build), but games read back and poll the
// sound registers, so they are kept with the correct read masks.
class Apu : private NonCopyable {
public:
    Apu();

    void reset();
    u8 read(u16 addr) const;
    void write(u16 addr, u8 value);

private:
    u8 regs_[0x30];  // 0xFF10 - 0xFF3F
    bool powered_;
};

}  // namespace gb

#endif
