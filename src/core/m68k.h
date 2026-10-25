#pragma once

#include <cstdint>

namespace plumsg {

// Everything the 68000 can see: a 24-bit, big-endian address space. Word accesses are
// always to even addresses (the CPU raises an address error before calling the bus).
class M68kBus {
public:
    virtual ~M68kBus() = default;
    virtual uint8_t read8(uint32_t addr) = 0;
    virtual uint16_t read16(uint32_t addr) = 0;
    virtual void write8(uint32_t addr, uint8_t value) = 0;
    virtual void write16(uint32_t addr, uint16_t value) = 0;
    // Called when the CPU accepts an interrupt, so the device can drop its request.
    virtual void irqAcknowledge(int /*level*/) {}
    // On the Mega Drive the bus never completes TAS's write cycle, so TAS does not
    // write its result back. Plain 68000 test sets expect the write.
    virtual bool tasWrites() const { return true; }
};

// Motorola 68000 interpreter: every instruction, addressing mode and exception, with
// instruction-level cycle counts (including the operand-dependent MULU/MULS/DIVU/DIVS).
// It does not model the prefetch queue or timing inside an instruction.
class M68000 {
public:
    explicit M68000(M68kBus& bus) : bus_(bus) {}

    // Reads the initial supervisor stack pointer and PC from the vector table.
    void reset();
    // Executes one instruction, or takes a pending interrupt. Returns clock cycles used.
    int step();
    // Interrupt priority level requested by the hardware, 0 (none) .. 7 (NMI). A new level
    // is taken after one more instruction: the one under way when the line changed still
    // finishes, and may read the flag that raised it (Mega-Lo-Mania/Tyrants waits on the
    // VINT flag with the VINT on; Sesame Street Counting Cafe enables it and then sets
    // up what its handler counts).
    void setIrqLevel(int level) { irqLevel_ = level & 7; }
    int irqLevel() const { return irqLevel_; }
    // Holds the program where it is now, running only its interrupts (song extraction:
    // the sound driver plays from VBlank while the game stays put). It lets VBlank in.
    void holdMain(bool on);
    // Song extraction, for sound drivers asked by calling a routine (the number in a
    // register): calls to `entry` from the game return at once, without running it. 0: none.
    // skipPops: bytes of arguments the routine takes off the stack itself as it returns.
    uint32_t skipRoutine = 0;
    uint32_t skipPops = 0;
    // Calls the routine at `entry` from where the program is, as the program would, with
    // the registers given (d0-d7, then a0-a6; `keep` in one keeps it as it is) and
    // interrupts masked; `argBytes` (2 or 4) of `arg` pushed first, for a routine that takes
    // its number on the stack. It runs as the console goes on; on its return every
    // register is put back as it was and the program carries on. inRoutine(): not back yet.
    void callRoutine(uint32_t entry, const uint32_t (&regs)[15], uint32_t keep, uint32_t arg = 0, int argBytes = 0);
    bool inRoutine() const { return calling_; }
    void forgetRoutine() { calling_ = false; }  // a state loaded: not in one any more

    // a[7] is the active stack pointer; the other one is kept in otherSp.
    uint32_t d[8]{};
    uint32_t a[8]{};
    uint32_t otherSp = 0;
    uint32_t pc = 0;
    uint16_t sr = 0x2700;
    bool stopped = false;  // STOP: waiting for an interrupt
    bool halted = false;   // double bus fault: the CPU is locked until reset
    bool holding = false;  // holdMain(): only interrupt handlers run
    uint32_t holdSp = 0;   // the stack pointer of the program held: deeper is a handler
    uint32_t addressErrors = 0;  // address error exceptions taken, for debugging

