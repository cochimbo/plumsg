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
    // Interrupt priority level requested by the hardware, 0 (none) .. 7 (NMI).
    void setIrqLevel(int level) { irqLevel_ = level & 7; }
    int irqLevel() const { return irqLevel_; }

    // a[7] is the active stack pointer; the other one is kept in otherSp.
    uint32_t d[8]{};
    uint32_t a[8]{};
    uint32_t otherSp = 0;
    uint32_t pc = 0;
    uint16_t sr = 0x2700;
    bool stopped = false;  // STOP: waiting for an interrupt
    bool halted = false;   // double bus fault: the CPU is locked until reset
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

private:
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
    int cycles_ = 0;
    uint32_t instrPc_ = 0;
    uint16_t ir_ = 0;
    int faultPcAdjust_ = 0;
};

}  // namespace plumsg
