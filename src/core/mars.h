#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "cartridge.h"
#include "sh2.h"
#include "video_frame_md.h"

namespace plumsg {

// The 32X (Mars): two SH-2s with 256 KB of SDRAM, a bitmap VDP with two 128 KB frame
// buffers and its own palette, a PWM sound unit, the registers both sides share
// (communication words, interrupts, the 68000-to-SH-2 FIFO) and the adapter's view of
// the cartridge for the 68000. The console owns it and calls it for every 68000 access
// in the areas the 32X answers, runs its CPUs alongside the 68000, and asks it to draw
// its layer over each line the VDP drew.
//
// No BIOS is needed: what the SH-2s' boot ROMs and the 68000's vector ROM do is done
// here (copying the game's initial program to SDRAM, the handshake with the 68000 and
// the jump to the game's entry points), as other emulators do.
class Mars : public Sh2Bus {
public:
    // Time is counted in thirds of a master clock: an SH-2 cycle is 7, a 68000 cycle 21.
    static constexpr int kTicksPerSh2Cycle = 7;

    explicit Mars(Cartridge& cart);

    void powerOn();  // clears memory and starts the boot sequence again
    void reset();        // everything but memory as it powers on (powerOn() does it)
    void resetButton();  // the console's reset button: the SH-2s get the VRES interrupt

    // Runs both SH-2s (and the PWM) up to `masterClock` of the console.
    void runUntil(uint64_t masterClock);
    // Starts the clocks again from 0 (the console does it with its own at each reset);
    // what was due later stays as far ahead, for the SH-2s run on through a reset.
    void rebaseClock() {
        const uint64_t base = masterTicks_;
        for (uint64_t* t : {&sh2Ticks_[0], &sh2Ticks_[1], &pwmTicks_, &pwm_.nextTick, &vdp_.fillEnd})
            *t = *t > base ? *t - base : 0;
        masterTicks_ = now_ = 0;
    }

    // 68000 side. `read68k` returns false when the address is not the 32X's and the
    // console should answer normally. `upper` and `lower` say which bytes are accessed.
    bool read68k(uint32_t addr, uint16_t& value);
    bool write68k(uint32_t addr, uint16_t value, bool upper, bool lower);

    // Video timing from the console: the start of a line (and of VBlank on the first
    // line after the active ones), and the start of horizontal blanking.
    void startLine(int line, int activeLines);
    void startHblank(int line, int activeLines);
    // Draws the 32X layer of one line over what the VDP drew there.
    void renderLine(int line, VideoFrameMd& frame);

    // Stereo PWM samples at the output rate produced since the last call.
    void setSampleRate(double hz, double masterClock);
    // A PAL console: the VDP's mode register says so, and the games' start-up code checks
    // it against the Mega Drive's version register.
    void setPal(bool pal) { pal_ = pal; }
    std::vector<int16_t>& samples() { return pwmOut_; }

    Sh2& master() { return master_; }
    Sh2& slave() { return slave_; }
    const std::array<uint16_t, 8>& comm() const { return comm_; }
    const std::array<uint8_t, 0x40000>& sdram() const { return sdram_; }

    // What only changes how long things take (the SH-2s' caches, their spin loops), a
    // block of its own in the savestate.
    template <class Ar>
    void timingState(Ar& ar) {
        master_.timingState(ar);
        slave_.timingState(ar);
    }
    // Turns on the SH-2s' caches, as the boot ROMs do, where they are off (a state from
    // a version that did not).
    void bootCache();
    void setRomView();
    void clearTiming() {
        master_.clearTiming();
        slave_.clearTiming();
    }

    // Kept apart from state(): older states lack them, and get them from adapterReset.
    template <class Ar>
    void ioState(Ar& ar) {
        ar(io_.resetBit, io_.tvBit8);
    }
    template <class Ar>
    void state(Ar& ar) {
        master_.state(ar);
        slave_.state(ar);
        ar(sdram_, dram_, cram_, comm_, masterTicks_, sh2Ticks_, pwmTicks_, now_);
        ar(io_.adapterEnable, io_.adapterReset, io_.romBank, io_.cartridgeMode, io_.vectorLevel4, io_.hperiod,
           io_.hcounter, io_.hintVblank);
        if constexpr (Ar::kLoading) {
            io_.resetBit = io_.adapterReset;
            io_.tvBit8 = false;
        }
        for (auto& q : irq_) ar(q.enable, q.active);
        ar(dreq_.vram, dreq_.dma, dreq_.active, dreq_.source, dreq_.target, dreq_.length, dreq_.fifo, dreq_.count);
        ar(vdp_.mode, vdp_.lines, vdp_.priority, vdp_.dotshift, vdp_.autofillLength, vdp_.autofillAddress,
           vdp_.autofillData, vdp_.access, vdp_.active, vdp_.select, vdp_.fillEnd, vdp_.hblank, vdp_.vblank,
           vdp_.latchMode, vdp_.latchLines, vdp_.latchPriority, vdp_.latchDotshift);
        ar(pwm_.lmode, pwm_.rmode, pwm_.mono, pwm_.dreqIrq, pwm_.timer, pwm_.cycle, pwm_.periods, pwm_.lfifo,
           pwm_.rfifo, pwm_.lcount, pwm_.rcount, pwm_.lsample, pwm_.rsample, pwm_.llatch, pwm_.rlatch, pwm_.mlatch,
           pwm_.nextTick);
        ar(out_.acc, out_.time, out_.dcInL, out_.dcInR, out_.dcOutL, out_.dcOutR);
    }

private:
    // SH-2 bus.
    uint32_t sh2Read(Sh2& cpu, uint32_t addr, int size) override;
    void sh2Write(Sh2& cpu, uint32_t addr, uint32_t value, int size) override;
    uint16_t sh2Read16(Sh2& cpu, uint32_t addr);
    void sh2Write16(Sh2& cpu, uint32_t addr, uint16_t value, bool upper, bool lower);
    uint16_t readSysReg(Sh2& cpu, uint32_t addr);
    uint16_t readSysRegValue(Sh2& cpu, uint32_t reg);
    void writeSysReg(Sh2& cpu, uint32_t addr, uint16_t value, bool upper, bool lower);
    // Registers both sides see the same way: PWM, VDP and palette.
    bool readShared(uint32_t reg, uint16_t& value);
    void writeShared(uint32_t reg, uint16_t value, bool upper, bool lower);

