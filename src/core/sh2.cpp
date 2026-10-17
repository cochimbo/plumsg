#include "sh2.h"

namespace plumsg {

namespace {

constexpr uint32_t kSrMask = 0x3F3;  // M, Q, I3-I0, S, T

int32_t s8(uint32_t v) { return static_cast<int8_t>(v); }
int32_t s16(uint32_t v) { return static_cast<int16_t>(v); }

}  // namespace

// ---------------------------------------------------------------------------------
// Memory as the CPU sees it

uint32_t Sh2::read(uint32_t addr, int size) {
    switch (addr >> 29) {
        case 0: case 1: case 4: case 5:
            return bus_.sh2Read(*this, addr & 0x1FFFFFFF & ~static_cast<uint32_t>(size - 1), size);
        case 6: {  // the cache's data array, as RAM
            uint32_t v = 0;
            const uint32_t a = addr & 0xFFF & ~static_cast<uint32_t>(size - 1);
            for (int i = 0; i < size; ++i) v = v << 8 | dataArray_[(a + static_cast<uint32_t>(i)) & 0xFFF];
            return v;
        }
        case 7: {
            if (addr < 0xFFFFFE00) return 0;
            uint32_t v = 0;
            const uint32_t a = addr & ~static_cast<uint32_t>(size - 1);
            for (int i = 0; i < size; ++i) v = v << 8 | readIo(a + static_cast<uint32_t>(i));
            return v;
        }
        default: return 0;  // cache purge and address array areas
    }
}

void Sh2::write(uint32_t addr, uint32_t value, int size) {
    switch (addr >> 29) {
        case 0: case 1: case 4: case 5:
            bus_.sh2Write(*this, addr & 0x1FFFFFFF & ~static_cast<uint32_t>(size - 1), value, size);
            return;
        case 6: {
            const uint32_t a = addr & 0xFFF & ~static_cast<uint32_t>(size - 1);
            for (int i = 0; i < size; ++i)
                dataArray_[(a + static_cast<uint32_t>(i)) & 0xFFF] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
            return;
        }
        case 7: {
            if (addr < 0xFFFFFE00) return;
            const uint32_t a = addr & ~static_cast<uint32_t>(size - 1);
            for (int i = 0; i < size; ++i) writeIo(a + static_cast<uint32_t>(i), static_cast<uint8_t>(value >> (8 * (size - 1 - i))));
            return;
        }
        default: return;  // purges and the address array: the cache is not simulated
    }
}

uint32_t Sh2::fetch(uint32_t addr) { return read(addr, 2); }

// ---------------------------------------------------------------------------------
// Reset, interrupts and the main step

void Sh2::reset() {
    r_.fill(0);
    pr_ = gbr_ = vbr_ = mach_ = macl_ = 0;
    sr_ = 0xF0;
    sleeping_ = noIrqNext_ = inSlot_ = false;
    irl_ = 0;
    dreq_ = {};
    frt_ = Frt{};
    wdt_ = Wdt{};
    divu_ = Divu{};
    dmac_ = Dmac{};
    sci_ = Sci{};
    intc_ = Intc{};
    ccr_ = sbycr_ = 0;
    bsc_.fill(0);
    pc_ = read(0, 4);
    r_[15] = read(4, 4);
}

void Sh2::exception(uint32_t vector, uint32_t returnPc) {
    r_[15] -= 4;
    write(r_[15], sr_, 4);
    r_[15] -= 4;
    write(r_[15], returnPc, 4);
    pc_ = read(vbr_ + vector * 4, 4);
}

// Picks the highest-priority interrupt above the mask, if any, and takes it. Sources
// at the same level go in the order of the manual: IRL, DIVU, DMAC 0 and 1, WDT, SCI,
// FRT.
bool Sh2::takeInterrupt() {
    int level = 0;
    uint32_t vector = 0;
    auto offer = [&](int l, uint32_t v) {
        if (l > level) {
            level = l;
            vector = v;
        }
    };
    if (irl_ > 0) offer(irl_, 64 + static_cast<uint32_t>(irl_ >> 1));
    if ((divu_.dvcr & 3) == 3) offer((intc_.ipra >> 12) & 15, divu_.vcrdiv & 0x7F);
    for (int c = 0; c < 2; ++c)
        if ((dmac_.chcr[c] & 6) == 6) offer((intc_.ipra >> 8) & 15, dmac_.vcrdma[c] & 0x7F);
    if ((wdt_.wtcsr & 0xC0) == 0x80 && (wdt_.wtcsr & 0x20)) offer((intc_.ipra >> 4) & 15, (intc_.vcrwdt >> 8) & 0x7F);
    {
        const int l = (intc_.iprb >> 12) & 15;
        if ((sci_.scr & 0x40) && (sci_.ssr & 0x40)) offer(l, intc_.vcra & 0x7F);         // receive full
        if ((sci_.scr & 0x80) && (sci_.ssr & 0x80)) offer(l, (intc_.vcrb >> 8) & 0x7F);  // transmit empty
    }
    {
        const int l = (intc_.iprb >> 8) & 15;
        if ((frt_.tier & 0x80) && (frt_.ftcsr & 0x80)) offer(l, (intc_.vcrc >> 8) & 0x7F);  // input capture
        if (((frt_.tier & 0x08) && (frt_.ftcsr & 0x08)) || ((frt_.tier & 0x04) && (frt_.ftcsr & 0x04)))
            offer(l, intc_.vcrc & 0x7F);  // output compare
        if ((frt_.tier & 0x02) && (frt_.ftcsr & 0x02)) offer(l, (intc_.vcrd >> 8) & 0x7F);  // overflow
    }
    if (level <= imask()) return false;
    sleeping_ = false;
    exception(vector, pc_);
    sr_ = (sr_ & ~0xF0u) | static_cast<uint32_t>(level) << 4;
    return true;
}

int Sh2::step() {
    extra_ = 0;
    int cycles = 1;
    if (runDma()) return 1 + extra_;
    if (!noIrqNext_ && takeInterrupt()) {
        cycles = 13;
    } else if (sleeping_) {
        cycles = 4;
    } else {
        noIrqNext_ = false;
        const uint16_t op = static_cast<uint16_t>(fetch(pc_));
        execute(op);
    }
    cycles += extra_;
    runTimers(cycles);
    if (!(sci_.ssr & 0x80)) sciTransfer();
    return cycles;
}

void Sh2::delaySlot(uint32_t target) {
    inSlot_ = true;
    slotTarget_ = target;
    const uint32_t slot = pc_ + 2;
    pc_ = slot;
    const uint16_t op = static_cast<uint16_t>(fetch(slot));
    // A branch in a delay slot is an illegal slot instruction; it is treated as one
    // that does nothing.
    const bool branch = (op & 0xF000) == 0xA000 || (op & 0xF000) == 0xB000 || (op & 0xFF00) == 0x8900 ||
                        (op & 0xFF00) == 0x8B00 || (op & 0xFF00) == 0x8D00 || (op & 0xFF00) == 0x8F00 ||
                        (op & 0xF0FF) == 0x0003 || (op & 0xF0FF) == 0x0023 || (op & 0xF0FF) == 0x400B ||
                        (op & 0xF0FF) == 0x402B || op == 0x000B || op == 0x002B || (op & 0xFF00) == 0xC300;
    if (!branch) execute(op);
    inSlot_ = false;
    pc_ = target;
    extra_ += 1;
}

// ---------------------------------------------------------------------------------
// Instructions

void Sh2::execute(uint16_t op) {
    const uint32_t n = (op >> 8) & 15, m = (op >> 4) & 15;
    uint32_t& Rn = r_[n];
    const uint32_t Rm = r_[m];
    uint32_t next = pc_ + 2;
    auto readB = [&](uint32_t a) { return read(a, 1); };
    auto readW = [&](uint32_t a) { return read(a, 2); };
    auto readL = [&](uint32_t a) { return read(a, 4); };
    auto writeB = [&](uint32_t a, uint32_t v) { write(a, v & 0xFF, 1); };
    auto writeW = [&](uint32_t a, uint32_t v) { write(a, v & 0xFFFF, 2); };
    auto writeL = [&](uint32_t a, uint32_t v) { write(a, v, 4); };
    auto illegal = [&] {
        exception(inSlot_ ? 6 : 4, pc_);
        next = pc_;
    };

    switch (op >> 12) {
        case 0x0:
            switch (op & 0xF) {
                case 0x2:
                    switch (m) {
                        case 0: Rn = sr_; break;           // STC SR,Rn
                        case 1: Rn = gbr_; break;          // STC GBR,Rn
                        case 2: Rn = vbr_; break;          // STC VBR,Rn
                        default: illegal(); break;
                    }
                    noIrqNext_ = true;
                    break;
                case 0x3:
                    if (m == 0) {  // BSRF Rm
                        pr_ = pc_ + 4;
                        delaySlot(pc_ + 4 + Rn);
                        extra_ += 1;
                        return;
                    }
                    if (m == 2) {  // BRAF Rm
                        delaySlot(pc_ + 4 + Rn);
                        extra_ += 1;
                        return;
                    }
                    illegal();
                    break;
                case 0x4: writeB(Rn + r_[0], Rm); break;  // MOV.B Rm,@(R0,Rn)
                case 0x5: writeW(Rn + r_[0], Rm); break;  // MOV.W Rm,@(R0,Rn)
                case 0x6: writeL(Rn + r_[0], Rm); break;  // MOV.L Rm,@(R0,Rn)
                case 0x7: macl_ = Rn * Rm; extra_ += 1; break;  // MUL.L Rm,Rn
                case 0x8:
                    if (op == 0x0008) setT(false);                    // CLRT
                    else if (op == 0x0018) setT(true);                // SETT
                    else if (op == 0x0028) mach_ = macl_ = 0;         // CLRMAC
                    else illegal();
                    break;
                case 0x9:
                    if (op == 0x0009) break;  // NOP
                    if (op == 0x0019) {       // DIV0U
                        sr_ &= ~0x301u;
                        break;
                    }
                    if ((op & 0xFF) == 0x29) {  // MOVT Rn
                        Rn = t();
                        break;
                    }
                    illegal();
                    break;
                case 0xA:
                    switch (m) {
                        case 0: Rn = mach_; break;  // STS MACH,Rn
                        case 1: Rn = macl_; break;  // STS MACL,Rn
                        case 2: Rn = pr_; break;    // STS PR,Rn
                        default: illegal(); break;
                    }
                    noIrqNext_ = true;
                    break;
                case 0xB:
                    if (op == 0x000B) {  // RTS
                        delaySlot(pr_);
                        extra_ += 1;
                        return;
                    }
                    if (op == 0x001B) {  // SLEEP
                        sleeping_ = true;
                        extra_ += 2;
                        break;
                    }
                    if (op == 0x002B) {  // RTE
                        const uint32_t target = readL(r_[15]);
                        sr_ = readL(r_[15] + 4) & kSrMask;
                        r_[15] += 8;
                        delaySlot(target);
                        extra_ += 2;
                        return;
                    }
                    illegal();
                    break;
                case 0xC: Rn = static_cast<uint32_t>(s8(readB(Rm + r_[0]))); break;   // MOV.B @(R0,Rm),Rn
                case 0xD: Rn = static_cast<uint32_t>(s16(readW(Rm + r_[0]))); break;  // MOV.W @(R0,Rm),Rn
                case 0xE: Rn = readL(Rm + r_[0]); break;                              // MOV.L @(R0,Rm),Rn
                case 0xF: {  // MAC.L @Rm+,@Rn+
                    const int64_t a = static_cast<int32_t>(readL(Rn));
                    Rn += 4;
                    const int64_t b = static_cast<int32_t>(readL(r_[m]));
                    r_[m] += 4;
                    int64_t mac = static_cast<int64_t>(static_cast<uint64_t>(mach_) << 32 | macl_);
                    mac += a * b;
                    if (sr_ & 2) {  // S: saturate to 48 bits
                        const int64_t lim = (int64_t{1} << 47);
                        if (mac >= lim) mac = lim - 1;
                        if (mac < -lim) mac = -lim;
                    }
                    mach_ = static_cast<uint32_t>(static_cast<uint64_t>(mac) >> 32);
                    macl_ = static_cast<uint32_t>(mac);
                    extra_ += 2;
                    break;
                }
                default: illegal(); break;
            }
            break;

        case 0x1: writeL(Rn + (op & 15) * 4, Rm); break;  // MOV.L Rm,@(disp,Rn)

        case 0x2:
            switch (op & 0xF) {
                case 0x0: writeB(Rn, Rm); break;
                case 0x1: writeW(Rn, Rm); break;
                case 0x2: writeL(Rn, Rm); break;
                case 0x4: writeB(Rn - 1, Rm); Rn -= 1; break;  // MOV.B Rm,@-Rn
                case 0x5: writeW(Rn - 2, Rm); Rn -= 2; break;
                case 0x6: writeL(Rn - 4, Rm); Rn -= 4; break;
                case 0x7: {  // DIV0S Rm,Rn
                    const uint32_t q = Rn >> 31, mm = Rm >> 31;
                    sr_ = (sr_ & ~0x301u) | q << 8 | mm << 9 | (q ^ mm);
                    break;
                }
                case 0x8: setT((Rn & Rm) == 0); break;  // TST
                case 0x9: Rn &= Rm; break;
                case 0xA: Rn ^= Rm; break;
                case 0xB: Rn |= Rm; break;
                case 0xC: {  // CMP/STR
                    const uint32_t x = Rn ^ Rm;
                    setT(!(x & 0xFF000000) || !(x & 0x00FF0000) || !(x & 0x0000FF00) || !(x & 0x000000FF));
                    break;
                }
                case 0xD: Rn = (Rn >> 16) | (Rm << 16); break;  // XTRCT
                case 0xE: macl_ = (Rn & 0xFFFF) * (Rm & 0xFFFF); break;  // MULU.W
                case 0xF: macl_ = static_cast<uint32_t>(s16(Rn) * s16(Rm)); break;  // MULS.W
                default: illegal(); break;
            }
            break;

        case 0x3:
            switch (op & 0xF) {
                case 0x0: setT(Rn == Rm); break;
                case 0x2: setT(Rn >= Rm); break;
                case 0x3: setT(static_cast<int32_t>(Rn) >= static_cast<int32_t>(Rm)); break;
                case 0x4: {  // DIV1 Rm,Rn
                    const bool oldQ = (sr_ >> 8) & 1;
                    const bool M = (sr_ >> 9) & 1;
                    bool Q = Rn >> 31;
                    const uint32_t before = (Rn << 1) | t();
                    uint32_t after;
                    bool carry;
                    if (oldQ == M) {
                        after = before - Rm;
                        carry = after > before;  // borrow
                    } else {
                        after = before + Rm;
                        carry = after < before;
                    }
                    Rn = after;
                    Q = Q ^ M ^ carry;
                    sr_ = (sr_ & ~0x101u) | (Q ? 0x100u : 0u) | ((Q == M) ? 1u : 0u);
                    break;
                }
                case 0x5: {  // DMULU.L
                    const uint64_t r = static_cast<uint64_t>(Rn) * Rm;
                    mach_ = static_cast<uint32_t>(r >> 32);
                    macl_ = static_cast<uint32_t>(r);
                    extra_ += 1;
                    break;
                }
                case 0x6: setT(Rn > Rm); break;
                case 0x7: setT(static_cast<int32_t>(Rn) > static_cast<int32_t>(Rm)); break;
                case 0x8: Rn -= Rm; break;
                case 0xA: {  // SUBC
                    const uint32_t a = Rn, b = a - Rm, c = b - t();
                    Rn = c;
                    setT(a < b || b < c);
                    break;
                }
                case 0xB: {  // SUBV
                    const int64_t r = static_cast<int64_t>(static_cast<int32_t>(Rn)) - static_cast<int32_t>(Rm);
                    Rn = static_cast<uint32_t>(r);
                    setT(r > INT32_MAX || r < INT32_MIN);
                    break;
                }
                case 0xC: Rn += Rm; break;
                case 0xD: {  // DMULS.L
                    const int64_t r = static_cast<int64_t>(static_cast<int32_t>(Rn)) * static_cast<int32_t>(Rm);
                    mach_ = static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
                    macl_ = static_cast<uint32_t>(r);
                    extra_ += 1;
                    break;
                }
                case 0xE: {  // ADDC
                    const uint32_t a = Rn, b = a + Rm, c = b + t();
                    Rn = c;
                    setT(b < a || c < b);
                    break;
                }
                case 0xF: {  // ADDV
                    const int64_t r = static_cast<int64_t>(static_cast<int32_t>(Rn)) + static_cast<int32_t>(Rm);
                    Rn = static_cast<uint32_t>(r);
                    setT(r > INT32_MAX || r < INT32_MIN);
                    break;
                }
                default: illegal(); break;
            }
            break;

        case 0x4:
            switch (op & 0xFF) {
                case 0x00: setT(Rn >> 31); Rn <<= 1; break;  // SHLL
                case 0x01: setT(Rn & 1); Rn >>= 1; break;    // SHLR
                case 0x02: Rn -= 4; writeL(Rn, mach_); noIrqNext_ = true; break;  // STS.L MACH,@-Rn
                case 0x03: Rn -= 4; writeL(Rn, sr_); noIrqNext_ = true; break;    // STC.L SR,@-Rn
                case 0x04: setT(Rn >> 31); Rn = (Rn << 1) | (Rn >> 31); break;    // ROTL
                case 0x05: setT(Rn & 1); Rn = (Rn >> 1) | (Rn << 31); break;      // ROTR
                case 0x06: mach_ = readL(Rn); Rn += 4; noIrqNext_ = true; break;  // LDS.L @Rm+,MACH
                case 0x07: sr_ = readL(Rn) & kSrMask; Rn += 4; noIrqNext_ = true; extra_ += 2; break;
                case 0x08: Rn <<= 2; break;
                case 0x09: Rn >>= 2; break;
                case 0x0A: mach_ = Rn; noIrqNext_ = true; break;
                case 0x0B: {  // JSR @Rm
                    pr_ = pc_ + 4;
                    delaySlot(Rn);
                    extra_ += 1;
                    return;
                }
                case 0x0E: sr_ = Rn & kSrMask; noIrqNext_ = true; break;  // LDC Rm,SR
                case 0x10: Rn -= 1; setT(Rn == 0); break;                   // DT
                case 0x11: setT(static_cast<int32_t>(Rn) >= 0); break;      // CMP/PZ
                case 0x12: Rn -= 4; writeL(Rn, macl_); noIrqNext_ = true; break;
                case 0x13: Rn -= 4; writeL(Rn, gbr_); noIrqNext_ = true; break;
                case 0x15: setT(static_cast<int32_t>(Rn) > 0); break;  // CMP/PL
                case 0x16: macl_ = readL(Rn); Rn += 4; noIrqNext_ = true; break;
                case 0x17: gbr_ = readL(Rn); Rn += 4; noIrqNext_ = true; extra_ += 2; break;
                case 0x18: Rn <<= 8; break;
                case 0x19: Rn >>= 8; break;
                case 0x1A: macl_ = Rn; noIrqNext_ = true; break;
                case 0x1B: {  // TAS.B @Rn
                    const uint32_t v = readB(Rn);
                    setT(v == 0);
                    writeB(Rn, v | 0x80);
                    extra_ += 3;
                    break;
                }
                case 0x1E: gbr_ = Rn; noIrqNext_ = true; break;
                case 0x20: setT(Rn >> 31); Rn <<= 1; break;  // SHAL
                case 0x21: setT(Rn & 1); Rn = static_cast<uint32_t>(static_cast<int32_t>(Rn) >> 1); break;  // SHAR
                case 0x22: Rn -= 4; writeL(Rn, pr_); noIrqNext_ = true; break;
                case 0x23: Rn -= 4; writeL(Rn, vbr_); noIrqNext_ = true; break;
                case 0x24: {  // ROTCL
                    const bool c = Rn >> 31;
                    Rn = (Rn << 1) | t();
                    setT(c);
                    break;
                }
                case 0x25: {  // ROTCR
                    const bool c = Rn & 1;
                    Rn = (Rn >> 1) | (t() ? 0x80000000u : 0u);
                    setT(c);
                    break;
                }
                case 0x26: pr_ = readL(Rn); Rn += 4; noIrqNext_ = true; break;
                case 0x27: vbr_ = readL(Rn); Rn += 4; noIrqNext_ = true; extra_ += 2; break;
                case 0x28: Rn <<= 16; break;
                case 0x29: Rn >>= 16; break;
                case 0x2A: pr_ = Rn; noIrqNext_ = true; break;
                case 0x2B: delaySlot(Rn); extra_ += 1; return;  // JMP @Rm
                case 0x2E: vbr_ = Rn; noIrqNext_ = true; break;
                default:
                    if ((op & 0xF) == 0xF) {  // MAC.W @Rm+,@Rn+
                        const int32_t a = s16(readW(Rn));
                        Rn += 2;
                        const int32_t b = s16(readW(r_[m]));
                        r_[m] += 2;
                        const int64_t product = static_cast<int64_t>(a) * b;
                        if (sr_ & 2) {  // S: 32-bit saturation, MACH bit 0 flags overflow
                            int64_t v = static_cast<int64_t>(static_cast<int32_t>(macl_)) + product;
                            if (v > INT32_MAX) {
                                v = INT32_MAX;
                                mach_ |= 1;
                            } else if (v < INT32_MIN) {
                                v = INT32_MIN;
                                mach_ |= 1;
                            }
                            macl_ = static_cast<uint32_t>(v);
                        } else {
                            int64_t mac = static_cast<int64_t>(static_cast<uint64_t>(mach_) << 32 | macl_) + product;
                            mach_ = static_cast<uint32_t>(static_cast<uint64_t>(mac) >> 32);
                            macl_ = static_cast<uint32_t>(mac);
                        }
                        extra_ += 1;
                    } else {
                        illegal();
                    }
                    break;
            }
            break;

        case 0x5: Rn = readL(Rm + (op & 15) * 4); break;  // MOV.L @(disp,Rm),Rn

        case 0x6:
            switch (op & 0xF) {
                case 0x0: Rn = static_cast<uint32_t>(s8(readB(Rm))); break;
                case 0x1: Rn = static_cast<uint32_t>(s16(readW(Rm))); break;
                case 0x2: Rn = readL(Rm); break;
                case 0x3: Rn = Rm; break;
                case 0x4: {
                    const uint32_t v = static_cast<uint32_t>(s8(readB(Rm)));
                    if (n != m) r_[m] += 1;
                    Rn = v;
                    break;
                }
                case 0x5: {
                    const uint32_t v = static_cast<uint32_t>(s16(readW(Rm)));
                    if (n != m) r_[m] += 2;
                    Rn = v;
                    break;
                }
                case 0x6: {
                    const uint32_t v = readL(Rm);
                    if (n != m) r_[m] += 4;
                    Rn = v;
                    break;
                }
                case 0x7: Rn = ~Rm; break;
                case 0x8: Rn = (Rm & 0xFFFF0000) | ((Rm >> 8) & 0xFF) | ((Rm << 8) & 0xFF00); break;  // SWAP.B
                case 0x9: Rn = (Rm << 16) | (Rm >> 16); break;                                        // SWAP.W
                case 0xA: {  // NEGC
                    const uint32_t a = 0 - Rm, c = a - t();
                    Rn = c;
                    setT(0 < a || a < c);
                    break;
                }
                case 0xB: Rn = 0 - Rm; break;
                case 0xC: Rn = Rm & 0xFF; break;
                case 0xD: Rn = Rm & 0xFFFF; break;
                case 0xE: Rn = static_cast<uint32_t>(s8(Rm)); break;
                case 0xF: Rn = static_cast<uint32_t>(s16(Rm)); break;
            }
            break;

        case 0x7: Rn += static_cast<uint32_t>(s8(op)); break;  // ADD #imm,Rn

        case 0x8:
            switch (n) {
                case 0x0: writeB(Rm + (op & 15), r_[0]); break;          // MOV.B R0,@(disp,Rn)
                case 0x1: writeW(Rm + (op & 15) * 2, r_[0]); break;      // MOV.W R0,@(disp,Rn)
                case 0x4: r_[0] = static_cast<uint32_t>(s8(readB(Rm + (op & 15)))); break;
                case 0x5: r_[0] = static_cast<uint32_t>(s16(readW(Rm + (op & 15) * 2))); break;
                case 0x8: setT(r_[0] == static_cast<uint32_t>(s8(op))); break;  // CMP/EQ #imm,R0
                case 0x9:  // BT
                    if (inSlot_) break;
                    if (t()) {
                        next = pc_ + 4 + static_cast<uint32_t>(s8(op) * 2);
                        extra_ += 2;
                    }
                    break;
                case 0xB:  // BF
                    if (inSlot_) break;
                    if (!t()) {
                        next = pc_ + 4 + static_cast<uint32_t>(s8(op) * 2);
                        extra_ += 2;
                    }
                    break;
                case 0xD:  // BT/S
                    if (t()) {
                        delaySlot(pc_ + 4 + static_cast<uint32_t>(s8(op) * 2));
                        extra_ += 1;
                        return;
                    }
                    break;
                case 0xF:  // BF/S
                    if (!t()) {
                        delaySlot(pc_ + 4 + static_cast<uint32_t>(s8(op) * 2));
                        extra_ += 1;
                        return;
                    }
                    break;
                default: illegal(); break;
            }
            break;

        case 0x9: {  // MOV.W @(disp,PC),Rn
            Rn = static_cast<uint32_t>(s16(readW(pcRelative() + (op & 0xFF) * 2)));
            break;
        }
        case 0xA: {  // BRA
            const int32_t d = static_cast<int32_t>(static_cast<uint32_t>(op & 0xFFF) << 20) >> 19;
            delaySlot(pc_ + 4 + static_cast<uint32_t>(d));
            extra_ += 1;
            return;
        }
        case 0xB: {  // BSR
            const int32_t d = static_cast<int32_t>(static_cast<uint32_t>(op & 0xFFF) << 20) >> 19;
            pr_ = pc_ + 4;
            delaySlot(pc_ + 4 + static_cast<uint32_t>(d));
            extra_ += 1;
            return;
        }
        case 0xC: {
            const uint32_t d = op & 0xFF;
            switch (n) {
                case 0x0: writeB(gbr_ + d, r_[0]); break;
                case 0x1: writeW(gbr_ + d * 2, r_[0]); break;
                case 0x2: writeL(gbr_ + d * 4, r_[0]); break;
                case 0x3:  // TRAPA #imm
                    exception(d, pc_ + 2);
                    extra_ += 7;
                    return;
                case 0x4: r_[0] = static_cast<uint32_t>(s8(readB(gbr_ + d))); break;
                case 0x5: r_[0] = static_cast<uint32_t>(s16(readW(gbr_ + d * 2))); break;
                case 0x6: r_[0] = readL(gbr_ + d * 4); break;
                case 0x7: r_[0] = (pcRelative() & ~3u) + d * 4; break;  // MOVA
                case 0x8: setT((r_[0] & d) == 0); break;
                case 0x9: r_[0] &= d; break;
                case 0xA: r_[0] ^= d; break;
                case 0xB: r_[0] |= d; break;
                case 0xC: setT((readB(gbr_ + r_[0]) & d) == 0); extra_ += 2; break;
                case 0xD: writeB(gbr_ + r_[0], readB(gbr_ + r_[0]) & d); extra_ += 2; break;
                case 0xE: writeB(gbr_ + r_[0], readB(gbr_ + r_[0]) ^ d); extra_ += 2; break;
                case 0xF: writeB(gbr_ + r_[0], readB(gbr_ + r_[0]) | d); extra_ += 2; break;
            }
            break;
        }
        case 0xD: Rn = readL((pcRelative() & ~3u) + (op & 0xFF) * 4); break;  // MOV.L @(disp,PC),Rn
        case 0xE: Rn = static_cast<uint32_t>(s8(op)); break;                  // MOV #imm,Rn
        default: illegal(); break;
    }
    if (!inSlot_) pc_ = next;
}

// ---------------------------------------------------------------------------------
// DMA controller

// One transfer unit of the first channel that has work, if any. Auto-request channels
// move a few units at once, so long copies do not crawl.
bool Sh2::runDma() {
    if (!(dmac_.dmaor & 1) || (dmac_.dmaor & 6)) return false;
    for (int c = 0; c < 2; ++c) {
        uint32_t& chcr = dmac_.chcr[c];
        if ((chcr & 3) != 1) continue;  // DE set, TE clear
        const bool autoRequest = chcr & 0x200;
        if (!autoRequest && (dmac_.drcr[c] & 3) != 0) continue;
        if (!autoRequest && !dreq_[c]) continue;
        const int units = autoRequest ? 8 : 1;
        static constexpr uint32_t kSize[4] = {1, 2, 4, 16};
        const int ts = (chcr >> 10) & 3;
        // A request is used up by the unit it starts; the device raises it again (even
        // during this unit's own access to it) when it has more.
        if (!autoRequest) dreq_[c] = false;
        for (int u = 0; u < units && (chcr & 2) == 0; ++u) {
            uint32_t& sar = dmac_.sar[c];
            uint32_t& dar = dmac_.dar[c];
            if (ts == 3) {
                for (uint32_t i = 0; i < 16; i += 4) write(dar + i, read(sar + i, 4), 4);
                sar += 16;
            } else {
                write(dar, read(sar, static_cast<int>(kSize[ts])), static_cast<int>(kSize[ts]));
                const int sm = (chcr >> 12) & 3;
                if (sm == 1) sar += kSize[ts];
                else if (sm == 2) sar -= kSize[ts];
            }
            const int dm = (chcr >> 14) & 3;
            if (dm == 1) dar += kSize[ts];
            else if (dm == 2) dar -= kSize[ts];
            uint32_t& tcr = dmac_.tcr[c];
            tcr = (tcr - (ts == 3 ? 4 : 1)) & 0xFFFFFF;
            if (tcr == 0 || (ts == 3 && (tcr & 0x800000))) {
                tcr = 0;
                chcr |= 2;  // TE: the interrupt (if IE) stays requested until TE is cleared
            }
            if (!autoRequest) break;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------
// Timers and the serial port

void Sh2::runTimers(int cycles) {
    static constexpr uint32_t kFrtDiv[4] = {8, 32, 128, 0};
    const uint32_t div = kFrtDiv[frt_.tcr & 3];
    if (div) {
        frt_.counter += static_cast<uint32_t>(cycles);
        while (frt_.counter >= div) {
            frt_.counter -= div;
            const uint16_t before = frt_.frc;
            frt_.frc = static_cast<uint16_t>(frt_.frc + 1);
            if (frt_.frc == 0) frt_.ftcsr |= 0x02;  // OVF
            if (before == frt_.ocra) {
                frt_.ftcsr |= 0x08;  // OCFA
                if (frt_.ftcsr & 0x01) frt_.frc = 0;  // CCLRA
            }
            if (before == frt_.ocrb) frt_.ftcsr |= 0x04;  // OCFB
        }
    }
    if (wdt_.wtcsr & 0x20) {  // TME
        static constexpr uint32_t kWdtDiv[8] = {2, 64, 128, 256, 512, 1024, 4096, 8192};
        const uint32_t d = kWdtDiv[wdt_.wtcsr & 7];
        wdt_.counter += static_cast<uint32_t>(cycles);
        while (wdt_.counter >= d) {
            wdt_.counter -= d;
            if (++wdt_.wtcnt == 0) wdt_.wtcsr |= 0x80;  // OVF (interval timer mode)
        }
    }
}

// The serial ports of the two SH-2s are wired to each other on the 32X; a byte written
// to one arrives at once at the other if its receiver is on.
void Sh2::sciTransfer() {
    if (!peer_ || !(sci_.scr & 0x20)) return;  // transmitter off
    Sci& other = peer_->sci_;
    if (!(other.scr & 0x10)) return;  // the other's receiver is off
    other.rdr = sci_.tdr;
    other.ssr |= 0x40;  // RDRF
    sci_.ssr |= 0x84;   // TDRE, TEND
}

// ---------------------------------------------------------------------------------
// On-chip registers (0xFFFFFE00-0xFFFFFFFF), byte by byte

uint8_t Sh2::readIo(uint32_t a) {
    auto byteOf = [](uint32_t v, uint32_t addr) { return static_cast<uint8_t>(v >> (8 * (3 - (addr & 3)))); };
    if (a >= 0xFFFFFF00) {
        const uint32_t r = a & 0xFF;
        if (r < 0x04) return byteOf(divu_.dvsr, r);
        if (r < 0x08) return byteOf(divu_.dvdntl, r);
        if (r < 0x0C) return byteOf(divu_.dvcr, r);
        if (r < 0x10) return byteOf(divu_.vcrdiv, r);
        if (r < 0x20) return byteOf((r & 4) ? divu_.dvdntl : divu_.dvdnth, r);
        if (r >= 0x80 && r < 0xA0) {
            const int c = (r >> 4) & 1;
            switch (r & 0x0C) {
                case 0x0: return byteOf(dmac_.sar[c], r);
                case 0x4: return byteOf(dmac_.dar[c], r);
                case 0x8: return byteOf(dmac_.tcr[c], r);
                default: return byteOf(dmac_.chcr[c], r);
            }
        }
        if (r >= 0xA0 && r < 0xA4) return byteOf(dmac_.vcrdma[0], r);
        if (r >= 0xA8 && r < 0xAC) return byteOf(dmac_.vcrdma[1], r);
        if (r >= 0xB0 && r < 0xB4) return byteOf(dmac_.dmaor, r);
        if (r >= 0xE0) return bsc_[r - 0xE0];
        return 0;
    }
    switch (a & 0x1FF) {
        case 0x000: return sci_.smr;
        case 0x001: return sci_.brr;
        case 0x002: return sci_.scr;
        case 0x003: return sci_.tdr;
        case 0x004: return sci_.ssr;
        case 0x005: return sci_.rdr;
        case 0x010: return frt_.tier;
        case 0x011: return frt_.ftcsr;
        case 0x012: return static_cast<uint8_t>(frt_.frc >> 8);
        case 0x013: return static_cast<uint8_t>(frt_.frc);
        case 0x014: return static_cast<uint8_t>(((frt_.tocr & 0x10) ? frt_.ocrb : frt_.ocra) >> 8);
        case 0x015: return static_cast<uint8_t>((frt_.tocr & 0x10) ? frt_.ocrb : frt_.ocra);
        case 0x016: return frt_.tcr;
        case 0x017: return frt_.tocr;
        case 0x018: return static_cast<uint8_t>(frt_.ficr >> 8);
        case 0x019: return static_cast<uint8_t>(frt_.ficr);
        case 0x060: return static_cast<uint8_t>(intc_.iprb >> 8);
        case 0x061: return static_cast<uint8_t>(intc_.iprb);
        case 0x062: return static_cast<uint8_t>(intc_.vcra >> 8);
        case 0x063: return static_cast<uint8_t>(intc_.vcra);
        case 0x064: return static_cast<uint8_t>(intc_.vcrb >> 8);
        case 0x065: return static_cast<uint8_t>(intc_.vcrb);
        case 0x066: return static_cast<uint8_t>(intc_.vcrc >> 8);
        case 0x067: return static_cast<uint8_t>(intc_.vcrc);
        case 0x068: return static_cast<uint8_t>(intc_.vcrd >> 8);
        case 0x069: return static_cast<uint8_t>(intc_.vcrd);
        case 0x071: return dmac_.drcr[0];
        case 0x072: return dmac_.drcr[1];
        case 0x080: return wdt_.wtcsr;
        case 0x081: return wdt_.wtcnt;
        case 0x083: return wdt_.rstcsr;
        case 0x091: return sbycr_;
        case 0x092: return ccr_ & ~0x10;
        case 0x0E0: return static_cast<uint8_t>(intc_.icr >> 8);
        case 0x0E1: return static_cast<uint8_t>(intc_.icr);
        case 0x0E2: return static_cast<uint8_t>(intc_.ipra >> 8);
        case 0x0E3: return static_cast<uint8_t>(intc_.ipra);
        case 0x0E4: return static_cast<uint8_t>(intc_.vcrwdt >> 8);
        case 0x0E5: return static_cast<uint8_t>(intc_.vcrwdt);
        default: return 0;
    }
}

void Sh2::writeIo(uint32_t a, uint8_t v) {
    auto setByte = [](uint32_t& reg, uint32_t addr, uint8_t value) {
        const int shift = 8 * (3 - static_cast<int>(addr & 3));
        reg = (reg & ~(0xFFu << shift)) | static_cast<uint32_t>(value) << shift;
    };
    if (a >= 0xFFFFFF00) {
        const uint32_t r = a & 0xFF;
        if (r < 0x04) {
            setByte(divu_.dvsr, r, v);
        } else if (r < 0x08) {
            setByte(divu_.dvdntl, r, v);
            if (r == 0x07) divide32();
        } else if (r < 0x0C) {
            setByte(divu_.dvcr, r, v);
            divu_.dvcr &= 3;
        } else if (r < 0x10) {
            setByte(divu_.vcrdiv, r, v);
        } else if (r < 0x20) {
            if (r & 4) {
                setByte(divu_.dvdntl, r, v);
                if ((r & 7) == 7) divide64();
            } else {
                setByte(divu_.dvdnth, r, v);
            }
        } else if (r >= 0x80 && r < 0xA0) {
            const int c = (r >> 4) & 1;
            switch (r & 0x0C) {
                case 0x0: setByte(dmac_.sar[c], r, v); break;
                case 0x4: setByte(dmac_.dar[c], r, v); break;
                case 0x8: setByte(dmac_.tcr[c], r, v); dmac_.tcr[c] &= 0xFFFFFF; break;
                default: {
                    const uint32_t te = dmac_.chcr[c] & 2;
                    setByte(dmac_.chcr[c], r, v);
                    // TE is cleared by writing 0 after reading 1; writing 1 keeps it.
                    if ((r & 3) == 3) dmac_.chcr[c] = (dmac_.chcr[c] & ~2u) | (te & dmac_.chcr[c]);
                    dmac_.chcr[c] &= 0xFFFF;
                    break;
                }
            }
        } else if (r >= 0xA0 && r < 0xA4) {
            setByte(dmac_.vcrdma[0], r, v);
        } else if (r >= 0xA8 && r < 0xAC) {
            setByte(dmac_.vcrdma[1], r, v);
        } else if (r >= 0xB0 && r < 0xB4) {
            const uint32_t flags = dmac_.dmaor & 6;
            setByte(dmac_.dmaor, r, v);
            if (r == 0xB3) dmac_.dmaor = (dmac_.dmaor & 9) | (flags & dmac_.dmaor);
        } else if (r >= 0xE0) {
            bsc_[r - 0xE0] = v;
        }
        return;
    }
    auto setHigh = [](uint16_t& reg, uint8_t value) { reg = static_cast<uint16_t>((reg & 0x00FF) | value << 8); };
    auto setLow = [](uint16_t& reg, uint8_t value) { reg = static_cast<uint16_t>((reg & 0xFF00) | value); };
    switch (a & 0x1FF) {
        case 0x000: sci_.smr = v; break;
        case 0x001: sci_.brr = v; break;
        case 0x002:
            sci_.scr = v;
            if (!(v & 0x20)) sci_.ssr |= 0x84;
            break;
        case 0x003: sci_.tdr = v; break;
        case 0x004:
            // Flags are cleared by writing 0; clearing TDRE sends the byte in TDR.
            sci_.ssr = static_cast<uint8_t>((sci_.ssr & (v | 0x06)) & 0xFE) | (v & 1);
            break;
        case 0x010: frt_.tier = v | 0x01; break;
        case 0x011: frt_.ftcsr = static_cast<uint8_t>((frt_.ftcsr & (v | 0x01) & 0x8E) | (v & 0x01)); break;
        case 0x012: frt_.latch = v; break;
        case 0x013: frt_.frc = static_cast<uint16_t>(frt_.latch << 8 | v); break;
        case 0x014: frt_.latch = v; break;
        case 0x015: {
            const uint16_t value = static_cast<uint16_t>(frt_.latch << 8 | v);
            if (frt_.tocr & 0x10) frt_.ocrb = value;
            else frt_.ocra = value;
            break;
        }
        case 0x016: frt_.tcr = v; break;
        case 0x017: frt_.tocr = v | 0xE0; break;
        case 0x060: setHigh(intc_.iprb, v); break;
        case 0x061: setLow(intc_.iprb, v); break;
        case 0x062: setHigh(intc_.vcra, v); break;
        case 0x063: setLow(intc_.vcra, v); break;
        case 0x064: setHigh(intc_.vcrb, v); break;
        case 0x065: setLow(intc_.vcrb, v); break;
        case 0x066: setHigh(intc_.vcrc, v); break;
        case 0x067: setLow(intc_.vcrc, v); break;
        case 0x068: setHigh(intc_.vcrd, v); break;
        case 0x069: setLow(intc_.vcrd, v); break;
        case 0x071: dmac_.drcr[0] = v & 3; break;
        case 0x072: dmac_.drcr[1] = v & 3; break;
        // The watchdog's registers are written as words: 0xA5xx sets WTCSR, 0x5Axx WTCNT.
        case 0x080: wdt_.rstcsr = v; break;  // high byte of the word: the key, kept here
        case 0x081:
            if (wdt_.rstcsr == 0xA5) {
                wdt_.wtcsr = static_cast<uint8_t>((wdt_.wtcsr & (v | 0x7F) & 0x80) | (v & 0x67) | 0x18);
                if (!(v & 0x20)) wdt_.wtcnt = 0;
            } else if (wdt_.rstcsr == 0x5A) {
                wdt_.wtcnt = v;
            }
            wdt_.rstcsr = 0x1F;
            break;
        case 0x091: sbycr_ = v; break;
        case 0x092: ccr_ = v & ~0x10; break;
        case 0x0E0: setHigh(intc_.icr, v); break;
        case 0x0E1: setLow(intc_.icr, v); break;
        case 0x0E2: setHigh(intc_.ipra, v); break;
        case 0x0E3: setLow(intc_.ipra, v); break;
        case 0x0E4: setHigh(intc_.vcrwdt, v); break;
        case 0x0E5: setLow(intc_.vcrwdt, v); break;
        default: break;
    }
}

void Sh2::divide32() {
    const int32_t divisor = static_cast<int32_t>(divu_.dvsr);
    const int32_t dividend = static_cast<int32_t>(divu_.dvdntl);
    if (divisor == 0 || (dividend == INT32_MIN && divisor == -1)) {
        divu_.dvcr |= 1;
        divu_.dvdnth = static_cast<uint32_t>(dividend);
        divu_.dvdntl = dividend < 0 ? 0x80000000u : 0x7FFFFFFFu;
        return;
    }
    divu_.dvdntl = static_cast<uint32_t>(dividend / divisor);
    divu_.dvdnth = static_cast<uint32_t>(dividend % divisor);
    extra_ += 0;  // the 39 cycles of the real unit overlap with the CPU
}

void Sh2::divide64() {
    const int32_t divisor = static_cast<int32_t>(divu_.dvsr);
    const int64_t dividend = static_cast<int64_t>(static_cast<uint64_t>(divu_.dvdnth) << 32 | divu_.dvdntl);
    if (divisor == 0) {
        divu_.dvcr |= 1;
        divu_.dvdntl = dividend < 0 ? 0x80000000u : 0x7FFFFFFFu;
        return;
    }
    const int64_t q = dividend / divisor;
    const int64_t r = dividend % divisor;
    if (q > INT32_MAX || q < INT32_MIN) {
        divu_.dvcr |= 1;
        divu_.dvdntl = q < 0 ? 0x80000000u : 0x7FFFFFFFu;
        return;
    }
    divu_.dvdntl = static_cast<uint32_t>(q);
    divu_.dvdnth = static_cast<uint32_t>(r);
}

}  // namespace plumsg
