#pragma once

#include <cstdint>

namespace plumsg {

// Everything the CPU can see: memory and I/O ports.
class Z80Bus {
public:
    virtual ~Z80Bus() = default;
    virtual uint8_t read(uint16_t addr) = 0;
    virtual void write(uint16_t addr, uint8_t value) = 0;
    virtual uint8_t in(uint16_t port) = 0;
    virtual void out(uint16_t port, uint8_t value) = 0;
};

// Zilog Z80 interpreter, including the undocumented opcodes and flags.
class Z80 {
public:
    // Register pair stored little-endian so the halves can be addressed directly.
    union Pair {
        uint16_t w;
        struct {
            uint8_t l, h;
        } b;
    };

    explicit Z80(Z80Bus& bus);

    void reset();

    // Executes one instruction (or accepts a pending interrupt). Returns T-states used.
    int step();

    // Maskable interrupt line (level triggered, active while true).
    void setIrqLine(bool asserted) { irqLine_ = asserted; }
    // Non-maskable interrupt (edge triggered).
    void triggerNmi() { nmiPending_ = true; }

    Pair af{}, bc{}, de{}, hl{}, ix{}, iy{};
    Pair af2{}, bc2{}, de2{}, hl2{};
    uint16_t sp = 0xFFFF;
    uint16_t pc = 0;
    uint16_t wz = 0;  // internal MEMPTR, leaks into some undocumented flags
    uint8_t i = 0;
    uint8_t r = 0;
    bool iff1 = false;
    bool iff2 = false;
    int im = 0;
    bool halted = false;

    template <class Ar>
    void state(Ar& ar) {
        ar(af.w, bc.w, de.w, hl.w, ix.w, iy.w, af2.w, bc2.w, de2.w, hl2.w, sp, pc, wz, i, r, iff1, iff2, im, halted,
           idx_, irqLine_, nmiPending_, eiDelay_);
    }

private:
    uint8_t& A() { return af.b.h; }
    uint8_t& F() { return af.b.l; }

    uint8_t read8(uint16_t a) { return bus_.read(a); }
    void write8(uint16_t a, uint8_t v) { bus_.write(a, v); }
    uint16_t read16(uint16_t a);
    void write16(uint16_t a, uint16_t v);
    uint8_t fetchOp();
    uint8_t fetch8() { return read8(pc++); }
    uint16_t fetch16();
    void push(uint16_t v);
    uint16_t pop();

    // Index-aware register access. idx_ selects HL (0), IX (1) or IY (2).
    Pair& indexReg();
    uint8_t* reg8(int r, bool allowIndex);
    Pair& rp(int p);
    Pair& rp2(int p);
    uint16_t indirectAddr();  // address for "(HL)", or "(IX+d)" (fetches d)
    bool cond(int cc) const;

    int execMain(uint8_t op);
    int execCB();
    int execIndexedCB();
    int execED();

    void alu(int op, uint8_t v);
    void add8(uint8_t v, int carry);
    void sub8(uint8_t v, int carry, bool store);
    uint8_t inc8(uint8_t v);
    uint8_t dec8(uint8_t v);
    uint16_t add16(uint16_t a, uint16_t b);
    void adc16(uint16_t v);
    void sbc16(uint16_t v);
    uint8_t rotShift(int op, uint8_t v);
    void bitTest(int bit, uint8_t v, uint8_t xyFrom);
    void daa();
    int blockOp(int y, int z);

    Z80Bus& bus_;
    int idx_ = 0;
    bool irqLine_ = false;
    bool nmiPending_ = false;
    bool eiDelay_ = false;
};

}  // namespace plumsg
