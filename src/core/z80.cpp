#include "z80.h"

#include <array>

namespace plumsg {

namespace {

constexpr uint8_t FC = 0x01;
constexpr uint8_t FN = 0x02;
constexpr uint8_t FPV = 0x04;
constexpr uint8_t FX = 0x08;
constexpr uint8_t FH = 0x10;
constexpr uint8_t FY = 0x20;
constexpr uint8_t FZ = 0x40;
constexpr uint8_t FS = 0x80;

struct FlagTables {
    std::array<uint8_t, 256> sz53{};   // S, Z and the undocumented X/Y bits
    std::array<uint8_t, 256> sz53p{};  // same plus parity
    FlagTables() {
        for (int v = 0; v < 256; ++v) {
            uint8_t f = static_cast<uint8_t>(v & (FS | FX | FY));
            if (v == 0) f |= FZ;
            int bits = 0;
            for (int b = 0; b < 8; ++b) bits += (v >> b) & 1;
            sz53[v] = f;
            sz53p[v] = static_cast<uint8_t>(f | ((bits & 1) ? 0 : FPV));
        }
    }
};

const FlagTables kTables;
const auto& sz53 = kTables.sz53;
const auto& sz53p = kTables.sz53p;

}  // namespace

Z80::Z80(Z80Bus& bus) : bus_(bus) { powerOn(); }

void Z80::powerOn() {
    af.w = bc.w = de.w = hl.w = 0xFFFF;
    af2.w = bc2.w = de2.w = hl2.w = 0xFFFF;
    ix.w = iy.w = 0xFFFF;
    sp = 0xFFFF;
    reset();
}

// The /RESET line leaves the registers as they were (misc_test's "Z80 no clear regs on
// Z80RES"): only PC, I, R, the interrupt flip-flops and the mode start over.
void Z80::reset() {
    pc = 0;
    wz = 0;
    i = r = 0;
    iff1 = iff2 = false;
    im = 0;
    halted = false;
    nmiPending_ = false;
    eiDelay_ = false;
}

uint16_t Z80::read16(uint16_t a) {
    uint8_t lo = read8(a);
    uint8_t hi = read8(static_cast<uint16_t>(a + 1));
    return static_cast<uint16_t>(lo | (hi << 8));
}

void Z80::write16(uint16_t a, uint16_t v) {
    write8(a, static_cast<uint8_t>(v));
    write8(static_cast<uint16_t>(a + 1), static_cast<uint8_t>(v >> 8));
}

uint8_t Z80::fetchOp() {
    r = static_cast<uint8_t>((r & 0x80) | ((r + 1) & 0x7F));
    return read8(pc++);
}

uint16_t Z80::fetch16() {
    uint16_t v = read16(pc);
    pc = static_cast<uint16_t>(pc + 2);
    return v;
}

void Z80::push(uint16_t v) {
    sp = static_cast<uint16_t>(sp - 2);
    write16(sp, v);
}

uint16_t Z80::pop() {
    uint16_t v = read16(sp);
    sp = static_cast<uint16_t>(sp + 2);
    return v;
}

Z80::Pair& Z80::indexReg() {
    if (idx_ == 1) return ix;
    if (idx_ == 2) return iy;
    return hl;
}

uint8_t* Z80::reg8(int reg, bool allowIndex) {
    switch (reg) {
        case 0: return &bc.b.h;
        case 1: return &bc.b.l;
        case 2: return &de.b.h;
        case 3: return &de.b.l;
        case 4: return allowIndex ? &indexReg().b.h : &hl.b.h;
        case 5: return allowIndex ? &indexReg().b.l : &hl.b.l;
        default: return &af.b.h;  // 7; 6 is memory and never routed here
    }
}

Z80::Pair& Z80::rp(int p) {
    switch (p) {
        case 0: return bc;
        case 1: return de;
        case 2: return indexReg();
        default: {
            // SP is kept as a plain integer; alias it through a scratch pair.
            static thread_local Pair spPair;
            spPair.w = sp;
            return spPair;
        }
    }
}

Z80::Pair& Z80::rp2(int p) { return p == 3 ? af : rp(p); }

uint16_t Z80::indirectAddr() {
    if (idx_ == 0) return hl.w;
    int8_t d = static_cast<int8_t>(fetch8());
    wz = static_cast<uint16_t>(indexReg().w + d);
    return wz;
}

bool Z80::cond(int cc) const {
    uint8_t f = af.b.l;
    switch (cc) {
        case 0: return !(f & FZ);
        case 1: return f & FZ;
        case 2: return !(f & FC);
        case 3: return f & FC;
        case 4: return !(f & FPV);
        case 5: return f & FPV;
        case 6: return !(f & FS);
        default: return f & FS;
    }
}

void Z80::add8(uint8_t v, int carry) {
    uint8_t a = A();
    int res = a + v + carry;
    uint8_t r8 = static_cast<uint8_t>(res);
    uint8_t f = sz53[r8];
    f |= (a ^ v ^ r8) & FH;
    if ((a ^ ~v) & (a ^ r8) & 0x80) f |= FPV;
    if (res > 0xFF) f |= FC;
    A() = r8;
    F() = f;
}

void Z80::sub8(uint8_t v, int carry, bool store) {
    uint8_t a = A();
    int res = a - v - carry;
    uint8_t r8 = static_cast<uint8_t>(res);
    uint8_t f = static_cast<uint8_t>((r8 & FS) | (r8 == 0 ? FZ : 0) | FN);
    f |= (a ^ v ^ r8) & FH;
    if ((a ^ v) & (a ^ r8) & 0x80) f |= FPV;
    if (res & 0x100) f |= FC;
    // CP takes the undocumented bits from the operand, not the result.
    f |= (store ? r8 : v) & (FX | FY);
    if (store) A() = r8;
    F() = f;
}

void Z80::alu(int op, uint8_t v) {
    switch (op) {
        case 0: add8(v, 0); break;
        case 1: add8(v, F() & FC); break;
        case 2: sub8(v, 0, true); break;
        case 3: sub8(v, F() & FC, true); break;
        case 4: A() &= v; F() = sz53p[A()] | FH; break;
        case 5: A() ^= v; F() = sz53p[A()]; break;
        case 6: A() |= v; F() = sz53p[A()]; break;
        default: sub8(v, 0, false); break;
    }
}

uint8_t Z80::inc8(uint8_t v) {
    uint8_t res = static_cast<uint8_t>(v + 1);
    uint8_t f = static_cast<uint8_t>((F() & FC) | sz53[res]);
    if ((res & 0x0F) == 0) f |= FH;
    if (v == 0x7F) f |= FPV;
    F() = f;
    return res;
}

uint8_t Z80::dec8(uint8_t v) {
    uint8_t res = static_cast<uint8_t>(v - 1);
    uint8_t f = static_cast<uint8_t>((F() & FC) | sz53[res] | FN);
    if ((v & 0x0F) == 0) f |= FH;
    if (v == 0x80) f |= FPV;
    F() = f;
    return res;
}

uint16_t Z80::add16(uint16_t a, uint16_t b) {
    uint32_t res = static_cast<uint32_t>(a) + b;
    wz = static_cast<uint16_t>(a + 1);
    uint8_t f = static_cast<uint8_t>(F() & (FS | FZ | FPV));
    f |= ((a ^ b ^ res) >> 8) & FH;
    f |= (res >> 16) & FC;
    f |= (res >> 8) & (FX | FY);
    F() = f;
    return static_cast<uint16_t>(res);
}

void Z80::adc16(uint16_t v) {
    uint16_t h = hl.w;
    uint32_t res = static_cast<uint32_t>(h) + v + (F() & FC);
    uint16_t r16 = static_cast<uint16_t>(res);
    wz = static_cast<uint16_t>(h + 1);
    uint8_t f = static_cast<uint8_t>((r16 >> 8) & (FS | FX | FY));
    if (r16 == 0) f |= FZ;
    f |= ((h ^ v ^ r16) >> 8) & FH;
    if (~(h ^ v) & (h ^ r16) & 0x8000) f |= FPV;
    if (res > 0xFFFF) f |= FC;
    hl.w = r16;
    F() = f;
}

void Z80::sbc16(uint16_t v) {
    uint16_t h = hl.w;
    int32_t res = static_cast<int32_t>(h) - v - (F() & FC);
    uint16_t r16 = static_cast<uint16_t>(res);
    wz = static_cast<uint16_t>(h + 1);
    uint8_t f = static_cast<uint8_t>(((r16 >> 8) & (FS | FX | FY)) | FN);
    if (r16 == 0) f |= FZ;
    f |= ((h ^ v ^ r16) >> 8) & FH;
    if ((h ^ v) & (h ^ r16) & 0x8000) f |= FPV;
    if (res & 0x10000) f |= FC;
    hl.w = r16;
    F() = f;
}

uint8_t Z80::rotShift(int op, uint8_t v) {
    uint8_t c;
    switch (op) {
        case 0: c = v >> 7; v = static_cast<uint8_t>((v << 1) | c); break;
        case 1: c = v & 1; v = static_cast<uint8_t>((v >> 1) | (c << 7)); break;
        case 2: c = v >> 7; v = static_cast<uint8_t>((v << 1) | (F() & FC)); break;
        case 3: c = v & 1; v = static_cast<uint8_t>((v >> 1) | ((F() & FC) << 7)); break;
        case 4: c = v >> 7; v = static_cast<uint8_t>(v << 1); break;
        case 5: c = v & 1; v = static_cast<uint8_t>((v >> 1) | (v & 0x80)); break;
        case 6: c = v >> 7; v = static_cast<uint8_t>((v << 1) | 1); break;  // SLL (undocumented)
        default: c = v & 1; v = static_cast<uint8_t>(v >> 1); break;
    }
    F() = static_cast<uint8_t>(sz53p[v] | c);
    return v;
}

void Z80::bitTest(int bit, uint8_t v, uint8_t xyFrom) {
    uint8_t f = static_cast<uint8_t>((F() & FC) | FH | (xyFrom & (FX | FY)));
    if (!(v & (1 << bit))) f |= FZ | FPV;
    if (bit == 7 && (v & 0x80)) f |= FS;
    F() = f;
}

void Z80::daa() {
    uint8_t a = A();
    uint8_t f = F();
    uint8_t corr = 0;
    bool carry = f & FC;
    if ((f & FH) || (a & 0x0F) > 9) corr |= 0x06;
    if (carry || a > 0x99) {
        corr |= 0x60;
        carry = true;
    }
    bool half;
    if (f & FN) {
        half = (f & FH) && (a & 0x0F) < 6;
        a = static_cast<uint8_t>(a - corr);
    } else {
        half = (a & 0x0F) > 9;
        a = static_cast<uint8_t>(a + corr);
    }
    A() = a;
    F() = static_cast<uint8_t>(sz53p[a] | (f & FN) | (half ? FH : 0) | (carry ? FC : 0));
}

int Z80::step() {
    if (nmiPending_) {
        nmiPending_ = false;
        halted = false;
        iff1 = false;
        r = static_cast<uint8_t>((r & 0x80) | ((r + 1) & 0x7F));
        push(pc);
        pc = 0x0066;
        wz = pc;
        return 11;
    }
    if (irqLine_ && iff1 && !eiDelay_) {
        halted = false;
        iff1 = iff2 = false;
        r = static_cast<uint8_t>((r & 0x80) | ((r + 1) & 0x7F));
        push(pc);
        if (im == 2) {
            // The Game Gear data bus floats high during acknowledge.
            pc = read16(static_cast<uint16_t>((i << 8) | 0xFF));
            wz = pc;
            return 19;
        }
        pc = 0x0038;  // IM 0 sees RST 38h on the floating bus, same as IM 1
        wz = pc;
        return 13;
    }
    eiDelay_ = false;
    if (halted) {
        r = static_cast<uint8_t>((r & 0x80) | ((r + 1) & 0x7F));
        return 4;
    }

    idx_ = 0;
    int cycles = 0;
    uint8_t op = fetchOp();
    while (op == 0xDD || op == 0xFD) {
        idx_ = op == 0xDD ? 1 : 2;
        cycles += 4;
        op = fetchOp();
    }
    if (op == 0xCB) return cycles + (idx_ ? execIndexedCB() : execCB());
    if (op == 0xED) {
        idx_ = 0;
        return cycles + execED();
    }
    return cycles + execMain(op);
}

int Z80::execMain(uint8_t op) {
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;
    const int p = y >> 1;
    const int q = y & 1;
    const int memExtra = idx_ ? 8 : 0;

    switch (x) {
        case 0:
            switch (z) {
                case 0:
                    switch (y) {
                        case 0: return 4;
                        case 1: {
                            uint16_t t = af.w;
                            af.w = af2.w;
                            af2.w = t;
                            return 4;
                        }
                        case 2: {
                            int8_t d = static_cast<int8_t>(fetch8());
                            if (--bc.b.h) {
                                pc = static_cast<uint16_t>(pc + d);
                                wz = pc;
                                return 13;
                            }
                            return 8;
                        }
                        case 3: {
                            int8_t d = static_cast<int8_t>(fetch8());
                            pc = static_cast<uint16_t>(pc + d);
                            wz = pc;
                            return 12;
                        }
                        default: {
                            int8_t d = static_cast<int8_t>(fetch8());
                            if (cond(y - 4)) {
                                pc = static_cast<uint16_t>(pc + d);
                                wz = pc;
                                return 12;
                            }
                            return 7;
                        }
                    }
                case 1:
                    if (q == 0) {
                        uint16_t v = fetch16();
                        if (p == 3) sp = v; else rp(p).w = v;
                        return 10;
                    } else {
                        uint16_t v = p == 3 ? sp : rp(p).w;
                        indexReg().w = add16(indexReg().w, v);
                        return 11;
                    }
                case 2:
                    switch (y) {
                        case 0:
                            write8(bc.w, A());
                            wz = static_cast<uint16_t>(((bc.w + 1) & 0xFF) | (A() << 8));
                            return 7;
                        case 1:
                            A() = read8(bc.w);
                            wz = static_cast<uint16_t>(bc.w + 1);
                            return 7;
                        case 2:
                            write8(de.w, A());
                            wz = static_cast<uint16_t>(((de.w + 1) & 0xFF) | (A() << 8));
                            return 7;
                        case 3:
                            A() = read8(de.w);
                            wz = static_cast<uint16_t>(de.w + 1);
                            return 7;
                        case 4: {
                            uint16_t a = fetch16();
                            write16(a, indexReg().w);
                            wz = static_cast<uint16_t>(a + 1);
                            return 16;
                        }
                        case 5: {
                            uint16_t a = fetch16();
                            indexReg().w = read16(a);
                            wz = static_cast<uint16_t>(a + 1);
                            return 16;
                        }
                        case 6: {
                            uint16_t a = fetch16();
                            write8(a, A());
                            wz = static_cast<uint16_t>(((a + 1) & 0xFF) | (A() << 8));
                            return 13;
                        }
                        default: {
                            uint16_t a = fetch16();
                            A() = read8(a);
                            wz = static_cast<uint16_t>(a + 1);
                            return 13;
                        }
                    }
                case 3: {
                    int delta = q == 0 ? 1 : -1;
                    if (p == 3) sp = static_cast<uint16_t>(sp + delta);
                    else rp(p).w = static_cast<uint16_t>(rp(p).w + delta);
                    return 6;
                }
                case 4:
                case 5:
                    if (y == 6) {
                        uint16_t a = indirectAddr();
                        uint8_t v = read8(a);
                        write8(a, z == 4 ? inc8(v) : dec8(v));
                        return 11 + memExtra;
                    } else {
                        uint8_t* reg = reg8(y, true);
                        *reg = z == 4 ? inc8(*reg) : dec8(*reg);
                        return 4;
                    }
                case 6:
                    if (y == 6) {
                        uint16_t a = indirectAddr();
                        write8(a, fetch8());
                        return 10 + (idx_ ? 5 : 0);
                    }
                    *reg8(y, true) = fetch8();
                    return 7;
                default: {
                    uint8_t a = A();
                    uint8_t keep = static_cast<uint8_t>(F() & (FS | FZ | FPV));
                    switch (y) {
                        case 0:
                            a = static_cast<uint8_t>((a << 1) | (a >> 7));
                            F() = static_cast<uint8_t>(keep | (a & (FX | FY | FC)));
                            break;
                        case 1: {
                            uint8_t c = a & 1;
                            a = static_cast<uint8_t>((a >> 1) | (c << 7));
                            F() = static_cast<uint8_t>(keep | (a & (FX | FY)) | c);
                            break;
                        }
                        case 2: {
                            uint8_t c = a >> 7;
                            a = static_cast<uint8_t>((a << 1) | (F() & FC));
                            F() = static_cast<uint8_t>(keep | (a & (FX | FY)) | c);
                            break;
                        }
                        case 3: {
                            uint8_t c = a & 1;
                            a = static_cast<uint8_t>((a >> 1) | ((F() & FC) << 7));
                            F() = static_cast<uint8_t>(keep | (a & (FX | FY)) | c);
                            break;
                        }
                        case 4: daa(); return 4;
                        case 5:
                            a = static_cast<uint8_t>(~a);
                            F() = static_cast<uint8_t>((F() & (FS | FZ | FPV | FC)) | FH | FN | (a & (FX | FY)));
                            break;
                        case 6:
                            F() = static_cast<uint8_t>(keep | FC | (a & (FX | FY)));
                            break;
                        default: {
                            uint8_t c = F() & FC;
                            F() = static_cast<uint8_t>(keep | (c ? FH : FC) | (a & (FX | FY)));
                            break;
                        }
                    }
                    A() = a;
                    return 4;
                }
            }

        case 1:
            if (y == 6 && z == 6) {
                halted = true;
                return 4;
            }
            if (y == 6) {
                uint16_t a = indirectAddr();
                write8(a, *reg8(z, false));
                return 7 + memExtra;
            }
            if (z == 6) {
                uint16_t a = indirectAddr();
                *reg8(y, false) = read8(a);
                return 7 + memExtra;
            }
            *reg8(y, true) = *reg8(z, true);
            return 4;

        case 2:
            if (z == 6) {
                alu(y, read8(indirectAddr()));
                return 7 + memExtra;
            }
            alu(y, *reg8(z, true));
            return 4;

        default:
            switch (z) {
                case 0:
                    if (cond(y)) {
                        pc = pop();
                        wz = pc;
                        return 11;
                    }
                    return 5;
                case 1:
                    if (q == 0) {
                        uint16_t v = pop();
                        if (p == 3) af.w = v; else rp(p).w = v;
                        return 10;
                    }
                    switch (p) {
                        case 0:
                            pc = pop();
                            wz = pc;
                            return 10;
                        case 1: {
                            uint16_t t;
                            t = bc.w; bc.w = bc2.w; bc2.w = t;
                            t = de.w; de.w = de2.w; de2.w = t;
                            t = hl.w; hl.w = hl2.w; hl2.w = t;
                            return 4;
                        }
                        case 2:
                            pc = indexReg().w;
                            return 4;
                        default:
                            sp = indexReg().w;
                            return 6;
                    }
                case 2: {
                    uint16_t a = fetch16();
                    wz = a;
                    if (cond(y)) pc = a;
                    return 10;
                }
                case 3:
                    switch (y) {
                        case 0:
                            pc = fetch16();
                            wz = pc;
                            return 10;
                        case 2: {
                            uint8_t n = fetch8();
                            ioCycle = 8;
                            bus_.out(static_cast<uint16_t>(n | (A() << 8)), A());
                            wz = static_cast<uint16_t>(((n + 1) & 0xFF) | (A() << 8));
                            return 11;
                        }
                        case 3: {
                            uint16_t port = static_cast<uint16_t>(fetch8() | (A() << 8));
                            ioCycle = 8;
                            A() = bus_.in(port);
                            wz = static_cast<uint16_t>(port + 1);
                            return 11;
                        }
                        case 4: {
                            uint16_t v = read16(sp);
                            write16(sp, indexReg().w);
                            indexReg().w = v;
                            wz = v;
                            return 19;
                        }
                        case 5: {
                            uint16_t t = de.w;
                            de.w = hl.w;
                            hl.w = t;
                            return 4;
                        }
                        case 6:
                            iff1 = iff2 = false;
                            return 4;
                        case 7:
                            iff1 = iff2 = true;
                            eiDelay_ = true;
                            return 4;
                        default:
                            return 4;  // CB prefix, handled in step()
                    }
                case 4: {
                    uint16_t a = fetch16();
                    wz = a;
                    if (cond(y)) {
                        push(pc);
                        pc = a;
                        return 17;
                    }
                    return 10;
                }
                case 5:
                    if (q == 0) {
                        push(p == 3 ? af.w : rp(p).w);
                        return 11;
                    } else {
                        // p == 0 is CALL nn; the other slots are prefixes handled in step().
                        uint16_t a = fetch16();
                        wz = a;
                        push(pc);
                        pc = a;
                        return 17;
                    }
                case 6:
                    alu(y, fetch8());
                    return 7;
                default:
                    push(pc);
                    pc = static_cast<uint16_t>(y * 8);
                    wz = pc;
                    return 11;
            }
    }
}

int Z80::execCB() {
    uint8_t op = fetchOp();
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;

    if (z == 6) {
        uint16_t a = hl.w;
        uint8_t v = read8(a);
        switch (x) {
            case 0: write8(a, rotShift(y, v)); return 15;
            case 1: bitTest(y, v, static_cast<uint8_t>(wz >> 8)); return 12;
            case 2: write8(a, static_cast<uint8_t>(v & ~(1 << y))); return 15;
            default: write8(a, static_cast<uint8_t>(v | (1 << y))); return 15;
        }
    }
    uint8_t* reg = reg8(z, false);
    switch (x) {
        case 0: *reg = rotShift(y, *reg); break;
        case 1: bitTest(y, *reg, *reg); break;
        case 2: *reg = static_cast<uint8_t>(*reg & ~(1 << y)); break;
        default: *reg = static_cast<uint8_t>(*reg | (1 << y)); break;
    }
    return 8;
}

int Z80::execIndexedCB() {
    int8_t d = static_cast<int8_t>(fetch8());
    uint8_t op = fetch8();
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;
    uint16_t a = static_cast<uint16_t>(indexReg().w + d);
    wz = a;
    uint8_t v = read8(a);

    switch (x) {
        case 0: v = rotShift(y, v); break;
        case 1: bitTest(y, v, static_cast<uint8_t>(a >> 8)); return 16;
        case 2: v = static_cast<uint8_t>(v & ~(1 << y)); break;
        default: v = static_cast<uint8_t>(v | (1 << y)); break;
    }
    write8(a, v);
    // Undocumented: the result is also copied into a register unless z == 6.
    if (z != 6) *reg8(z, false) = v;
    return 19;
}

int Z80::blockOp(int y, int z) {
    const int dir = (y & 1) ? -1 : 1;
    const bool repeat = y >= 6;

    switch (z) {
        case 0: {  // LDI / LDD / LDIR / LDDR
            uint8_t v = read8(hl.w);
            write8(de.w, v);
            hl.w = static_cast<uint16_t>(hl.w + dir);
            de.w = static_cast<uint16_t>(de.w + dir);
            --bc.w;
            uint8_t n = static_cast<uint8_t>(v + A());
            uint8_t f = static_cast<uint8_t>((F() & (FS | FZ | FC)) | (n & FX) | ((n & 0x02) << 4));
            if (bc.w) f |= FPV;
            F() = f;
            if (repeat && bc.w) {
                pc = static_cast<uint16_t>(pc - 2);
                wz = static_cast<uint16_t>(pc + 1);
                return 21;
            }
            return 16;
        }
        case 1: {  // CPI / CPD / CPIR / CPDR
            uint8_t v = read8(hl.w);
            uint8_t res = static_cast<uint8_t>(A() - v);
            uint8_t h = (A() ^ v ^ res) & FH;
            hl.w = static_cast<uint16_t>(hl.w + dir);
            --bc.w;
            wz = static_cast<uint16_t>(wz + dir);
            uint8_t n = static_cast<uint8_t>(res - (h ? 1 : 0));
            uint8_t f = static_cast<uint8_t>((res & FS) | (res == 0 ? FZ : 0) | h | FN | (F() & FC) |
                                             (n & FX) | ((n & 0x02) << 4));
            if (bc.w) f |= FPV;
            F() = f;
            if (repeat && bc.w && res != 0) {
                pc = static_cast<uint16_t>(pc - 2);
                wz = static_cast<uint16_t>(pc + 1);
                return 21;
            }
            return 16;
        }
        case 2: {  // INI / IND / INIR / INDR
            ioCycle = 10;
            uint8_t v = bus_.in(bc.w);
            write8(hl.w, v);
            wz = static_cast<uint16_t>(bc.w + dir);
            --bc.b.h;
            hl.w = static_cast<uint16_t>(hl.w + dir);
            unsigned k = v + static_cast<uint8_t>(bc.b.l + dir);
            uint8_t f = sz53[bc.b.h];
            if (v & 0x80) f |= FN;
            if (k > 0xFF) f |= FH | FC;
            f |= sz53p[(k & 7) ^ bc.b.h] & FPV;
            F() = f;
            if (repeat && bc.b.h) {
                pc = static_cast<uint16_t>(pc - 2);
                return 21;
            }
            return 16;
        }
        default: {  // OUTI / OUTD / OTIR / OTDR
            uint8_t v = read8(hl.w);
            --bc.b.h;
            ioCycle = 13;
            bus_.out(bc.w, v);
            hl.w = static_cast<uint16_t>(hl.w + dir);
            wz = static_cast<uint16_t>(bc.w + dir);
            unsigned k = v + hl.b.l;
            uint8_t f = sz53[bc.b.h];
            if (v & 0x80) f |= FN;
            if (k > 0xFF) f |= FH | FC;
            f |= sz53p[(k & 7) ^ bc.b.h] & FPV;
            F() = f;
            if (repeat && bc.b.h) {
                pc = static_cast<uint16_t>(pc - 2);
                return 21;
            }
            return 16;
        }
    }
}

int Z80::execED() {
    uint8_t op = fetchOp();
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;
    const int p = y >> 1;
    const int q = y & 1;

    if (x == 2 && z <= 3 && y >= 4) return blockOp(y, z);
    if (x != 1) return 8;  // invalid ED opcodes behave as two NOPs

    switch (z) {
        case 0: {
            ioCycle = 9;
            uint8_t v = bus_.in(bc.w);
            wz = static_cast<uint16_t>(bc.w + 1);
            if (y != 6) *reg8(y, false) = v;
            F() = static_cast<uint8_t>((F() & FC) | sz53p[v]);
            return 12;
        }
        case 1:
            ioCycle = 9;
            bus_.out(bc.w, y == 6 ? 0 : *reg8(y, false));
            wz = static_cast<uint16_t>(bc.w + 1);
            return 12;
        case 2: {
            uint16_t v = p == 3 ? sp : rp(p).w;
            if (q == 0) sbc16(v); else adc16(v);
            return 15;
        }
        case 3: {
            uint16_t a = fetch16();
            wz = static_cast<uint16_t>(a + 1);
            if (q == 0) {
                write16(a, p == 3 ? sp : rp(p).w);
            } else {
                uint16_t v = read16(a);
                if (p == 3) sp = v; else rp(p).w = v;
            }
            return 20;
        }
        case 4: {
            uint8_t v = A();
            A() = 0;
            sub8(v, 0, true);
            return 8;
        }
        case 5:
            iff1 = iff2;
            pc = pop();
            wz = pc;
            return 14;
        case 6: {
            static constexpr int kModes[8] = {0, 0, 1, 2, 0, 0, 1, 2};
            im = kModes[y];
            return 8;
        }
        default:
            switch (y) {
                case 0: i = A(); return 9;
                case 1: r = A(); return 9;
                case 2:
                case 3: {
                    uint8_t v = y == 2 ? i : r;
                    A() = v;
                    F() = static_cast<uint8_t>((F() & FC) | sz53[v] | (iff2 ? FPV : 0));
                    return 9;
                }
                case 4: {
                    uint8_t v = read8(hl.w);
                    write8(hl.w, static_cast<uint8_t>((A() << 4) | (v >> 4)));
                    A() = static_cast<uint8_t>((A() & 0xF0) | (v & 0x0F));
                    F() = static_cast<uint8_t>((F() & FC) | sz53p[A()]);
                    wz = static_cast<uint16_t>(hl.w + 1);
                    return 18;
                }
                case 5: {
                    uint8_t v = read8(hl.w);
                    write8(hl.w, static_cast<uint8_t>((v << 4) | (A() & 0x0F)));
                    A() = static_cast<uint8_t>((A() & 0xF0) | (v >> 4));
                    F() = static_cast<uint8_t>((F() & FC) | sz53p[A()]);
                    wz = static_cast<uint16_t>(hl.w + 1);
                    return 18;
                }
                default:
                    return 8;
            }
    }
}

}  // namespace plumsg