    bool supervisor() const { return (sr & 0x2000) != 0; }
    uint32_t usp() const { return supervisor() ? otherSp : a[7]; }
    uint32_t ssp() const { return supervisor() ? a[7] : otherSp; }
    void setUsp(uint32_t v) { (supervisor() ? otherSp : a[7]) = v; }
    void setSsp(uint32_t v) { (supervisor() ? a[7] : otherSp) = v; }
    // Changes SR, swapping stack pointers when the supervisor bit changes.
    void setSr(uint16_t value);

    template <class Ar>
    void state(Ar& ar) {
        ar(d, a, otherSp, pc, sr, stopped, halted, irqLevel_);
    }
    // A state without irqState() (older): its level counts as seen already.
    void settleIrq() { seenIrqLevel_ = irqLevel_; }
    // The level seen as the last instruction began (kept apart: older states lack it).
    template <class Ar>
    void irqState(Ar& ar) {
        ar(seenIrqLevel_);
    }

private:
    // What callRoutine() puts back.
    bool calling_ = false;
    uint32_t savedD_[8]{}, savedA_[8]{}, savedOtherSp_ = 0, savedPc_ = 0;
    uint16_t savedSr_ = 0;
    bool savedStopped_ = false, savedHolding_ = false;
    void returnFromRoutine();

    struct Ea {
        enum Kind : uint8_t { DataReg, AddrReg, Memory, Immediate } kind;
        uint8_t reg;
        uint32_t addr;
    };
    struct AddressError {
        uint32_t addr;
        bool read;
        bool instruction;
    };

    uint8_t read8(uint32_t addr) { return bus_.read8(addr & 0xFFFFFF); }
    uint16_t read16(uint32_t addr, bool instruction = false);
    uint32_t read32(uint32_t addr);
    void write8(uint32_t addr, uint8_t v) { bus_.write8(addr & 0xFFFFFF, v); }
    void write16(uint32_t addr, uint16_t v);
    void write32(uint32_t addr, uint32_t v);
    uint16_t fetch16();
    uint32_t fetch32();
    void push16(uint16_t v);
    void push32(uint32_t v);
    uint16_t pop16();
    uint32_t pop32();

    // Resolves an effective address (mode, register) for an operand of `size` bytes,
    // applying (An)+ / -(An) and fetching extension words. Adds the standard address
    // calculation time when `timed`.
    Ea resolve(int mode, int reg, int size, bool timed = true);
    uint32_t read(const Ea& ea, int size);
    void write(const Ea& ea, int size, uint32_t value);
    uint32_t indexed(uint32_t base);  // brief extension word: base + d8 + Xn
    static int eaTime(int mode, int reg, bool lng);
    static bool validEa(int mode, int reg, uint16_t allowed);

    bool cond(int cc) const;
    void setNZ(uint32_t v, int size);
    uint32_t add(uint32_t src, uint32_t dst, int size, bool withX);
    uint32_t sub(uint32_t src, uint32_t dst, int size, bool withX, bool setX = true);
    uint8_t abcd(uint8_t src, uint8_t dst);
    uint8_t sbcd(uint8_t src, uint8_t dst);

    void exception(int vector, uint32_t returnPc);
    void addressErrorException(const AddressError& e);

    void execute(uint16_t op);
    void group0(uint16_t op);
    void move(uint16_t op);
    void group4(uint16_t op);
    void group5(uint16_t op);
    void branch(uint16_t op);
    void arith(uint16_t op);  // groups 8, 9, B, C, D
    void shift(uint16_t op);
    void movem(uint16_t op);
    void illegal() { exception(4, instrPc_); }
    bool privileged();
    void jump(uint32_t target);  // sets PC; an odd target faults on the next fetch  // true when in user mode (and raises the exception)

    M68kBus& bus_;
    int irqLevel_ = 0;
    int seenIrqLevel_ = 0;
    int cycles_ = 0;
    uint32_t instrPc_ = 0;
    uint16_t ir_ = 0;
    int faultPcAdjust_ = 0;
    // The instruction was not run (illegal, line A/F, privilege violation): no trace after it.
    bool untraced_ = false;
};

}  // namespace plumsg
