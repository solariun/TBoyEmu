#ifndef TBOYEMU_GBA_BACKUP_H
#define TBOYEMU_GBA_BACKUP_H

#include <string>
#include <vector>

#include "Common.h"

namespace gba {

// Cartridge save memory.  The chip type is detected from the library ID
// string the Nintendo SDK leaves in the ROM ("EEPROM_V", "FLASH1M_V", ...).
class Backup : private NonCopyable {
public:
    enum class Type { None, Sram, Flash64, Flash128, Eeprom };

    virtual ~Backup() {}
    static std::unique_ptr<Backup> create(const std::vector<u8>& rom);

    virtual Type type() const = 0;
    virtual const char* name() const = 0;

    // 0x0E000000 region (SRAM / Flash); 8-bit bus.
    virtual u8 read8(u32 /*addr*/) { return 0xFF; }
    virtual void write8(u32 /*addr*/, u8 /*value*/) {}
    // 0x0D000000 region (EEPROM); serial, one bit per 16-bit access.
    virtual u16 readSerial() { return 1; }
    virtual void writeSerial(u16 /*value*/) {}
    // DMA transfer length tells the EEPROM its address width.
    virtual void hintDmaLength(int /*units*/) {}

    std::vector<u8>& data() { return data_; }
    const std::vector<u8>& data() const { return data_; }
    bool load(const std::string& path);
    bool save(const std::string& path) const;

protected:
    std::vector<u8> data_;
};

class NoBackup : public Backup {
public:
    Type type() const { return Type::None; }
    const char* name() const { return "no save"; }
};

class Sram : public Backup {
public:
    Sram() { data_.assign(32 * 1024, 0xFF); }
    Type type() const { return Type::Sram; }
    const char* name() const { return "SRAM"; }
    u8 read8(u32 addr) { return data_[addr & 0x7FFF]; }
    void write8(u32 addr, u8 value) { data_[addr & 0x7FFF] = value; }
};

// Flash with the standard JEDEC command protocol, 64 KiB or 128 KiB (two
// banks).
class Flash : public Backup {
public:
    explicit Flash(bool large);
    Type type() const { return large_ ? Type::Flash128 : Type::Flash64; }
    const char* name() const { return large_ ? "FLASH 128K" : "FLASH 64K"; }
    u8 read8(u32 addr);
    void write8(u32 addr, u8 value);

private:
    enum State { Ready, Cmd1, Cmd2 };
    bool large_;
    State state_;
    bool idMode_;
    bool eraseArmed_;
    bool writeArmed_;
    bool bankArmed_;
    unsigned bank_;
};

// Serial EEPROM, 512 bytes (6-bit address) or 8 KiB (14-bit address).
class Eeprom : public Backup {
public:
    Eeprom();
    Type type() const { return Type::Eeprom; }
    const char* name() const { return data_.size() > 512 ? "EEPROM 8K" : "EEPROM 512"; }
    u16 readSerial();
    void writeSerial(u16 value);
    void hintDmaLength(int units);

private:
    int addressBits() const { return data_.size() > 512 ? 14 : 6; }
    void setLarge(bool large);

    bool sizeKnown_;
    std::vector<int> bits_;  // incoming command bits
    u32 readAddress_;
    int readPosition_;  // -1 when not reading
};

}  // namespace gba

#endif
