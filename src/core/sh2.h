#pragma once

#include <array>
#include <cstdint>

namespace plumsg {

class Sh2;

// What an SH-2 sees outside its own chip: the address space below 0xC0000000 with the
// cache-through bits (0x20000000) already removed, so 0x06000000 and 0x26000000 both
// arrive as 0x06000000. `size` is 1, 2 or 4 bytes, values are right-aligned. A slow
// device adds its wait states with cpu.addCycles().
class Sh2Bus {
public:
    virtual ~Sh2Bus() = default;
    virtual uint32_t sh2Read(Sh2& cpu, uint32_t addr, int size) = 0;
    virtual void sh2Write(Sh2& cpu, uint32_t addr, uint32_t value, int size) = 0;
};

// Hitachi SH-2 (SH7604) interpreter: the CPU with its delay slots, and the on-chip
// modules games use: the division unit, the two-channel DMA controller (with the DREQ
// lines the 32X wires to its FIFO and PWM), the free-running and watchdog timers, the
// serial port (linked to the other SH-2), the interrupt controller and the cache's
// data array, which games use as 4 KB of fast RAM. The cache itself is not simulated:
// memory always reads its current contents.
class Sh2 {
public:
    Sh2(Sh2Bus& bus, int id) : bus_(bus), id_(id) {}

    int id() const { return id_; }  // 0 master, 1 slave
    void setPeer(Sh2* other) { peer_ = other; }

    // Power-on reset: clears the on-chip modules and reads PC and SP from vectors 0
    // and 1 of the bus (the boot ROM).
    void reset();
    // Starts at `pc` with `sp`, as if the boot ROM had jumped there.
    void jumpTo(uint32_t pc, uint32_t sp) {
        pc_ = pc;
        r_[15] = sp;
    }

    // Runs one instruction (or takes an interrupt, or one step of a DMA transfer).
    // Returns the clock cycles it took.
    int step();

    // The external interrupt request (the IRL pins): 0 none, else the level, 1-15.
    // Taken with an auto vector, 64 + level / 2.
    void setIrl(int level) { irl_ = level; }
    // The DMA request pins: channel 0 is the 32X's 68000 FIFO, channel 1 its PWM.
    void setDreq(int channel, bool on) { dreq_[channel] = on; }
    // A wait state from a slow device during the current access.
    void addCycles(int n) { extra_ += n; }
    bool sleeping() const { return sleeping_; }

    uint32_t pc() const { return pc_; }
    uint32_t reg(int n) const { return r_[n]; }
    void setReg(int n, uint32_t v) { r_[n] = v; }
    uint32_t sr() const { return sr_; }
    void setGbr(uint32_t v) { gbr_ = v; }
    void setVbr(uint32_t v) { vbr_ = v; }
    uint32_t vbr() const { return vbr_; }

    // Memory through the CPU's eyes (cache-through bits, on-chip registers, data array).
    uint32_t read(uint32_t addr, int size);
    void write(uint32_t addr, uint32_t value, int size);

    template <class Ar>
    void state(Ar& ar) {
        ar(r_, pc_, pr_, gbr_, vbr_, mach_, macl_, sr_, sleeping_, noIrqNext_, irl_, dreq_, dataArray_);
        ar(frt_.tier, frt_.ftcsr, frt_.frc, frt_.ocra, frt_.ocrb, frt_.tcr, frt_.tocr, frt_.ficr, frt_.counter,
           frt_.latch);
        ar(wdt_.wtcsr, wdt_.wtcnt, wdt_.rstcsr, wdt_.counter);
        ar(divu_.dvsr, divu_.dvdnth, divu_.dvdntl, divu_.dvcr, divu_.vcrdiv);
        ar(dmac_.sar, dmac_.dar, dmac_.tcr, dmac_.chcr, dmac_.vcrdma, dmac_.drcr, dmac_.dmaor);
        ar(sci_.smr, sci_.brr, sci_.scr, sci_.tdr, sci_.ssr, sci_.rdr);
        ar(intc_.ipra, intc_.iprb, intc_.vcra, intc_.vcrb, intc_.vcrc, intc_.vcrd, intc_.vcrwdt, intc_.icr);
        ar(ccr_, sbycr_, bsc_);
    }

private:
    uint32_t fetch(uint32_t addr);
    void execute(uint16_t op);
    void delaySlot(uint32_t target);  // runs the instruction after a delayed branch, then jumps
    uint32_t pcRelative() const { return inSlot_ ? slotTarget_ + 2 : pc_ + 4; }
    bool takeInterrupt();
    void exception(uint32_t vector, uint32_t returnPc);
    bool runDma();
    void runTimers(int cycles);
    uint8_t readIo(uint32_t addr);
    void writeIo(uint32_t addr, uint8_t value);
    void divide32();
    void divide64();
    void sciTransfer();

    bool t() const { return sr_ & 1; }
    void setT(bool v) { sr_ = (sr_ & ~1u) | (v ? 1u : 0u); }
    int imask() const { return (sr_ >> 4) & 15; }

    Sh2Bus& bus_;
    int id_;
    Sh2* peer_ = nullptr;

    std::array<uint32_t, 16> r_{};
    uint32_t pc_ = 0, pr_ = 0, gbr_ = 0, vbr_ = 0, mach_ = 0, macl_ = 0;
    uint32_t sr_ = 0xF0;  // T bit 0, S 1, I 4-7, Q 8, M 9
    bool sleeping_ = false;
    bool noIrqNext_ = false;  // the last instruction blocks interrupts for one instruction
    bool inSlot_ = false;
    uint32_t slotTarget_ = 0;
    int extra_ = 0;  // wait states added by the bus during this step
    int irl_ = 0;
    std::array<bool, 2> dreq_{};
    std::array<uint8_t, 0x1000> dataArray_{};

    struct Frt {
        uint8_t tier = 0x01, ftcsr = 0, tcr = 0, tocr = 0xE0;
        uint16_t frc = 0, ocra = 0xFFFF, ocrb = 0xFFFF, ficr = 0;
        uint32_t counter = 0;
        uint8_t latch = 0;  // the high byte, read or written first
    } frt_;
    struct Wdt {
        uint8_t wtcsr = 0x18, wtcnt = 0, rstcsr = 0x1F;
        uint32_t counter = 0;
    } wdt_;
    struct Divu {
        uint32_t dvsr = 0, dvdnth = 0, dvdntl = 0, dvcr = 0, vcrdiv = 0;
    } divu_;
    struct Dmac {
        std::array<uint32_t, 2> sar{}, dar{}, tcr{}, chcr{}, vcrdma{};
        std::array<uint8_t, 2> drcr{};
        uint32_t dmaor = 0;
    } dmac_;
    struct Sci {
        uint8_t smr = 0, brr = 0xFF, scr = 0, tdr = 0xFF, ssr = 0x84, rdr = 0;
    } sci_;
    struct Intc {
        uint16_t ipra = 0, iprb = 0, vcra = 0, vcrb = 0, vcrc = 0, vcrd = 0, vcrwdt = 0, icr = 0;
    } intc_;
    uint8_t ccr_ = 0, sbycr_ = 0;
    std::array<uint8_t, 32> bsc_{};  // bus state controller registers, kept but not acted on
};

}  // namespace plumsg
