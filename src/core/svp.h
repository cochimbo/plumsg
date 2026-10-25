#pragma once

#include <array>
#include <cstdint>

#include "cartridge.h"

namespace plumsg {

// Virtua Racing's cartridge: the SVP chip, a Samsung SSP1601 DSP with 128 KB of DRAM,
// 2 KB of instruction RAM and a memory controller. The DSP runs its program from the
// cartridge ROM (word address = ROM word) and from the instruction RAM (the first 0x400
// words), works on two banks of 256 words of its own RAM, and reaches the ROM, the DRAM
// and the instruction RAM through "programmable memory" registers (PM0-PM4) that step
// through memory by themselves once set up through PMC. The game's 68000 talks to it
// through XST (0xA15000/2) and a status word (0xA15004), and reads the pictures it draws
// from the DRAM at 0x300000, directly or rearranged into tiles at 0x390000 and 0x3A0000.
class Svp {
public:
    // The ROM header says "SV" at 0x1C8, where the DSP's boot code looks.
    static bool detect(const Cartridge& cart);

    explicit Svp(const Cartridge& cart) : cart_(cart) {}
    void powerOn();
    void reset();
    void runUntil(uint64_t masterClock);
    void rebaseClock();

    // The 68000's side: DRAM, its tile views and the registers. False: not the chip's.
    bool read68k(uint32_t addr, uint16_t& value);
    bool write68k(uint32_t addr, uint16_t value, bool upper, bool lower);
    // A VDP DMA from the cartridge gets each word one read late: the first word it
    // writes is the one before the source (Virtua Racing sets its sources to match).
    bool dmaRead(uint32_t addr, uint16_t& value);

    // For tests.
    std::array<uint16_t, 0x10000>& dram() { return dram_; }
    uint16_t pc() const { return pc_; }

    template <class Ar>
    void state(Ar& ar) {
        ar(dram_, iram_, ram_, x_, y_, a_, st_, stack_, sp_, pc_, r_, pm0_, pm1_, pm2_, xst_, pm4_, pmc_, pmcState_);
        ar(pmac_[0], pmac_[1], ticks_);
    }

private:
    // The registers by number: -, X, Y, A, ST, STACK, PC, P, PM0, PM1, PM2, XST, PM4, (13), PMC, AL.
    enum { kZero, kX, kY, kA, kSt, kStack, kPc, kP, kPm0, kPm1, kPm2, kXst, kPm4, kExt5, kPmc, kAl };
    static constexpr uint16_t kFlagZ = 0x2000, kFlagN = 0x8000, kFlags = 0xF000;

    int step();  // one instruction; its cycles
    uint16_t fetch() { return programWord(pc_++); }
    uint16_t programWord(uint16_t addr) const;
    uint16_t romWord(uint32_t index) const;
    int32_t product() const { return static_cast<int32_t>(static_cast<int16_t>(x_)) * static_cast<int16_t>(y_) * 2; }
    uint16_t readReg(int r);
    void writeReg(int r, uint16_t v);
    void push(uint16_t v);
    uint16_t pop();
    bool condition(uint16_t op) const;
    void setZn() {
        st_ = static_cast<uint16_t>((st_ & ~(kFlagZ | kFlagN)) | (a_ == 0 ? kFlagZ : (a_ >> 16 & kFlagN)));
    }
    void setFlags(uint32_t v) {  // add, sub, cmp: L and V are not kept (nothing reads them)
        st_ = static_cast<uint16_t>((st_ & ~kFlags) | (v == 0 ? kFlagZ : (v >> 16 & kFlagN)));
    }
    void alu(int op, uint32_t v);  // A op= v, v already in A's 32-bit place

    // (ri) and ((ri)): `ri` 0-3, `bank` 0 (r0-r3, RAM0) or 1 (r4-r7, RAM1), `mod` 0-3.
    uint16_t& pointed(int ri, int bank, int mod, bool reading);
    uint16_t readPointer(uint16_t op) { return pointed(op & 3, op >> 8 & 1, op >> 2 & 3, true); }
    void writePointer(uint16_t op, uint16_t v) { pointed(op & 3, op >> 8 & 1, op >> 2 & 3, false) = v; }
    uint16_t readProgramPointer(uint16_t op);

    // The memory controller.
    uint32_t pmAccess(int reg, bool write, uint16_t v, bool& handled);
    uint16_t readPm(int reg);
    void writePm(int reg, uint16_t v);

    const Cartridge& cart_;
    std::array<uint16_t, 0x10000> dram_{};
    std::array<uint16_t, 0x400> iram_{};
    std::array<uint16_t, 0x200> ram_{};  // RAM0, then RAM1
    uint16_t x_ = 0, y_ = 0;
    uint32_t a_ = 0;
    uint16_t st_ = 0;
    std::array<uint16_t, 6> stack_{};
    uint8_t sp_ = 0;
    uint16_t pc_ = 0x400;
    std::array<uint8_t, 8> r_{};
    // PM0 and XST double as the two sides' mailbox when ST's GP0 bits are clear: XST is
    // the word passed, PM0 bit 0 says the DSP wrote it, bit 1 that the 68000 did.
    uint16_t pm0_ = 0, pm1_ = 0, pm2_ = 0, xst_ = 0, pm4_ = 0;
    uint32_t pmc_ = 0;        // mode << 16 | address
    uint8_t pmcState_ = 0;    // 0, 1: address given (mode next), 2: set (a blind PMx access takes it)
    std::array<uint32_t, 5> pmac_[2]{};  // each PMx's read and write programming
    uint16_t op_ = 0;         // the instruction under way
    uint64_t ticks_ = 0;      // where the DSP has got to, in master clocks * 3
};

}  // namespace plumsg
