#ifndef TBOYEMU_GBA_CPU_H
#define TBOYEMU_GBA_CPU_H

#include "Bus.h"
#include "Common.h"

namespace gba {

// Receives SWI (software interrupt) calls: the BIOS is emulated in C++.
class SwiHandler {
public:
    virtual ~SwiHandler() {}
    virtual void swi(u32 number) = 0;
};

// ARM7TDMI (ARMv4T) interpreter: ARM and Thumb instruction sets, banked
// registers for every processor mode, IRQ exceptions.  step() executes one
// instruction and returns the cycles it took (from the bus wait states plus
// internal cycles).
class Cpu : private NonCopyable {
public:
    enum Mode : u32 { User = 0x10, Fiq = 0x11, Irq = 0x12, Supervisor = 0x13, Abort = 0x17, Undefined = 0x1B, System = 0x1F };

    Cpu(Bus& bus, Interrupts& interrupts);

    void reset();  // state left by the BIOS boot sequence, PC = 0x08000000
    int step();

    bool halted() const { return halted_; }
    void halt() { halted_ = true; }

    // Register access for the BIOS emulation.
    u32 reg(int i) const { return i == 15 ? pc_ : r_[i]; }
    void setReg(int i, u32 v) {
        if (i == 15) {
            branchTo(v);
        } else {
            r_[i] = v;
        }
    }
    u32 cpsr() const { return cpsr_; }
    bool thumb() const { return (cpsr_ & 0x20) != 0; }
    u32 currentPc() const { return pc_; }
    void branchTo(u32 addr);  // keeps the current instruction set
    void setSwiHandler(SwiHandler* handler) { swiHandler_ = handler; }
    // Make the current instruction run again (used for IntrWait).
    void repeatInstruction() { branchTo(instructionAddr_); }

private:
    // ---- execution ----
    void executeArm(u32 op);
    void executeThumb(u16 op);
    void enterIrq();

    // ARM instruction groups
    void armBranchExchange(u32 op);
    void armBranch(u32 op);
    void armDataProcessing(u32 op);
    void armPsrTransfer(u32 op);
    void armMultiply(u32 op);
    void armMultiplyLong(u32 op);
    void armSwap(u32 op);
    void armHalfwordTransfer(u32 op);
    void armSingleTransfer(u32 op);
    void armBlockTransfer(u32 op);

    // ---- helpers ----
    bool condition(u32 cond) const;
    u32 shift(u32 value, u32 type, u32 amount, bool immediate, bool& carry) const;
    u32 addWithFlags(u32 a, u32 b, u32 carryIn, bool setFlags);
    void setNZ(u32 v) {
        cpsr_ = (cpsr_ & 0x3FFFFFFF) | (v & 0x80000000u) | (v == 0 ? 0x40000000u : 0);
    }
    void setC(bool c) { cpsr_ = c ? (cpsr_ | 0x20000000u) : (cpsr_ & ~0x20000000u); }
    bool flagC() const { return (cpsr_ & 0x20000000u) != 0; }
    u32 loadWord(u32 addr);  // with the ARM rotation for misaligned addresses
    u32 operandReg(int i) const { return r_[i]; }
    void writeReg(int i, u32 v) {
        if (i == 15) {
            branchTo(v);
        } else {
            r_[i] = v;
        }
    }
    int multiplyCycles(u32 rs, bool isSigned) const;

    // ---- modes ----
    static int bankIndex(u32 mode);
    void switchMode(u32 mode);
    void setCpsr(u32 value);
    u32 spsr() const;
    void setSpsr(u32 value);
    u32 userReg(int i) const;
    void setUserReg(int i, u32 v);

    Bus& bus_;
    Interrupts& interrupts_;
    SwiHandler* swiHandler_;

    u32 r_[16];  // r_[15] = address of the current instruction + 8 (ARM) / + 4 (Thumb)
    u32 cpsr_;
    u32 spsrBank_[6];
    u32 bank13_[6], bank14_[6];
    u32 fiqBank_[5], userBank_[5];  // r8-r12

    u32 pc_;               // next instruction to execute
    u32 instructionAddr_;  // instruction being executed
    bool branched_;
    bool sequential_;
    bool halted_;
    int internalCycles_;
};

}  // namespace gba

#endif
