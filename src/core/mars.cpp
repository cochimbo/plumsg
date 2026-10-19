#include "mars.h"

#include <algorithm>
#include <cmath>

namespace plumsg {

namespace {

// Where the IRQ sources sit on the SH-2s' interrupt levels.
constexpr int kLevels[] = {14, 12, 10, 8, 6};  // VRES, VINT, HINT, CMD, PWM

uint32_t colorOf(uint16_t c) {
    auto expand = [](uint32_t v) { return (v << 3) | (v >> 2); };
    return 0xFF000000u | expand(c & 31) << 16 | expand((c >> 5) & 31) << 8 | expand((c >> 10) & 31);
}

}  // namespace

Mars::Mars(Cartridge& cart) : cart_(cart) {
    master_.setPeer(&slave_);
    slave_.setPeer(&master_);
    master_.setFastMemory(0x06000000, 0x40000, sdram_.data());
    slave_.setFastMemory(0x06000000, 0x40000, sdram_.data());

    // The SH-2 boot ROMs. Every exception vector leads to a loop at 0x200; reset starts
    // at 0x204. The master waits a little (the 68000 is still clearing memory), says
    // "M_OK" in the first communication words and jumps to the game's master entry; the
    // slave waits for that, answers "S_OK" and jumps to the slave entry. Copying the
    // game's initial program and setting VBR and GBR is done by bootSh2s().
    for (int c = 0; c < 2; ++c) {
        auto& rom = bootRom_[c];
        auto put16 = [&](uint32_t a, uint16_t v) {
            rom[a] = static_cast<uint8_t>(v >> 8);
            rom[a + 1] = static_cast<uint8_t>(v);
        };
        auto put32 = [&](uint32_t a, uint32_t v) {
            put16(a, static_cast<uint16_t>(v >> 16));
            put16(a + 2, static_cast<uint16_t>(v));
        };
        for (uint32_t v = 0; v < 128; ++v) put32(v * 4, 0x200);
        const uint32_t sp = c == 0 ? 0x06040000 : 0x0603F800;
        put32(0, 0x204);
        put32(4, sp);
        put32(8, 0x204);
        put32(12, sp);
        put16(0x200, 0xAFFE);  // bra 0x200
        put16(0x202, 0x0009);  // nop
        static constexpr uint16_t kMaster[] = {
            0xD10E,  // 204 mov.l @(0x240),r1      delay count
            0x71FF,  // 206 add #-1,r1
            0x4115,  // 208 cmp/pl r1
            0x89FC,  // 20a bt 0x206
            0xD00D,  // 20c mov.l @(0x244),r0      "M_OK"
            0xC208,  // 20e mov.l r0,@(0x20,gbr)
            0xD20D,  // 210 mov.l @(0x248),r2      where the entry point is
            0x6822,  // 212 mov.l @r2,r8
            0x482B,  // 214 jmp @r8
            0x0009,  // 216 nop
        };
        static constexpr uint16_t kSlave[] = {
            0xD10E,  // 204 mov.l @(0x240),r1      "M_OK"
            0xC608,  // 206 mov.l @(0x20,gbr),r0
            0x3100,  // 208 cmp/eq r0,r1
            0x8BFC,  // 20a bf 0x206
            0xD00D,  // 20c mov.l @(0x244),r0      "S_OK"
            0xC209,  // 20e mov.l r0,@(0x24,gbr)
            0xD20D,  // 210 mov.l @(0x248),r2
            0x6822,  // 212 mov.l @r2,r8
            0x482B,  // 214 jmp @r8
            0x0009,  // 216 nop
        };
        const uint16_t* code = c == 0 ? kMaster : kSlave;
        for (uint32_t i = 0; i < 10; ++i) put16(0x204 + i * 2, code[i]);
        if (c == 0) {
            put32(0x240, 0x10000);
            put32(0x244, 0x4D5F4F4B);  // "M_OK"
            put32(0x248, 0x220003E0);
        } else {
            put32(0x240, 0x4D5F4F4B);
            put32(0x244, 0x535F4F4B);  // "S_OK"
            put32(0x248, 0x220003E4);
        }
    }

    // The 68000's vector ROM, seen while the adapter is on: every vector jumps into the
    // table the game keeps at 0x880200, and two small routines the games' start-up code
    // calls (they set the cartridge's bank registers and give the ROM back to the SH-2s).
    for (uint32_t i = 1; i < 0xC0 / 4; ++i) {
        const uint32_t target = 0x880200 + (i - 1) * 6;
        vectorRom_[i * 2] = static_cast<uint16_t>(target >> 16);
        vectorRom_[i * 2 + 1] = static_cast<uint16_t>(target);
    }
    vectorRom_[0x70 / 2] = vectorRom_[0x72 / 2] = 0;
    for (uint32_t a = 0xC0; a < 0x100; a += 2) vectorRom_[a / 2] = 0x4E71;  // nop
    static constexpr uint16_t kAt0xC8[] = {
        0x1280,                          // move.b d0,(a1)
        0x0239, 0x00FE, 0x00A1, 0x5107,  // and.b #$fe,$a15107 (RV off)
        0x4E75,                          // rts
    };
    static constexpr uint16_t kAt0xD4[] = {
        0x48E7, 0x8040,                  // movem.l d0/a1,-(sp)
        0x227C, 0x00A1, 0x30F1,          // movea.l #$a130f1,a1
        0x7007,                          // moveq #7,d0
        0x12D8,                          // move.b (a0)+,(a1)+
        0x5289,                          // addq.l #1,a1
        0x51C8, 0xFFFA,                  // dbra d0,*-4
        0x0239, 0x00FE, 0x00A1, 0x5107,  // and.b #$fe,$a15107
        0x4CDF, 0x0201,                  // movem.l (sp)+,d0/a1
    };
    std::copy(std::begin(kAt0xC8), std::end(kAt0xC8), vectorRom_.begin() + 0xC8 / 2);
    std::copy(std::begin(kAt0xD4), std::end(kAt0xD4), vectorRom_.begin() + 0xD4 / 2);
    vectorRom_[0xFE / 2] = 0x4E75;  // rts

    powerOn();
}

void Mars::powerOn() {
    sdram_.fill(0);
    dram_.fill(0);
    cram_.fill(0);
    io_ = Io{};
    reset();
}

void Mars::reset() {
    const uint32_t vector4 = io_.vectorLevel4;
    io_ = Io{};
    io_.vectorLevel4 = vector4;
    comm_.fill(0);
    irq_ = {};
    dreq_ = Dreq{};
    vdp_ = Vdp{};
    const uint64_t pwmTicks = pwm_.nextTick;
    pwm_ = Pwm{};
    pwm_.nextTick = pwmTicks;
    out_.acc = {};
    out_.time = 0;
    out_.dcInL = out_.dcInR = out_.dcOutL = out_.dcOutR = 0;
    master_.reset();
    slave_.reset();
    updateIrls();
}

void Mars::bootSh2s() {
    master_.reset();
    slave_.reset();
    irq_ = {};
    for (auto& q : irq_) q.enable[kVres] = true;
    for (Sh2* cpu : {&master_, &slave_}) {
        cpu->setGbr(0x20004000);
        cpu->setDreq(0, false);
        cpu->setDreq(1, false);
    }
    const std::vector<uint8_t>& rom = cart_.rom();
    auto rd = [&](uint32_t a, int bytes) {
        uint32_t v = 0;
        for (int i = 0; i < bytes; ++i) v = v << 8 | (a + static_cast<uint32_t>(i) < rom.size() ? rom[a + static_cast<uint32_t>(i)] : 0);
        return v;
    };
    // The initial program: where it is in the ROM, where it goes in SDRAM, its size.
    const uint32_t src = rd(0x3D4, 4) & 0x3FFFFF;
    const uint32_t dst = rd(0x3D8, 4) & 0x3FFFF;
    const uint32_t size = std::min<uint32_t>(rd(0x3DC, 4), 0x40000);
    for (uint32_t i = 0; i < size; ++i) {
        const uint32_t a = src + i;
        sdram_[(dst + i) & 0x3FFFF] = a < rom.size() ? rom[a] : 0;
    }
    master_.setVbr(rd(0x3E8, 4));
    slave_.setVbr(rd(0x3EC, 4));
    comm_[4] = static_cast<uint16_t>(rd(0x18E, 2));  // the checksum, as the BIOS leaves it
    updateIrls();
}

void Mars::updateIrl(int c) {
    int level = 0;
    for (int s = 0; s < kSources; ++s)
        if (irq_[c].enable[s] && irq_[c].active[s]) level = std::max(level, kLevels[s]);
    (c ? slave_ : master_).setIrl(level);
}

// ---------------------------------------------------------------------------------
// Scheduling

void Mars::runUntil(uint64_t masterClock) {
    const uint64_t target = masterClock * 3;
    if (target <= masterTicks_) return;
    masterTicks_ = target;
    if (!io_.adapterReset) {  // both SH-2s held in reset
        sh2Ticks_[0] = std::max(sh2Ticks_[0], target);
        sh2Ticks_[1] = std::max(sh2Ticks_[1], target);
        runPwm(target);
        return;
    }
    // Whichever SH-2 is behind runs next, a few instructions at a time (at most
    // kQuantum cycles past the other), so they see each other's writes nearly in order.
    constexpr uint64_t kQuantum = 16 * kTicksPerSh2Cycle;
    for (;;) {
        const int c = sh2Ticks_[0] <= sh2Ticks_[1] ? 0 : 1;
        uint64_t& ticks = sh2Ticks_[c];
        if (ticks >= target) break;
        const uint64_t until = std::min(target, sh2Ticks_[c ^ 1] + kQuantum);
        Sh2& cpu = c ? slave_ : master_;
        do {
            now_ = ticks;
            if (pwm_.nextTick <= now_) runPwm(now_);
            ticks += static_cast<uint64_t>(cpu.step()) * kTicksPerSh2Cycle;
            cpu.setDreq(1, false);  // the PWM's request is a pulse
        } while (ticks < until);
    }
    runPwm(target);
}

// ---------------------------------------------------------------------------------
// PWM

void Mars::setSampleRate(double hz, double masterClock) {
    out_.ticksPerSample = hz > 0 ? masterClock * 3.0 / hz : 0;
}

void Mars::runPwm(uint64_t ticks) {
    while (pwm_.nextTick <= ticks) {
        pwmOutput(pwm_.nextTick);
        const uint32_t clocks = std::max<uint32_t>((pwm_.cycle - 1u) & 0xFFF, 1);
        const bool on = ((pwm_.lmode ^ (pwm_.lmode >> 1)) & 1) || ((pwm_.rmode ^ (pwm_.rmode >> 1)) & 1);
        if (pwm_.cycle != 1 && on) {
            auto pop = [](std::array<uint16_t, 3>& fifo, uint8_t& count, uint16_t& sample) {
                if (!count) return;
                sample = fifo[0];
                fifo[0] = fifo[1];
                fifo[1] = fifo[2];
                --count;
            };
            pop(pwm_.lfifo, pwm_.lcount, pwm_.lsample);
            pop(pwm_.rfifo, pwm_.rcount, pwm_.rsample);
            if (pwm_.periods++ == ((pwm_.timer - 1u) & 15)) {
                pwm_.periods = 0;
                for (int c = 0; c < 2; ++c) irq_[c].active[kPwm] = true;
                updateIrls();
                if (pwm_.dreqIrq) {
                    master_.setDreq(1, true);
                    slave_.setDreq(1, true);
                }
            }
        }
        pwm_.nextTick += static_cast<uint64_t>(clocks) * kTicksPerSh2Cycle;
    }
    pwmOutput(ticks);
}

// Spreads the PWM's present output over the time up to `ticks`, averaging it into
// output samples, and takes the DC offset out (a pulse width of half the cycle is
// silence).
void Mars::pwmOutput(uint64_t ticks) {
    if (ticks <= pwmTicks_) return;
    double dt = static_cast<double>(ticks - pwmTicks_);
    pwmTicks_ = ticks;
    if (out_.ticksPerSample <= 0) return;
    const double clocks = static_cast<double>(std::max<uint32_t>((pwm_.cycle - 1u) & 0xFFF, 1));
    auto level = [&](uint8_t mode) -> double {
        if (mode == 1) return (pwm_.lsample & 0xFFF) / clocks;
        if (mode == 2) return (pwm_.rsample & 0xFFF) / clocks;
        return 0;
    };
    const double l = level(pwm_.lmode);
    const double r = pwm_.rmode == 1 ? (pwm_.rsample & 0xFFF) / clocks
                     : pwm_.rmode == 2 ? (pwm_.lsample & 0xFFF) / clocks
                                       : 0;
    while (dt > 0) {
        const double take = std::min(dt, out_.ticksPerSample - out_.time);
        out_.acc[0] += l * take;
        out_.acc[1] += r * take;
        out_.time += take;
        dt -= take;
        if (out_.time >= out_.ticksPerSample) {
            const double inL = out_.acc[0] / out_.ticksPerSample, inR = out_.acc[1] / out_.ticksPerSample;
            out_.dcOutL = inL - out_.dcInL + 0.998 * out_.dcOutL;
            out_.dcOutR = inR - out_.dcInR + 0.998 * out_.dcOutR;
            out_.dcInL = inL;
            out_.dcInR = inR;
            for (double v : {out_.dcOutL, out_.dcOutR})
                pwmOut_.push_back(static_cast<int16_t>(std::clamp(std::lround(v * 20000.0), -32768L, 32767L)));
            out_.acc = {};
            out_.time = 0;
        }
    }
}

// ---------------------------------------------------------------------------------
// The 68000-to-SH-2 FIFO

void Mars::raiseDreq0() {
    master_.setDreq(0, dreq_.count > 0);
    slave_.setDreq(0, dreq_.count > 0);
}

void Mars::pushFifo(uint16_t value) {
    if (!dreq_.active || dreq_.count >= dreq_.fifo.size()) return;  // lost, as on the real thing
    dreq_.fifo[dreq_.count++] = value;
    if (--dreq_.length == 0) dreq_.active = false;
    raiseDreq0();
}

uint16_t Mars::popFifo() {
    if (!dreq_.count) return 0;
    const uint16_t v = dreq_.fifo[0];
    std::copy(dreq_.fifo.begin() + 1, dreq_.fifo.end(), dreq_.fifo.begin());
    --dreq_.count;
    raiseDreq0();
    return v;
}

// ---------------------------------------------------------------------------------
// VDP

void Mars::selectFramebuffer(int select) {
    vdp_.select = static_cast<uint8_t>(select & 1);
    if (!vdp_.vblank && vdp_.latchMode) return;  // swapped at the next VBlank
    vdp_.active = vdp_.select;
}

bool Mars::framebufferBusy() const {
    return (vdp_.latchMode != 0 && vdp_.select != vdp_.active) || now_ < vdp_.fillEnd;
}

void Mars::fill() {
    vdp_.fillEnd = now_ + static_cast<uint64_t>(7 + 3 * (vdp_.autofillLength + 1)) * kTicksPerSh2Cycle;
    uint16_t* fb = backBuffer();
    for (int i = 0; i <= vdp_.autofillLength; ++i) {
        fb[vdp_.autofillAddress] = vdp_.autofillData;
        vdp_.autofillAddress = static_cast<uint16_t>((vdp_.autofillAddress & 0xFF00) | ((vdp_.autofillAddress + 1) & 0xFF));
    }
}

void Mars::startLine(int line, int activeLines) {
    if (vdp_.hblank) {  // the registers take effect at the end of horizontal blanking
        vdp_.latchMode = vdp_.mode;
        vdp_.latchLines = vdp_.lines;
        vdp_.latchPriority = vdp_.priority;
        vdp_.latchDotshift = vdp_.dotshift;
    }
    vdp_.hblank = false;
    for (auto& q : irq_) q.active[kHint] = false;
    if (line == activeLines) {
        vdp_.vblank = true;
        for (auto& q : irq_) q.active[kVint] = true;
        selectFramebuffer(vdp_.select);
    } else if (line == 0) {
        vdp_.vblank = false;
        for (auto& q : irq_) q.active[kVint] = false;
    }
    updateIrls();
}

void Mars::startHblank(int, int) {
    vdp_.hblank = true;
    for (auto& q : irq_) q.active[kHint] = false;
    if (!vdp_.vblank || io_.hintVblank) {
        if (io_.hcounter++ >= io_.hperiod) {
            io_.hcounter = 0;
            for (auto& q : irq_) q.active[kHint] = true;
        }
    }
    updateIrls();
}

void Mars::renderLine(int line, VideoFrameMd& frame) {
    uint32_t* layer = &frame.mars[static_cast<size_t>(line) * VideoFrameMd::kMaxWidth];
    std::fill(layer, layer + VideoFrameMd::kMaxWidth, 0u);
    if (vdp_.latchMode == 0 || line >= (vdp_.latchLines ? 240 : 224)) return;
    const uint16_t* fb = frontBuffer();
    uint16_t addr = fb[line];
    const int width = std::min(frame.width, VideoFrameMd::kMaxWidth);
    // With widescreen the Mega Drive's picture starts `wideLeft` columns in; the 32X's is
    // only as wide as the console's.
    layer += frame.wideLeft;
    const size_t row = static_cast<size_t>(frame.interlaced ? line * 2 : line) * VideoFrameMd::kMaxWidth + frame.wideLeft;
    auto plot = [&](int x, uint16_t color) {
        if (x + frame.wideLeft >= width) return;
        const uint32_t rgba = colorOf(color);
        layer[x] = rgba;
        MdPixel& info = frame.info[row + static_cast<size_t>(x)];
        const bool through = color & 0x8000;
        const bool backdrop = info.layer == MdLayer::Backdrop;
        const bool shown = vdp_.latchPriority ? (!through || backdrop) : (through || backdrop);
        if (!shown) return;
        frame.rgba[row + static_cast<size_t>(x)] = rgba;
        info = MdPixel{};
        info.layer = MdLayer::Mars;
    };
    switch (vdp_.latchMode) {
        case 1:  // packed pixels: one byte per pixel, through the palette
            for (int x = 0; x < 320; ++x) {
                const int p = x + (vdp_.latchDotshift ? 1 : 0);
                const uint16_t w = fb[static_cast<uint16_t>(addr + (p >> 1))];
                plot(x, cram_[(p & 1) ? (w & 0xFF) : (w >> 8)]);
            }
            break;
        case 2:  // direct colour
            for (int x = 0; x < 320; ++x) plot(x, fb[addr++]);
            break;
        case 3:  // runs: colour index in the low byte, length - 1 in the high byte
            for (int x = 0; x < 320;) {
                const uint16_t w = fb[addr++];
                const uint16_t color = cram_[w & 0xFF];
                for (int n = 0; n <= (w >> 8) && x < 320; ++n) plot(x++, color);
            }
            break;
    }
}

// ---------------------------------------------------------------------------------
// Registers both CPUs see alike (numbered as the SH-2s see them, from 0x4000)

bool Mars::readShared(uint32_t reg, uint16_t& v) {
    auto fifoFlags = [](uint8_t count) { return static_cast<uint16_t>((count == 0 ? 0x4000 : 0) | (count == 3 ? 0x8000 : 0)); };
    switch (reg) {
        case 0x30:
            v = static_cast<uint16_t>(pwm_.lmode | pwm_.rmode << 2 | (pwm_.mono ? 0x10 : 0) | (pwm_.dreqIrq ? 0x80 : 0) |
                                      (pwm_.timer & 15) << 8);
            return true;
        case 0x32: v = pwm_.cycle & 0xFFF; return true;
        case 0x34: v = static_cast<uint16_t>(fifoFlags(pwm_.lcount)); return true;
        case 0x36: v = static_cast<uint16_t>(fifoFlags(pwm_.rcount)); return true;
        case 0x38:
            v = static_cast<uint16_t>(((pwm_.lcount == 0 && pwm_.rcount == 0) ? 0x4000 : 0) |
                                      ((pwm_.lcount == 3 || pwm_.rcount == 3) ? 0x8000 : 0));
            return true;
        case 0x100:
            v = static_cast<uint16_t>(vdp_.mode | (vdp_.lines ? 0x40 : 0) | (vdp_.priority ? 0x80 : 0) | 0x8000);
            return true;
        case 0x102: v = vdp_.dotshift ? 1 : 0; return true;
        case 0x104: v = vdp_.autofillLength; return true;
        case 0x106: v = vdp_.autofillAddress; return true;
        case 0x108: v = vdp_.autofillData; return true;
        case 0x10A: {
            const bool paletteBusy = !vdp_.vblank && !vdp_.hblank && (vdp_.latchMode & 1);
            v = static_cast<uint16_t>(vdp_.active | (framebufferBusy() ? 2 : 0) | (paletteBusy ? 0 : 0x2000) |
                                      (vdp_.hblank ? 0x4000 : 0) | (vdp_.vblank ? 0x8000 : 0));
            return true;
        }
        default:
            if (reg >= 0x200 && reg < 0x400) {
                v = cram_[(reg >> 1) & 0xFF];
                return true;
            }
            return false;
    }
}

void Mars::writeShared(uint32_t reg, uint16_t v, bool upper, bool lower) {
    auto pushPwm = [](std::array<uint16_t, 3>& fifo, uint8_t& count, uint16_t value) {
        if (count == 3) {  // full: the oldest sample goes
            fifo[0] = fifo[1];
            fifo[1] = fifo[2];
            count = 2;
        }
        fifo[count++] = value;
    };
    auto setHigh = [&](uint16_t& r) { r = static_cast<uint16_t>((r & 0x00FF) | (v & 0x0F00)); };
    switch (reg) {
        case 0x32:
            if (lower) pwm_.cycle = static_cast<uint16_t>((pwm_.cycle & 0xF00) | (v & 0xFF));
            if (upper) pwm_.cycle = static_cast<uint16_t>((pwm_.cycle & 0x0FF) | (v & 0xF00));
            break;
        case 0x34:
            if (upper) setHigh(pwm_.llatch);
            if (lower) {
                pwm_.llatch = static_cast<uint16_t>((pwm_.llatch & 0xF00) | (v & 0xFF));
                pushPwm(pwm_.lfifo, pwm_.lcount, pwm_.llatch);
            }
            break;
        case 0x36:
            if (upper) setHigh(pwm_.rlatch);
            if (lower) {
                pwm_.rlatch = static_cast<uint16_t>((pwm_.rlatch & 0xF00) | (v & 0xFF));
                pushPwm(pwm_.rfifo, pwm_.rcount, pwm_.rlatch);
            }
            break;
        case 0x38:
            if (upper) setHigh(pwm_.mlatch);
            if (lower) {
                pwm_.mlatch = static_cast<uint16_t>((pwm_.mlatch & 0xF00) | (v & 0xFF));
                pushPwm(pwm_.lfifo, pwm_.lcount, pwm_.mlatch);
                pushPwm(pwm_.rfifo, pwm_.rcount, pwm_.mlatch);
            }
            break;
        case 0x100:
            if (lower) {
                vdp_.mode = v & 3;
                vdp_.lines = v & 0x40;
                vdp_.priority = v & 0x80;
            }
            break;
        case 0x102:
            if (lower) vdp_.dotshift = v & 1;
            break;
        case 0x104:
            if (lower) vdp_.autofillLength = static_cast<uint8_t>(v);
            break;
        case 0x106:
            if (upper) vdp_.autofillAddress = static_cast<uint16_t>((vdp_.autofillAddress & 0x00FF) | (v & 0xFF00));
            if (lower) vdp_.autofillAddress = static_cast<uint16_t>((vdp_.autofillAddress & 0xFF00) | (v & 0x00FF));
            break;
        case 0x108:
            if (upper) vdp_.autofillData = static_cast<uint16_t>((vdp_.autofillData & 0x00FF) | (v & 0xFF00));
            if (lower) vdp_.autofillData = static_cast<uint16_t>((vdp_.autofillData & 0xFF00) | (v & 0x00FF));
            fill();
            break;
        case 0x10A:
            if (lower) selectFramebuffer(v & 1);
            break;
        default:
            if (reg >= 0x200 && reg < 0x400) {
                uint16_t& c = cram_[(reg >> 1) & 0xFF];
                if (upper) c = static_cast<uint16_t>((c & 0x00FF) | (v & 0xFF00));
                if (lower) c = static_cast<uint16_t>((c & 0xFF00) | (v & 0x00FF));
            }
            break;
    }
}

// ---------------------------------------------------------------------------------
// 68000 side

bool Mars::read68k(uint32_t addr, uint16_t& v) {
    addr &= 0xFFFFFE;
    now_ = masterTicks_;
    if (addr < 0x400000) {
        if (!io_.adapterEnable) return false;
        if (addr < 0x100) {
            if (addr == 0x70) v = static_cast<uint16_t>(io_.vectorLevel4 >> 16);
            else if (addr == 0x72) v = static_cast<uint16_t>(io_.vectorLevel4);
            else v = vectorRom_[addr >> 1];
            return true;
        }
        // RV gives the 68000 the cartridge where it always is. The first 64 KB stay
        // visible anyway: the games' start-up code turns the adapter on while running
        // there, which works on the real thing thanks to the 68000's prefetch.
        if (dreq_.vram || addr < 0x10000) return false;
        v = 0;
        return true;
    }
    if (addr >= 0x800000 && addr < 0xA00000) {
        v = 0;
        if (!io_.adapterEnable) return true;
        if (addr >= 0x840000 && addr < 0x880000) {
            if (!vdp_.access) v = backBuffer()[(addr >> 1) & 0xFFFF];
        } else if (addr >= 0x880000 && addr < 0x900000) {
            if (!dreq_.vram) v = cart_.read16(addr & 0x7FFFF);
        } else if (addr >= 0x900000) {
            if (!dreq_.vram) v = cart_.read16(static_cast<uint32_t>(io_.romBank) << 20 | (addr & 0xFFFFF));
        }
        return true;
    }
    if (addr == 0xA130EC) {
        v = 0x4D41;  // "MA"
        return true;
    }
    if (addr == 0xA130EE) {
        v = 0x5253;  // "RS"
        return true;
    }
    if (addr < 0xA15100 || addr >= 0xA15400) return false;
    const uint32_t reg = addr - 0xA15100;
    switch (reg) {
        case 0x00:
            v = static_cast<uint16_t>((io_.adapterEnable ? 1 : 0) | (io_.adapterReset ? 2 : 0) | 0x80 |
                                      (vdp_.access ? 0x8000 : 0));
            return true;
        case 0x02:
            v = static_cast<uint16_t>((irq_[0].active[kCmd] ? 1 : 0) | (irq_[1].active[kCmd] ? 2 : 0));
            return true;
        case 0x04: v = io_.romBank; return true;
        case 0x06:
            v = static_cast<uint16_t>((dreq_.vram ? 1 : 0) | (dreq_.dma ? 2 : 0) | (dreq_.active ? 4 : 0) |
                                      (dreq_.count >= dreq_.fifo.size() ? 0x80 : 0));
            return true;
        case 0x08: v = static_cast<uint16_t>(dreq_.source >> 16 & 0xFF); return true;
        case 0x0A: v = static_cast<uint16_t>(dreq_.source); return true;
        case 0x0C: v = static_cast<uint16_t>(dreq_.target >> 16 & 0xFF); return true;
        case 0x0E: v = static_cast<uint16_t>(dreq_.target); return true;
        case 0x10: v = dreq_.length; return true;
        case 0x1A: v = io_.cartridgeMode ? 1 : 0; return true;
        default: break;
    }
    if (reg >= 0x20 && reg < 0x30) {
        v = comm_[(reg >> 1) & 7];
        return true;
    }
    v = 0;
    if (reg >= 0x30 && reg < 0x3A) {
        readShared(reg, v);
        return true;
    }
    // The VDP and the palette, unless the SH-2s have them.
    if (reg >= 0x80 && reg < 0x8C && !vdp_.access) readShared(reg + 0x80, v);
    if (reg >= 0x100 && reg < 0x300 && !vdp_.access) readShared(reg + 0x100, v);
    return true;
}

bool Mars::write68k(uint32_t addr, uint16_t v, bool upper, bool lower) {
    addr &= 0xFFFFFE;
    now_ = masterTicks_;
    if (addr < 0x400000) {
        if (!io_.adapterEnable) return false;
        if (addr < 0x100) {
            uint32_t& vec = io_.vectorLevel4;
            if (addr == 0x70) {
                if (upper) vec = (vec & 0x00FFFFFF) | static_cast<uint32_t>(v & 0xFF00) << 16;
                if (lower) vec = (vec & 0xFF00FFFF) | static_cast<uint32_t>(v & 0x00FF) << 16;
            } else if (addr == 0x72) {
                if (upper) vec = (vec & 0xFFFF00FF) | (v & 0xFF00);
                if (lower) vec = (vec & 0xFFFFFF00) | (v & 0x00FF);
            }
            return true;
        }
        return !dreq_.vram && addr >= 0x10000;
    }
    if (addr >= 0x800000 && addr < 0xA00000) {
        if (!io_.adapterEnable || vdp_.access || addr < 0x840000 || addr >= 0x880000) return true;
        uint16_t& w = backBuffer()[(addr >> 1) & 0xFFFF];
        if (addr < 0x860000) {
            if (v == 0 && (!upper || !lower)) return true;  // byte writes of 0 do not go through
            if (upper) w = static_cast<uint16_t>((w & 0x00FF) | (v & 0xFF00));
            if (lower) w = static_cast<uint16_t>((w & 0xFF00) | (v & 0x00FF));
        } else {  // overwrite image: zero bytes leave the pixel alone
            if (upper && (v & 0xFF00)) w = static_cast<uint16_t>((w & 0x00FF) | (v & 0xFF00));
            if (lower && (v & 0x00FF)) w = static_cast<uint16_t>((w & 0xFF00) | (v & 0x00FF));
        }
        return true;
    }
    if (addr < 0xA15100 || addr >= 0xA15400) return false;
    const uint32_t reg = addr - 0xA15100;
    switch (reg) {
        case 0x00:
            if (lower) {
                io_.adapterEnable = v & 1;
                const bool run = v & 2;
                if (run && !io_.adapterReset) {
                    io_.adapterReset = true;
                    bootSh2s();
                }
                io_.adapterReset = run;
            }
            if (upper) vdp_.access = v & 0x8000;
            return true;
        case 0x02:
            if (lower) {
                irq_[0].active[kCmd] = v & 1;
                irq_[1].active[kCmd] = v & 2;
                updateIrls();
            }
            return true;
        case 0x04:
            if (lower) io_.romBank = v & 3;
            return true;
        case 0x06:
            if (lower) {
                dreq_.vram = v & 1;
                dreq_.dma = v & 2;
                const bool start = v & 4;
                if (dreq_.active && !start) {
                    dreq_.count = 0;
                    raiseDreq0();
                }
                dreq_.active = start;
            }
            return true;
        case 0x08:
            if (lower) dreq_.source = (dreq_.source & 0x00FFFF) | static_cast<uint32_t>(v & 0xFF) << 16;
            return true;
        case 0x0A:
            if (upper) dreq_.source = (dreq_.source & 0xFF00FF) | (v & 0xFF00);
            if (lower) dreq_.source = (dreq_.source & 0xFFFF00) | (v & 0x00FF);
            return true;
        case 0x0C:
            if (lower) dreq_.target = (dreq_.target & 0x00FFFF) | static_cast<uint32_t>(v & 0xFF) << 16;
            return true;
        case 0x0E:
            if (upper) dreq_.target = (dreq_.target & 0xFF00FF) | (v & 0xFF00);
            if (lower) dreq_.target = (dreq_.target & 0xFFFF00) | (v & 0x00FF);
            return true;
        case 0x10:
            if (upper) dreq_.length = static_cast<uint16_t>((dreq_.length & 0x00FF) | (v & 0xFF00));
            if (lower) dreq_.length = static_cast<uint16_t>((dreq_.length & 0xFF00) | (v & 0x00FC));
            return true;
        case 0x12:
            pushFifo(v);
            return true;
        case 0x1A:
            if (lower) io_.cartridgeMode = v & 1;
            return true;
        default: break;
    }
    if (reg >= 0x20 && reg < 0x30) {
        uint16_t& c = comm_[(reg >> 1) & 7];
        if (upper) c = static_cast<uint16_t>((c & 0x00FF) | (v & 0xFF00));
        if (lower) c = static_cast<uint16_t>((c & 0xFF00) | (v & 0x00FF));
        return true;
    }
    if (reg == 0x30) {  // the 68000 cannot set the timer or the DMA request
        if (lower) {
            pwm_.lmode = v & 3;
            pwm_.rmode = (v >> 2) & 3;
            pwm_.mono = v & 0x10;
        }
        return true;
    }
    if (reg >= 0x32 && reg < 0x3A) {
        runPwm(now_);
        writeShared(reg, v, upper, lower);
        return true;
    }
    if (reg >= 0x80 && reg < 0x8C && !vdp_.access) writeShared(reg + 0x80, v, upper, lower);
    if (reg >= 0x100 && reg < 0x300 && !vdp_.access) writeShared(reg + 0x100, v, upper, lower);
    return true;
}

// ---------------------------------------------------------------------------------
// SH-2 side

uint32_t Mars::sh2Read(Sh2& cpu, uint32_t addr, int size) {
    if ((addr & 0xFFFC0000) == 0x06000000) {  // SDRAM, the common case
        const uint32_t a = addr & 0x3FFFF;
        uint32_t v = 0;
        for (int i = 0; i < size; ++i) v = v << 8 | sdram_[(a + static_cast<uint32_t>(i)) & 0x3FFFF];
        return v;
    }
    if (size == 4) return static_cast<uint32_t>(sh2Read16(cpu, addr)) << 16 | sh2Read16(cpu, addr + 2);
    const uint16_t w = sh2Read16(cpu, addr & ~1u);
    if (size == 2) return w;
    return (addr & 1) ? (w & 0xFF) : (w >> 8);
}

void Mars::sh2Write(Sh2& cpu, uint32_t addr, uint32_t value, int size) {
    if ((addr & 0xFFFC0000) == 0x06000000) {
        const uint32_t a = addr & 0x3FFFF;
        for (int i = 0; i < size; ++i)
            sdram_[(a + static_cast<uint32_t>(i)) & 0x3FFFF] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
        return;
    }
    if (size == 4) {
        sh2Write16(cpu, addr, static_cast<uint16_t>(value >> 16), true, true);
        sh2Write16(cpu, addr + 2, static_cast<uint16_t>(value), true, true);
    } else if (size == 2) {
        sh2Write16(cpu, addr, static_cast<uint16_t>(value), true, true);
    } else {
        const uint16_t b = static_cast<uint16_t>(value & 0xFF);
        sh2Write16(cpu, addr & ~1u, static_cast<uint16_t>(b << 8 | b), !(addr & 1), (addr & 1) != 0);
    }
}

uint16_t Mars::sh2Read16(Sh2& cpu, uint32_t addr) {
    if (addr < 0x4000) {
        const auto& rom = bootRom_[cpu.id()];
        const uint32_t a = addr & 0x3FE;
        return static_cast<uint16_t>(rom[a] << 8 | rom[a + 1]);
    }
    if (addr < 0x4400) {
        cpu.addCycles(1);
        return readSysReg(cpu, addr);
    }
    if (addr >= 0x02000000 && addr < 0x04000000) {
        cpu.addCycles(6);
        return cart_.read16(addr & 0x3FFFFE);
    }
    if (addr >= 0x04000000 && addr < 0x06000000) {
        if (!vdp_.access) return 0;
        cpu.addCycles(5);
        return backBuffer()[(addr >> 1) & 0xFFFF];
    }
    return 0;
}

void Mars::sh2Write16(Sh2& cpu, uint32_t addr, uint16_t v, bool upper, bool lower) {
    if (addr >= 0x4000 && addr < 0x4400) {
        cpu.addCycles(1);
        writeSysReg(cpu, addr, v, upper, lower);
        return;
    }
    if (addr >= 0x04000000 && addr < 0x06000000) {
        if (!vdp_.access) return;
        cpu.addCycles(4);
        const uint32_t a = addr & 0x3FFFF;
        uint16_t& w = backBuffer()[(a >> 1) & 0xFFFF];
        if (a < 0x20000) {
            if (v == 0 && (!upper || !lower)) return;
            if (upper) w = static_cast<uint16_t>((w & 0x00FF) | (v & 0xFF00));
            if (lower) w = static_cast<uint16_t>((w & 0xFF00) | (v & 0x00FF));
        } else {
            if (upper && (v & 0xFF00)) w = static_cast<uint16_t>((w & 0x00FF) | (v & 0xFF00));
            if (lower && (v & 0x00FF)) w = static_cast<uint16_t>((w & 0xFF00) | (v & 0x00FF));
        }
    }
}

uint16_t Mars::readSysReg(Sh2& cpu, uint32_t addr) {
    const uint32_t reg = addr & 0x3FE;
    const Irq& q = irq_[cpu.id()];
    switch (reg) {
        case 0x00:
            return static_cast<uint16_t>((q.enable[kPwm] ? 1 : 0) | (q.enable[kCmd] ? 2 : 0) | (q.enable[kHint] ? 4 : 0) |
                                         (q.enable[kVint] ? 8 : 0) | (io_.hintVblank ? 0x80 : 0) |
                                         (io_.adapterEnable ? 0x200 : 0) | (vdp_.access ? 0x8000 : 0));
        case 0x04: return io_.hperiod;
        case 0x06:
            return static_cast<uint16_t>((dreq_.vram ? 1 : 0) | (dreq_.dma ? 2 : 0) | (dreq_.active ? 4 : 0) |
                                         (dreq_.count == 0 ? 0x4000 : 0) |
                                         (dreq_.count >= dreq_.fifo.size() ? 0x8000 : 0));
        case 0x08: return static_cast<uint16_t>(dreq_.source >> 16 & 0xFF);
        case 0x0A: return static_cast<uint16_t>(dreq_.source);
        case 0x0C: return static_cast<uint16_t>(dreq_.target >> 16 & 0xFF);
        case 0x0E: return static_cast<uint16_t>(dreq_.target);
        case 0x10: return dreq_.length;
        case 0x12: return popFifo();
        default: break;
    }
    if (reg >= 0x20 && reg < 0x30) return comm_[(reg >> 1) & 7];
    if (reg >= 0x30 && reg < 0x3A) {
        runPwm(now_);
        uint16_t v = 0;
        readShared(reg, v);
        return v;
    }
    if (reg >= 0x100 && !vdp_.access) return 0;
    uint16_t v = 0;
    readShared(reg, v);
    return v;
}

void Mars::writeSysReg(Sh2& cpu, uint32_t addr, uint16_t v, bool upper, bool lower) {
    const uint32_t reg = addr & 0x3FE;
    const int c = cpu.id();
    Irq& q = irq_[c];
    switch (reg) {
        case 0x00:
            if (lower) {
                q.enable[kPwm] = v & 1;
                q.enable[kCmd] = v & 2;
                q.enable[kHint] = v & 4;
                q.enable[kVint] = v & 8;
                io_.hintVblank = v & 0x80;
                updateIrl(c);
            }
            if (upper) vdp_.access = v & 0x8000;
            return;
        case 0x04:
            if (lower) {
                io_.hperiod = static_cast<uint8_t>(v);
                io_.hcounter = 0;
            }
            return;
        case 0x14: q.active[kVres] = false; updateIrl(c); return;
        case 0x16: q.active[kVint] = false; updateIrl(c); return;
        case 0x18: q.active[kHint] = false; updateIrl(c); return;
        case 0x1A: q.active[kCmd] = false; updateIrl(c); return;
        case 0x1C: q.active[kPwm] = false; updateIrl(c); return;
        default: break;
    }
    if (reg >= 0x20 && reg < 0x30) {
        uint16_t& w = comm_[(reg >> 1) & 7];
        if (upper) w = static_cast<uint16_t>((w & 0x00FF) | (v & 0xFF00));
        if (lower) w = static_cast<uint16_t>((w & 0xFF00) | (v & 0x00FF));
        return;
    }
    if (reg == 0x30) {
        runPwm(now_);
        if (lower) {
            pwm_.lmode = v & 3;
            pwm_.rmode = (v >> 2) & 3;
            pwm_.mono = v & 0x10;
            pwm_.dreqIrq = v & 0x80;
            if (!pwm_.dreqIrq) {
                master_.setDreq(1, false);
                slave_.setDreq(1, false);
            }
        }
        if (upper) {
            pwm_.timer = static_cast<uint8_t>((v >> 8) & 15);
            pwm_.periods = 0;
        }
        return;
    }
    if (reg >= 0x32 && reg < 0x3A) {
        runPwm(now_);
        writeShared(reg, v, upper, lower);
        return;
    }
    if (reg >= 0x100 && !vdp_.access) return;
    writeShared(reg, v, upper, lower);
}

}  // namespace plumsg