    void bootSh2s();        // what the boot ROMs do, then the CPUs run the game
    void updateIrl(int cpu);
    void updateIrls() {
        updateIrl(0);
        updateIrl(1);
    }
    void runPwm(uint64_t ticks);  // brings the PWM and its output up to `ticks`
    void pwmOutput(uint64_t ticks);
    void selectFramebuffer(int select);
    bool framebufferBusy() const;
    void fill();
    uint16_t* frontBuffer() { return &dram_[static_cast<size_t>(vdp_.active) * 0x10000]; }
    uint16_t* backBuffer() { return &dram_[static_cast<size_t>(vdp_.active ^ 1) * 0x10000]; }
    void pushFifo(uint16_t value);
    uint16_t popFifo();
    void raiseDreq0();

    Cartridge& cart_;
    bool pal_ = false;
    Sh2 master_{*this, 0};
    Sh2 slave_{*this, 1};
    std::array<uint8_t, 0x400> bootRom_[2]{};  // generated boot code of each SH-2
    std::array<uint16_t, 0x80> vectorRom_{};   // the 68000's vectors while the adapter is on

    std::array<uint8_t, 0x40000> sdram_{};
    std::array<uint16_t, 0x20000> dram_{};  // two frame buffers of 0x10000 words
    std::array<uint16_t, 0x100> cram_{};
    std::array<uint16_t, 8> comm_{};

    uint64_t masterTicks_ = 0;           // where the console asked us to run to
    std::array<uint64_t, 2> sh2Ticks_{};  // where each SH-2 has got to
    uint64_t pwmTicks_ = 0;
    uint64_t now_ = 0;  // the time of the access being made, for the fill timer

    struct Io {
        bool adapterEnable = false, adapterReset = false;
        // What the 68000 reads back of A15100's RES and A1511A's bit 8 (testpico's "32x
        // init"): RES reads 1 from power on, though the SH-2s wait for it to go 0 to 1.
        bool resetBit = true, tvBit8 = false;
        uint8_t romBank = 0;
        bool cartridgeMode = false;
        uint32_t vectorLevel4 = 0;
        uint8_t hperiod = 0, hcounter = 0;
        bool hintVblank = false;
    } io_;

    // Interrupt sources of each SH-2: VRES, VINT, HINT, CMD, PWM.
    enum { kVres, kVint, kHint, kCmd, kPwm, kSources };
    struct Irq {
        std::array<bool, kSources> enable{}, active{};
    };
    std::array<Irq, 2> irq_{};

    struct Dreq {
        bool vram = false, dma = false, active = false;
        uint32_t source = 0, target = 0;
        uint16_t length = 0xFFFF;  // as the console powers on (testpico)
        std::array<uint16_t, 8> fifo{};
        uint8_t count = 0;
    } dreq_;

    struct Vdp {
        uint8_t mode = 0;  // 0 blank, 1 packed pixels, 2 direct colour, 3 run length
        bool lines = false, priority = false, dotshift = false;
        uint8_t autofillLength = 0;
        uint16_t autofillAddress = 0, autofillData = 0;
        bool access = false;  // FM: the SH-2s have the VDP, not the 68000
        uint8_t active = 0;   // the frame buffer shown
        uint8_t select = 0;   // the one asked for, swapped in at VBlank
        uint64_t fillEnd = 0;
        bool hblank = false, vblank = true;
        uint8_t latchMode = 0;
        bool latchLines = false, latchPriority = false, latchDotshift = false;
    } vdp_;

    struct Pwm {
        uint8_t lmode = 0, rmode = 0;
        bool mono = false, dreqIrq = false;
        uint8_t timer = 0;
        uint16_t cycle = 0;
        uint32_t periods = 0;
        std::array<uint16_t, 3> lfifo{}, rfifo{};
        uint8_t lcount = 0, rcount = 0;
        uint16_t lsample = 0, rsample = 0;
        uint16_t llatch = 0, rlatch = 0, mlatch = 0;
        uint64_t nextTick = 0;
    } pwm_;

    // Box-filter resampling of the PWM output to the frontend's rate.
    struct Output {
        double ticksPerSample = 0;
        std::array<double, 2> acc{};
        double time = 0;  // ticks accumulated towards the next output sample
        double dcInL = 0, dcInR = 0, dcOutL = 0, dcOutR = 0;
    } out_;
    std::vector<int16_t> pwmOut_;
    uint64_t levelKey_ = ~uint64_t{0};  // what pwmOutput() last worked its levels out from
    double levelL_ = 0, levelR_ = 0;
};

}  // namespace plumsg
