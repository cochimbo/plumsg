#include "m68k.h"

#include <algorithm>
#include <utility>

namespace plumsg {

namespace {

constexpr uint16_t kC = 0x01, kV = 0x02, kZ = 0x04, kN = 0x08, kX = 0x10;

constexpr uint32_t maskOf(int size) { return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
constexpr uint32_t msbOf(int size) { return size == 1 ? 0x80u : size == 2 ? 0x8000u : 0x80000000u; }
constexpr int sizeFromBits(int bits) { return bits == 0 ? 1 : bits == 1 ? 2 : bits == 2 ? 4 : 0; }
constexpr uint32_t signExtend(uint32_t v, int size) {
    return size == 1 ? static_cast<uint32_t>(static_cast<int8_t>(v))
                     : size == 2 ? static_cast<uint32_t>(static_cast<int16_t>(v)) : v;
}

// Addressing-mode classes, as bit sets over the 12 modes: Dn, An, (An), (An)+, -(An),
// d16(An), d8(An,Xn), abs.W, abs.L, d16(PC), d8(PC,Xn), #imm.
constexpr uint16_t kModeAll = 0x0FFF;
constexpr uint16_t kModeData = kModeAll & ~0x0002;               // not An
constexpr uint16_t kModeMemory = kModeAll & ~0x0003;             // not Dn, An
constexpr uint16_t kModeAlterable = 0x01FF;                      // no PC-relative, no immediate
constexpr uint16_t kModeDataAlt = kModeAlterable & ~0x0002;
constexpr uint16_t kModeMemAlt = kModeAlterable & ~0x0003;
constexpr uint16_t kModeControl = 0x07E4;                        // (An), d16, d8, abs, PC-relative

int modeIndex(int mode, int reg) { return mode < 7 ? mode : reg <= 4 ? 7 + reg : -1; }

int popcount16(uint32_t v) {
    int n = 0;
    for (v &= 0xFFFF; v; v &= v - 1) ++n;
    return n;
}

// Exact DIVU/DIVS timings (Jorge Cwik's analysis of the microcode).
int divuCycles(uint32_t dividend, uint16_t divisor) {
    if ((dividend >> 16) >= divisor) return 10;
    int mcycles = 38;
    const uint32_t hdivisor = static_cast<uint32_t>(divisor) << 16;
    for (int i = 0; i < 15; ++i) {
        const uint32_t temp = dividend;
        dividend <<= 1;
        if (static_cast<int32_t>(temp) < 0) {
            dividend -= hdivisor;
        } else {
            mcycles += 2;
            if (dividend >= hdivisor) {
                dividend -= hdivisor;
                --mcycles;
            }
        }
    }
    return mcycles * 2;
}

int divsCycles(int32_t dividend, int16_t divisor) {
    int mcycles = 6;
    if (dividend < 0) ++mcycles;
    const uint32_t absDividend = dividend < 0 ? 0u - static_cast<uint32_t>(dividend) : static_cast<uint32_t>(dividend);
    const uint32_t absDivisor = divisor < 0 ? static_cast<uint32_t>(-static_cast<int32_t>(divisor)) : static_cast<uint32_t>(divisor);
    if ((absDividend >> 16) >= absDivisor) return (mcycles + 2) * 2;
    uint32_t aquot = absDividend / absDivisor;
    if (aquot > 0x7FFF) return (mcycles + 2) * 2;
    mcycles += 55;
    if (divisor >= 0) {
        if (dividend >= 0) --mcycles;
        else ++mcycles;
    }
    for (int i = 0; i < 15; ++i) {
        if (static_cast<int16_t>(aquot) >= 0) ++mcycles;
        aquot <<= 1;
    }
    return mcycles * 2;
}

}  // namespace

void M68000::reset() {
    halted = false;
    stopped = false;
    sr = 0x2700;
    a[7] = 0;
    otherSp = 0;
    try {
        a[7] = read32(0);
        pc = read32(4);
    } catch (const AddressError&) {
        halted = true;
    }
}

void M68000::setSr(uint16_t value) {
    value &= 0xA71F;
    if ((value ^ sr) & 0x2000) {
        const uint32_t t = a[7];
        a[7] = otherSp;
        otherSp = t;
    }
    sr = value;
}

uint16_t M68000::read16(uint32_t addr, bool instruction) {
    if (addr & 1) throw AddressError{addr, true, instruction};
    return bus_.read16(addr & 0xFFFFFF);
}

uint32_t M68000::read32(uint32_t addr) {
    const uint32_t hi = read16(addr);
    return (hi << 16) | read16(addr + 2);
}

void M68000::write16(uint32_t addr, uint16_t v) {
    if (addr & 1) throw AddressError{addr, false, false};
    bus_.write16(addr & 0xFFFFFF, v);
}

void M68000::write32(uint32_t addr, uint32_t v) {
    write16(addr, static_cast<uint16_t>(v >> 16));
    write16(addr + 2, static_cast<uint16_t>(v));
}

uint16_t M68000::fetch16() {
    const uint16_t v = read16(pc, true);
    pc += 2;
    return v;
}

uint32_t M68000::fetch32() {
    const uint32_t hi = fetch16();
    return (hi << 16) | fetch16();
}

void M68000::push16(uint16_t v) {
    a[7] -= 2;
    write16(a[7], v);
}

void M68000::push32(uint32_t v) {
    a[7] -= 4;
    write32(a[7], v);
}

uint16_t M68000::pop16() {
    const uint16_t v = read16(a[7]);
    a[7] += 2;
    return v;
}

uint32_t M68000::pop32() {
    const uint32_t v = read32(a[7]);
    a[7] += 4;
    return v;
}

int M68000::eaTime(int mode, int reg, bool lng) {
    switch (mode) {
        case 0: case 1: return 0;
        case 2: case 3: return lng ? 8 : 4;
        case 4: return lng ? 10 : 6;
        case 5: return lng ? 12 : 8;
        case 6: return lng ? 14 : 10;
        default:
            switch (reg) {
                case 0: return lng ? 12 : 8;
                case 1: return lng ? 16 : 12;
                case 2: return lng ? 12 : 8;
                case 3: return lng ? 14 : 10;
                case 4: return lng ? 8 : 4;
                default: return 0;
            }
    }
}

bool M68000::validEa(int mode, int reg, uint16_t allowed) {
    const int i = modeIndex(mode, reg);
    return i >= 0 && (allowed >> i) & 1;
}

uint32_t M68000::indexed(uint32_t base) {
    const uint16_t ext = fetch16();
    const int r = (ext >> 12) & 7;
    uint32_t index = (ext & 0x8000) ? a[r] : d[r];
    if (!(ext & 0x0800)) index = static_cast<uint32_t>(static_cast<int16_t>(index));
    return base + static_cast<uint32_t>(static_cast<int8_t>(ext & 0xFF)) + index;
}

M68000::Ea M68000::resolve(int mode, int reg, int size, bool timed) {
    if (timed) cycles_ += eaTime(mode, reg, size == 4);
    switch (mode) {
        case 0: return {Ea::DataReg, static_cast<uint8_t>(reg), 0};
        case 1: return {Ea::AddrReg, static_cast<uint8_t>(reg), 0};
        case 2: return {Ea::Memory, 0, a[reg]};
        case 3: {
            const uint32_t addr = a[reg];
            a[reg] += (size == 1 && reg == 7) ? 2 : size;
            return {Ea::Memory, 0, addr};
        }
        case 4:
            a[reg] -= (size == 1 && reg == 7) ? 2 : size;
            return {Ea::Memory, 0, a[reg]};
        case 5: {
            const uint32_t base = a[reg];
            return {Ea::Memory, 0, base + static_cast<uint32_t>(static_cast<int16_t>(fetch16()))};
        }
        case 6: return {Ea::Memory, 0, indexed(a[reg])};
        default:
            switch (reg) {
                case 0: return {Ea::Memory, 0, static_cast<uint32_t>(static_cast<int16_t>(fetch16()))};
                case 1: return {Ea::Memory, 0, fetch32()};
                case 2: {
                    const uint32_t base = pc;
                    return {Ea::Memory, 0, base + static_cast<uint32_t>(static_cast<int16_t>(fetch16()))};
                }
                case 3: return {Ea::Memory, 0, indexed(pc)};
                default: {
                    // Immediate data: the value itself goes in addr.
                    uint32_t v = size == 4 ? fetch32() : fetch16();
                    if (size == 1) v &= 0xFF;
                    return {Ea::Immediate, 0, v};
                }
            }
    }
}

uint32_t M68000::read(const Ea& ea, int size) {
    switch (ea.kind) {
        case Ea::DataReg: return d[ea.reg] & maskOf(size);
        case Ea::AddrReg: return a[ea.reg] & maskOf(size);
        case Ea::Immediate: return ea.addr & maskOf(size);
        default:
            if (size == 1) return read8(ea.addr);
            if (size == 2) return read16(ea.addr);
            return read32(ea.addr);
    }
}

void M68000::write(const Ea& ea, int size, uint32_t v) {
    switch (ea.kind) {
        case Ea::DataReg: {
            const uint32_t m = maskOf(size);
            d[ea.reg] = (d[ea.reg] & ~m) | (v & m);
            break;
        }
        case Ea::AddrReg: a[ea.reg] = v; break;
        case Ea::Immediate: break;
        default:
            if (size == 1) write8(ea.addr, static_cast<uint8_t>(v));
            else if (size == 2) write16(ea.addr, static_cast<uint16_t>(v));
            else write32(ea.addr, v);
    }
}

bool M68000::cond(int cc) const {
    const bool c = sr & kC, v = sr & kV, z = sr & kZ, n = sr & kN;
    switch (cc) {
        case 0: return true;
        case 1: return false;
        case 2: return !c && !z;    // HI
        case 3: return c || z;      // LS
        case 4: return !c;          // CC
        case 5: return c;           // CS
        case 6: return !z;          // NE
        case 7: return z;           // EQ
        case 8: return !v;          // VC
        case 9: return v;           // VS
        case 10: return !n;         // PL
        case 11: return n;          // MI
        case 12: return n == v;     // GE
        case 13: return n != v;     // LT
        case 14: return !z && n == v;  // GT
        default: return z || n != v;   // LE
    }
}

void M68000::setNZ(uint32_t v, int size) {
    sr &= ~(kN | kZ | kV | kC);
    if (!(v & maskOf(size))) sr |= kZ;
    if (v & msbOf(size)) sr |= kN;
}

uint32_t M68000::add(uint32_t src, uint32_t dst, int size, bool withX) {
    const uint32_t m = maskOf(size), hi = msbOf(size);
    src &= m;
    dst &= m;
    const uint64_t wide = static_cast<uint64_t>(src) + dst + (withX && (sr & kX) ? 1 : 0);
    const uint32_t res = static_cast<uint32_t>(wide) & m;
    uint16_t f = sr & ~(kN | kV | kC | kX | (withX ? 0 : kZ));
    if (res & hi) f |= kN;
    if (withX) {
        if (res) f &= ~kZ;
    } else if (!res) {
        f |= kZ;
    }
    if ((src ^ res) & (dst ^ res) & hi) f |= kV;
    if ((wide >> (size * 8)) & 1) f |= kC | kX;
    sr = f;
    return res;
}

uint32_t M68000::sub(uint32_t src, uint32_t dst, int size, bool withX, bool setX) {
    const uint32_t m = maskOf(size), hi = msbOf(size);
    src &= m;
    dst &= m;
    const uint64_t wide = static_cast<uint64_t>(dst) - src - (withX && (sr & kX) ? 1 : 0);
    const uint32_t res = static_cast<uint32_t>(wide) & m;
    uint16_t f = sr & ~(kN | kV | kC | (setX ? kX : 0) | (withX ? 0 : kZ));
    if (res & hi) f |= kN;
    if (withX) {
        if (res) f &= ~kZ;
    } else if (!res) {
        f |= kZ;
    }
    if ((src ^ dst) & (res ^ dst) & hi) f |= kV;
    if ((wide >> (size * 8)) & 1) f |= kC | (setX ? kX : 0);
    sr = f;
    return res;
}

uint8_t M68000::abcd(uint8_t src, uint8_t dst) {
    const int x = (sr & kX) ? 1 : 0;
    const int unadjusted = src + dst + x;
    int adjust = 0;
    if ((src & 0x0F) + (dst & 0x0F) + x > 9) adjust = 0x06;
    bool carry = false;
    if (unadjusted > 0x99) {
        adjust |= 0x60;
        carry = true;
    }
    const int result = (unadjusted + adjust) & 0xFF;
    uint16_t f = sr & ~(kN | kV | kC | kX);
    if (carry) f |= kC | kX;
    if (~unadjusted & result & 0x80) f |= kV;
    if (result & 0x80) f |= kN;
    if (result) f &= ~kZ;
    sr = f;
    return static_cast<uint8_t>(result);
}

uint8_t M68000::sbcd(uint8_t src, uint8_t dst) {
    const int x = (sr & kX) ? 1 : 0;
    const int unadjusted = dst - src - x;
    int adjust = 0;
    if ((dst & 0x0F) - (src & 0x0F) - x < 0) adjust = 0x06;
    bool carry = false;
    if (unadjusted < 0) {
        adjust |= 0x60;
        carry = true;
    }
    const int full = unadjusted - adjust;
    if (full < -0xFF || (full < 0 && !carry)) carry = true;
    const int result = full & 0xFF;
    uint16_t f = sr & ~(kN | kV | kC | kX);
    if (carry) f |= kC | kX;
    if (unadjusted & ~result & 0x80) f |= kV;
    if (result & 0x80) f |= kN;
    if (result) f &= ~kZ;
    sr = f;
    return static_cast<uint8_t>(result);
}

void M68000::exception(int vector, uint32_t returnPc) {
    const uint16_t old = sr;
    setSr(static_cast<uint16_t>((sr | 0x2000) & ~0x8000));
    push32(returnPc);
    push16(old);
    pc = read32(static_cast<uint32_t>(vector) * 4);
}

void M68000::addressErrorException(const AddressError& e) {
    ++addressErrors;
    const uint16_t old = sr;
    setSr(static_cast<uint16_t>((sr | 0x2000) & ~0x8000));
    const uint16_t fc = static_cast<uint16_t>(((old & 0x2000) ? 4 : 0) | (e.instruction ? 2 : 1));
    const uint16_t status = static_cast<uint16_t>((ir_ & 0xFFE0) | (e.read ? 0x10 : 0) | (e.instruction ? 0x08 : 0) | fc);
    push32(e.instruction ? e.addr - 4 : pc - 2 - faultPcAdjust_);
    push16(old);
    push16(ir_);
    push32(e.addr);
    push16(status);
    pc = read32(12);
    cycles_ += 50;
}

void M68000::jump(uint32_t target) {
    pc = target;
    if (target & 1) throw AddressError{target, true, true};
}

bool M68000::privileged() {
    if (supervisor()) return false;
    pc = instrPc_;
    exception(8, instrPc_);
    cycles_ += 34;
    return true;
}

void M68000::holdMain(bool on) {
    holding = on;
    holdSp = a[7];
    if (on && ((sr >> 8) & 7) >= 6) setSr(static_cast<uint16_t>((sr & ~0x0700) | 0x0300));
}

namespace {
constexpr uint32_t kBackFromRoutine = 0xF00000;  // a return address no code is at
}

void M68000::callRoutine(uint32_t entry, const uint32_t (&regs)[15], uint32_t keep) {
    std::copy(d, d + 8, savedD_);
    std::copy(a, a + 8, savedA_);
    savedOtherSp_ = otherSp;
    savedPc_ = pc;
    savedSr_ = sr;
    savedStopped_ = stopped;
    savedHolding_ = holding;
    for (int i = 0; i < 15; ++i)
        if (regs[i] != keep) (i < 8 ? d[i] : a[i - 8]) = regs[i];
    setSr(static_cast<uint16_t>(sr | 0x2700));
    a[7] -= 0x100;  // clear of what the program had on the stack
    stopped = holding = false;
    calling_ = true;
    try {
        push32(kBackFromRoutine);
    } catch (const AddressError&) {
        returnFromRoutine();
        return;
    }
    pc = entry;
}

void M68000::returnFromRoutine() {
    std::copy(savedD_, savedD_ + 8, d);
    sr = savedSr_;  // with the stack pointers as they were
    std::copy(savedA_, savedA_ + 8, a);
    otherSp = savedOtherSp_;
    pc = savedPc_;
    stopped = savedStopped_;
    holding = savedHolding_;
    calling_ = false;
}

int M68000::step() {
    if (halted) return 4;
    cycles_ = 0;
    try {
        const int mask = (sr >> 8) & 7;
        if (irqLevel_ > mask || irqLevel_ == 7) {
            const int level = irqLevel_;
            stopped = false;
            bus_.irqAcknowledge(level);
            instrPc_ = pc;
            exception(24 + level, pc);
            sr = static_cast<uint16_t>((sr & ~0x0700) | (level << 8));
            return 44;
        }
        if (stopped) return 4;
        if (holding && (!supervisor() || a[7] >= holdSp)) return 4;
        if (calling_ && pc == kBackFromRoutine) {
            returnFromRoutine();
            return 4;
        }
        if (skipRoutine && pc == skipRoutine && !calling_) {
            pc = pop32();  // RTS
            return 16;
        }
        const bool trace = (sr & 0x8000) != 0;
        instrPc_ = pc;
        faultPcAdjust_ = 0;
        ir_ = fetch16();
        execute(ir_);
        if (trace) {
            exception(9, pc);
            cycles_ += 34;
        }
    } catch (const AddressError& e) {
        try {
            addressErrorException(e);
        } catch (const AddressError&) {
            halted = true;  // an address error while handling one: double fault
        }
    }
    return cycles_;
}

void M68000::execute(uint16_t op) {
    switch (op >> 12) {
        case 0x0: group0(op); break;
        case 0x1: case 0x2: case 0x3: move(op); break;
        case 0x4: group4(op); break;
        case 0x5: group5(op); break;
        case 0x6: branch(op); break;
        case 0x7:
            if (op & 0x100) {
                illegal();
                cycles_ += 34;
                break;
            }
            d[(op >> 9) & 7] = static_cast<uint32_t>(static_cast<int8_t>(op & 0xFF));
            setNZ(d[(op >> 9) & 7], 4);
            cycles_ += 4;
            break;
        case 0xA:
            exception(10, instrPc_);
            cycles_ += 34;
            break;
        case 0xE: shift(op); break;
        case 0xF:
            exception(11, instrPc_);
            cycles_ += 34;
            break;
        default: arith(op); break;
    }
}

void M68000::group0(uint16_t op) {
    const int mode = (op >> 3) & 7, reg = op & 7;
    auto bad = [&] {
        illegal();
        cycles_ += 34;
    };

    if ((op & 0x0100) && mode == 1) {
        // MOVEP: bytes to or from every other address.
        const int dx = (op >> 9) & 7;
        const uint32_t addr = a[reg] + static_cast<uint32_t>(static_cast<int16_t>(fetch16()));
        switch ((op >> 6) & 3) {
            case 0:
                d[dx] = (d[dx] & 0xFFFF0000u) | (read8(addr) << 8) | read8(addr + 2);
                cycles_ += 16;
                break;
            case 1:
                d[dx] = (static_cast<uint32_t>(read8(addr)) << 24) | (read8(addr + 2) << 16) | (read8(addr + 4) << 8) |
                        read8(addr + 6);
                cycles_ += 24;
                break;
            case 2:
                write8(addr, static_cast<uint8_t>(d[dx] >> 8));
                write8(addr + 2, static_cast<uint8_t>(d[dx]));
                cycles_ += 16;
                break;
            default:
                write8(addr, static_cast<uint8_t>(d[dx] >> 24));
                write8(addr + 2, static_cast<uint8_t>(d[dx] >> 16));
                write8(addr + 4, static_cast<uint8_t>(d[dx] >> 8));
                write8(addr + 6, static_cast<uint8_t>(d[dx]));
                cycles_ += 24;
                break;
        }
        return;
    }

    const bool dynamicBit = (op & 0x0100) != 0;
    if (dynamicBit || ((op >> 9) & 7) == 4) {
        // BTST / BCHG / BCLR / BSET with the bit number in Dn or in an immediate byte.
        const int type = (op >> 6) & 3;
        uint16_t allowed = type == 0 ? kModeData : kModeDataAlt;
        if (!dynamicBit && type == 0) allowed = kModeData & ~0x0800;  // no immediate for static BTST
        if (!validEa(mode, reg, allowed)) return bad();
        uint32_t bit = dynamicBit ? d[(op >> 9) & 7] : (fetch16() & 0xFF);
        const int size = mode == 0 ? 4 : 1;
        bit &= mode == 0 ? 31 : 7;
        const Ea ea = resolve(mode, reg, 1);
        const uint32_t v = read(ea, size);
        const uint32_t m = 1u << bit;
        if (v & m) sr &= ~kZ;
        else sr |= kZ;
        uint32_t r = v;
        switch (type) {
            case 1: r = v ^ m; break;
            case 2: r = v & ~m; break;
            case 3: r = v | m; break;
            default: break;
        }
        if (type) write(ea, size, r);
        const int base = dynamicBit ? 0 : 4;
        if (mode == 0) {
            if (type == 0) cycles_ += 6 + base;
            else if (type == 2) cycles_ += (bit < 16 ? 8 : 10) + base;
            else cycles_ += (bit < 16 ? 6 : 8) + base;
        } else {
            cycles_ += (type == 0 ? (mode == 7 && reg == 4 ? 6 : 4) : 8) + base;
        }
        return;
    }

    const int kind = (op >> 9) & 7;
    const int sizeBits = (op >> 6) & 3;
    if (kind == 7 || sizeBits == 3) return bad();
    const int size = sizeFromBits(sizeBits);

    if (mode == 7 && reg == 4) {
        // ORI / ANDI / EORI to CCR (byte) or SR (word).
        if ((kind != 0 && kind != 1 && kind != 5) || size == 4) return bad();
        if (size == 2 && privileged()) return;
        const uint16_t imm = fetch16();
        uint16_t v = sr;
        if (kind == 0) v |= imm;
        else if (kind == 1) v &= imm;
        else v ^= imm;
        if (size == 1) setSr(static_cast<uint16_t>((sr & 0xFF00) | (v & 0x1F)));
        else setSr(v);
        cycles_ += 20;
        return;
    }
    if (!validEa(mode, reg, kModeDataAlt)) return bad();
    const uint32_t imm = (size == 4 ? fetch32() : fetch16()) & maskOf(size);
    const Ea ea = resolve(mode, reg, size);
    const uint32_t v = read(ea, size);
    uint32_t r = 0;
    switch (kind) {
        case 0: r = v | imm; setNZ(r, size); break;
        case 1: r = v & imm; setNZ(r, size); break;
        case 2: r = sub(imm, v, size, false); break;
        case 3: r = add(imm, v, size, false); break;
        case 5: r = v ^ imm; setNZ(r, size); break;
        default: sub(imm, v, size, false, false); break;  // CMPI
    }
    if (kind != 6) write(ea, size, r);
    if (kind == 6) cycles_ += mode == 0 ? (size == 4 ? 14 : 8) : (size == 4 ? 12 : 8);
    else cycles_ += mode == 0 ? (size == 4 ? 16 : 8) : (size == 4 ? 20 : 12);
}

void M68000::move(uint16_t op) {
    const int size = (op >> 12) == 1 ? 1 : (op >> 12) == 3 ? 2 : 4;
    const int srcMode = (op >> 3) & 7, srcReg = op & 7;
    const int dstMode = (op >> 6) & 7, dstReg = (op >> 9) & 7;
    if (!validEa(srcMode, srcReg, kModeAll) || (size == 1 && srcMode == 1) ||
        !validEa(dstMode, dstReg, kModeAlterable) || (size == 1 && dstMode == 1)) {
        illegal();
        cycles_ += 34;
        return;
    }
    const Ea src = resolve(srcMode, srcReg, size);
    const uint32_t v = read(src, size);
    cycles_ += 4;
    if (dstMode == 1) {  // MOVEA: sign-extended, flags untouched
        a[dstReg] = signExtend(v, size);
        return;
    }
    // Destination timing differs from the source table: -(An) costs the same as (An).
    static constexpr int kDstWord[] = {0, 0, 4, 4, 4, 8, 10};
    if (dstMode < 7) cycles_ += kDstWord[dstMode] + (size == 4 && dstMode >= 2 ? 4 : 0);
    else cycles_ += (dstReg == 0 ? 8 : 12) + (size == 4 ? 4 : 0);
    // Flags are set before the write, so they survive a fault on it. A misaligned
    // (An)+ destination faults before the increment; a long -(An) one writes its low
    // word first, two bytes down.
    setNZ(v, size);
    if (dstMode == 7 && dstReg == 1) faultPcAdjust_ = 2;
    if (dstMode == 4) faultPcAdjust_ = -2;
    if (size > 1 && (a[dstReg] & 1)) {
        if (dstMode == 3) throw AddressError{a[dstReg], false, false};
        if (dstMode == 4 && size == 4) {
            a[dstReg] -= 2;
            throw AddressError{a[dstReg], false, false};
        }
    }
    const Ea dst = resolve(dstMode, dstReg, size, false);
    // MOVE to (xxx).L writes before fetching the address's second word, so a fault
    // stacks a PC one word earlier.
    write(dst, size, v);
}

void M68000::group4(uint16_t op) {
    const int mode = (op >> 3) & 7, reg = op & 7;
    auto bad = [&] {
        illegal();
        cycles_ += 34;
    };

    if (op & 0x0100) {
        const int ax = (op >> 9) & 7;
        if ((op & 0x01C0) == 0x01C0) {  // LEA
            if (!validEa(mode, reg, kModeControl)) return bad();
            a[ax] = resolve(mode, reg, 4, false).addr;
            static constexpr int kLea[] = {0, 0, 4, 0, 0, 8, 12};
            cycles_ += mode < 7 ? kLea[mode] : (reg == 1 ? 12 : reg == 3 ? 12 : 8);
            return;
        }
        if ((op & 0x01C0) == 0x0180) {  // CHK.W <ea>,Dn
            if (!validEa(mode, reg, kModeData)) return bad();
            const Ea ea = resolve(mode, reg, 2);
            const int16_t bound = static_cast<int16_t>(read(ea, 2));
            const int16_t v = static_cast<int16_t>(d[ax]);
            sr &= ~(kZ | kV | kC);
            if (v > bound || v < 0) {
                sr = static_cast<uint16_t>((sr & ~kN) | (v < 0 ? kN : 0));
                exception(6, pc);
                cycles_ += v > bound ? 38 : 40;
            } else {
                cycles_ += 10;
            }
            return;
        }
        return bad();
    }

    switch ((op >> 8) & 0xF) {
        case 0x0: case 0x2: case 0x4: case 0x6: {
            const int sizeBits = (op >> 6) & 3;
            const int kind = (op >> 9) & 3;  // 0 NEGX, 1 CLR, 2 NEG, 3 NOT
            if (sizeBits == 3) {
                if (kind == 0) {  // MOVE from SR
                    if (!validEa(mode, reg, kModeDataAlt)) return bad();
                    const Ea ea = resolve(mode, reg, 2);
                    if (mode != 0) read(ea, 2);  // the 68000 reads before writing
                    write(ea, 2, sr);
                    cycles_ += mode == 0 ? 6 : 8;
                    return;
                }
                if (kind == 1) return bad();
                // MOVE to CCR / MOVE to SR
                if (!validEa(mode, reg, kModeData)) return bad();
                if (kind == 3 && privileged()) return;
                const Ea ea = resolve(mode, reg, 2);
                const uint16_t v = static_cast<uint16_t>(read(ea, 2));
                if (kind == 2) setSr(static_cast<uint16_t>((sr & 0xFF00) | (v & 0x1F)));
                else setSr(v);
                cycles_ += 12;
                return;
            }
            if (!validEa(mode, reg, kModeDataAlt)) return bad();
            const int size = sizeFromBits(sizeBits);
            const Ea ea = resolve(mode, reg, size);
            uint32_t r = 0;
            switch (kind) {
                case 0: r = sub(read(ea, size), 0, size, true); break;
                case 1:
                    read(ea, size);  // the 68000 reads before clearing
                    r = 0;
                    setNZ(0, size);
                    break;
                case 2: r = sub(read(ea, size), 0, size, false); break;
                default: r = ~read(ea, size); setNZ(r, size); break;
            }
            write(ea, size, r);
            cycles_ += mode == 0 ? (size == 4 ? 6 : 4) : (size == 4 ? 12 : 8);
            return;
        }
        case 0x8: {
            const int sub2 = (op >> 6) & 3;
            if (sub2 == 0) {  // NBCD
                if (!validEa(mode, reg, kModeDataAlt)) return bad();
                const Ea ea = resolve(mode, reg, 1);
                write(ea, 1, sbcd(static_cast<uint8_t>(read(ea, 1)), 0));
                cycles_ += mode == 0 ? 6 : 8;
                return;
            }
            if (sub2 == 1) {
                if (mode == 0) {  // SWAP
                    d[reg] = (d[reg] >> 16) | (d[reg] << 16);
                    setNZ(d[reg], 4);
                    cycles_ += 4;
                    return;
                }
                if (!validEa(mode, reg, kModeControl)) return bad();  // PEA
                const uint32_t addr = resolve(mode, reg, 4, false).addr;
                push32(addr);
                static constexpr int kPea[] = {0, 0, 12, 0, 0, 16, 20};
                cycles_ += mode < 7 ? kPea[mode] : (reg == 1 ? 20 : reg == 3 ? 20 : 16);
                return;
            }
            if (mode == 0) {  // EXT.W / EXT.L
                if (sub2 == 2) {
                    d[reg] = (d[reg] & 0xFFFF0000u) | (static_cast<uint32_t>(static_cast<int8_t>(d[reg])) & 0xFFFF);
                    setNZ(d[reg], 2);
                } else {
                    d[reg] = static_cast<uint32_t>(static_cast<int16_t>(d[reg]));
                    setNZ(d[reg], 4);
                }
                cycles_ += 4;
                return;
            }
            return movem(op);
        }
        case 0xA: {
            if (op == 0x4AFC) return bad();  // ILLEGAL
            const int sizeBits = (op >> 6) & 3;
            if (sizeBits == 3) {  // TAS
                if (!validEa(mode, reg, kModeDataAlt)) return bad();
                const Ea ea = resolve(mode, reg, 1);
                const uint32_t v = read(ea, 1);
                setNZ(v, 1);
                if (mode == 0 || bus_.tasWrites()) write(ea, 1, v | 0x80);
                cycles_ += mode == 0 ? 4 : 10;
                return;
            }
            if (!validEa(mode, reg, kModeDataAlt)) return bad();  // TST
            const int size = sizeFromBits(sizeBits);
            const Ea ea = resolve(mode, reg, size);
            setNZ(read(ea, size), size);
            cycles_ += 4;
            return;
        }
        case 0xC: {
            if (((op >> 7) & 1) == 0) return bad();
            return movem(op);
        }
        case 0xE: {
            if ((op & 0xFFF0) == 0x4E40) {  // TRAP #n
                exception(32 + (op & 15), pc);
                cycles_ += 34;
                return;
            }
            if ((op & 0xFFF8) == 0x4E50) {  // LINK An,#d16
                const int16_t disp = static_cast<int16_t>(fetch16());
                push32(reg == 7 ? a[7] - 4 : a[reg]);
                a[reg] = a[7];
                a[7] += static_cast<uint32_t>(disp);
                cycles_ += 16;
                return;
            }
            if ((op & 0xFFF8) == 0x4E58) {  // UNLK An
                a[7] = a[reg];
                const uint32_t v = read32(a[7]);
                a[7] += 4;
                a[reg] = v;
                cycles_ += 12;
                return;
            }
            if ((op & 0xFFF0) == 0x4E60) {  // MOVE An,USP / MOVE USP,An
                if (privileged()) return;
                if (op & 8) a[reg] = otherSp;
                else otherSp = a[reg];
                cycles_ += 4;
                return;
            }
            switch (op) {
                case 0x4E70:  // RESET
                    if (privileged()) return;
                    cycles_ += 132;
                    return;
                case 0x4E71: cycles_ += 4; return;  // NOP
                case 0x4E72: {  // STOP #imm
                    if (privileged()) return;
                    const uint16_t v = fetch16();
                    setSr(v);
                    stopped = true;
                    cycles_ += 4;
                    return;
                }
                case 0x4E73: {  // RTE
                    if (privileged()) return;
                    const uint16_t newSr = pop16();
                    const uint32_t newPc = pop32();
                    setSr(newSr);
                    jump(newPc);
                    cycles_ += 20;
                    return;
                }
                case 0x4E75: jump(pop32()); cycles_ += 16; return;  // RTS
                case 0x4E76:  // TRAPV
                    cycles_ += 4;
                    if (sr & kV) {
                        exception(7, pc);
                        cycles_ += 30;
                    }
                    return;
                case 0x4E77: {  // RTR
                    const uint16_t ccr = pop16();
                    setSr(static_cast<uint16_t>((sr & 0xFF00) | (ccr & 0x1F)));
                    jump(pop32());
                    cycles_ += 20;
                    return;
                }
                default: break;
            }
            if ((op & 0xFF80) == 0x4E80) {  // JSR (bit 6 clear) / JMP (bit 6 set)
                if (!validEa(mode, reg, kModeControl)) return bad();
                const uint32_t target = resolve(mode, reg, 4, false).addr;
                const bool jsr = !(op & 0x40);
                static constexpr int kJmp[] = {0, 0, 8, 0, 0, 10, 14};
                int t = mode < 7 ? kJmp[mode] : (reg == 0 ? 10 : reg == 1 ? 12 : reg == 2 ? 10 : 14);
                if (jsr) {
                    if (target & 1) jump(target);  // faults before pushing anything
                    push32(pc);
                    t += 8;
                }
                jump(target);
                cycles_ += t;
                return;
            }
            return bad();
        }
        default: return bad();
    }
}

void M68000::movem(uint16_t op) {
    const int mode = (op >> 3) & 7, reg = op & 7;
    const bool toRegs = (op & 0x0400) != 0;
    const int size = (op & 0x40) ? 4 : 2;
    const uint16_t allowed = toRegs ? (kModeControl | 0x0008) : (kModeControl & kModeAlterable) | 0x0010;
    if (!validEa(mode, reg, allowed)) {
        illegal();
        cycles_ += 34;
        return;
    }
    const uint16_t list = fetch16();
    int count = 0;
    if (toRegs) {
        uint32_t addr = mode == 3 ? a[reg] : resolve(mode, reg, size, false).addr;
        if (mode == 3 && (addr & 1) && list) {
            a[reg] += 2;
            throw AddressError{addr, true, false};
        }
        for (int i = 0; i < 16; ++i) {
            if (!(list & (1 << i))) continue;
            const uint32_t v = size == 4 ? read32(addr) : signExtend(read16(addr), 2);
            if (i < 8) d[i] = v;
            else a[i - 8] = v;
            addr += size;
            ++count;
        }
        read16(addr);  // the 68000 reads one extra word
        if (mode == 3) a[reg] = addr;
        static constexpr int kBase[] = {0, 0, 12, 12, 0, 16, 18};
        cycles_ += (mode < 7 ? kBase[mode] : (reg == 0 ? 16 : reg == 1 ? 20 : reg == 2 ? 16 : 18)) + count * (size == 4 ? 8 : 4);
    } else if (mode == 4) {
        // Predecrement: the list is reversed (bit 0 = A7) and stored downwards.
        uint32_t addr = a[reg];
        for (int i = 0; i < 16; ++i) {
            if (!(list & (1 << i))) continue;
            const int r = 15 - i;
            const uint32_t v = r < 8 ? d[r] : a[r - 8];
            addr -= size;
            if (size == 4) {  // low word first
                write16(addr + 2, static_cast<uint16_t>(v));
                write16(addr, static_cast<uint16_t>(v >> 16));
            } else {
                write16(addr, static_cast<uint16_t>(v));
            }
            ++count;
        }
        a[reg] = addr;
        cycles_ += 8 + count * (size == 4 ? 8 : 4);
    } else {
        uint32_t addr = resolve(mode, reg, size, false).addr;
        for (int i = 0; i < 16; ++i) {
            if (!(list & (1 << i))) continue;
            const uint32_t v = i < 8 ? d[i] : a[i - 8];
            if (size == 4) write32(addr, v);
            else write16(addr, static_cast<uint16_t>(v));
            addr += size;
            ++count;
        }
        static constexpr int kBase[] = {0, 0, 8, 0, 0, 12, 14};
        cycles_ += (mode < 7 ? kBase[mode] : (reg == 0 ? 12 : 16)) + count * (size == 4 ? 8 : 4);
    }
}

void M68000::group5(uint16_t op) {
    const int mode = (op >> 3) & 7, reg = op & 7;
    const int sizeBits = (op >> 6) & 3;
    if (sizeBits == 3) {
        const int cc = (op >> 8) & 15;
        if (mode == 1) {  // DBcc
            const uint32_t base = pc;
            const int16_t disp = static_cast<int16_t>(fetch16());
            if (cond(cc)) {
                cycles_ += 12;
                return;
            }
            const uint16_t counter = static_cast<uint16_t>(d[reg] - 1);
            d[reg] = (d[reg] & 0xFFFF0000u) | counter;
            if (counter == 0xFFFF) {
                cycles_ += 14;
                return;
            }
            jump(base + static_cast<uint32_t>(disp));
            cycles_ += 10;
            return;
        }
        if (!validEa(mode, reg, kModeDataAlt)) {
            illegal();
            cycles_ += 34;
            return;
        }
        const Ea ea = resolve(mode, reg, 1);
        const bool t = cond(cc);
        if (mode != 0) read(ea, 1);  // Scc reads before writing
        write(ea, 1, t ? 0xFF : 0x00);
        cycles_ += mode == 0 ? (t ? 6 : 4) : 8;
        return;
    }
    const int size = sizeFromBits(sizeBits);
    if (!validEa(mode, reg, kModeAlterable) || (mode == 1 && size == 1)) {
        illegal();
        cycles_ += 34;
        return;
    }
    uint32_t data = (op >> 9) & 7;
    if (data == 0) data = 8;
    const bool isSub = (op & 0x0100) != 0;
    if (mode == 1) {  // to an address register: whole register, no flags
        a[reg] = isSub ? a[reg] - data : a[reg] + data;
        cycles_ += size == 4 ? 6 : 8;
        return;
    }
    const Ea ea = resolve(mode, reg, size);
    const uint32_t v = read(ea, size);
    write(ea, size, isSub ? sub(data, v, size, false) : add(data, v, size, false));
    cycles_ += mode == 0 ? (size == 4 ? 8 : 4) : (size == 4 ? 12 : 8);
}

void M68000::branch(uint16_t op) {
    const int cc = (op >> 8) & 15;
    const uint32_t base = pc;
    uint32_t disp = static_cast<uint32_t>(static_cast<int8_t>(op & 0xFF));
    const bool wordDisp = (op & 0xFF) == 0;
    if (wordDisp) disp = static_cast<uint32_t>(static_cast<int16_t>(fetch16()));
    if (cc == 1) {  // BSR
        push32(pc);
        jump(base + disp);
        cycles_ += 18;
        return;
    }
    if (cond(cc)) {
        jump(base + disp);
        cycles_ += 10;
    } else {
        cycles_ += wordDisp ? 12 : 8;
    }
}

void M68000::arith(uint16_t op) {
    const int group = op >> 12;
    const int mode = (op >> 3) & 7, reg = op & 7;
    const int rx = (op >> 9) & 7;
    const int opmode = (op >> 6) & 7;
    auto bad = [&] {
        illegal();
        cycles_ += 34;
    };

    // Multiply and divide (groups C and 8, opmode 3 and 7).
    if ((group == 0x8 || group == 0xC) && (opmode == 3 || opmode == 7)) {
        if (!validEa(mode, reg, kModeData)) return bad();
        const Ea ea = resolve(mode, reg, 2);
        const uint32_t src = read(ea, 2);
        const bool sign = opmode == 7;
        if (group == 0xC) {
            uint32_t r;
            int n;
            if (sign) {
                r = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(d[rx])) * static_cast<int16_t>(src));
                n = popcount16((src << 1) ^ src);
            } else {
                r = (d[rx] & 0xFFFF) * src;
                n = popcount16(src);
            }
            d[rx] = r;
            setNZ(r, 4);
            cycles_ += 38 + 2 * n;
            return;
        }
        if (src == 0) {
            sr &= ~(kN | kZ | kV | kC);
            exception(5, pc);
            cycles_ += 38;
            return;
        }
        if (!sign) {
            const uint32_t dividend = d[rx];
            cycles_ += divuCycles(dividend, static_cast<uint16_t>(src));
            const uint32_t q = dividend / src, r = dividend % src;
            if (q > 0xFFFF) {
                sr = static_cast<uint16_t>((sr & ~kC) | kV);
                return;
            }
            d[rx] = (r << 16) | q;
            setNZ(q, 2);
        } else {
            const int32_t dividend = static_cast<int32_t>(d[rx]);
            const int16_t divisor = static_cast<int16_t>(src);
            cycles_ += divsCycles(dividend, divisor);
            const int64_t q = static_cast<int64_t>(dividend) / divisor;
            const int64_t r = static_cast<int64_t>(dividend) % divisor;
            if (q > 32767 || q < -32768) {
                sr = static_cast<uint16_t>((sr & ~kC) | kV);
                return;
            }
            d[rx] = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(q) & 0xFFFF);
            setNZ(static_cast<uint32_t>(q), 2);
        }
        return;
    }

    // ABCD / SBCD.
    if ((group == 0x8 || group == 0xC) && opmode == 4 && mode <= 1) {
        const bool isAdd = group == 0xC;
        if (mode == 0) {
            const uint8_t s = static_cast<uint8_t>(d[reg]), t = static_cast<uint8_t>(d[rx]);
            const uint8_t r = isAdd ? abcd(s, t) : sbcd(s, t);
            d[rx] = (d[rx] & 0xFFFFFF00u) | r;
            cycles_ += 6;
        } else {
            const Ea src = resolve(4, reg, 1, false);
            const uint8_t s = static_cast<uint8_t>(read(src, 1));
            const Ea dst = resolve(4, rx, 1, false);
            const uint8_t t = static_cast<uint8_t>(read(dst, 1));
            write(dst, 1, isAdd ? abcd(s, t) : sbcd(s, t));
            cycles_ += 18;
        }
        return;
    }

    // EXG.
    if (group == 0xC && (opmode == 5 || opmode == 6) && mode <= 1) {
        if (opmode == 5 && mode == 0) std::swap(d[rx], d[reg]);
        else if (opmode == 5 && mode == 1) std::swap(a[rx], a[reg]);
        else if (opmode == 6 && mode == 1) std::swap(d[rx], a[reg]);
        else return bad();
        cycles_ += 6;
        return;
    }

    // ADDA / SUBA / CMPA.
    if ((group == 0x9 || group == 0xD || group == 0xB) && (opmode == 3 || opmode == 7)) {
        if (!validEa(mode, reg, kModeAll)) return bad();
        const int size = opmode == 3 ? 2 : 4;
        const Ea ea = resolve(mode, reg, size);
        const uint32_t src = signExtend(read(ea, size), size);
        if (group == 0xB) {
            sub(src, a[rx], 4, false, false);
            cycles_ += 6;
            return;
        }
        a[rx] = group == 0xD ? a[rx] + src : a[rx] - src;
        if (size == 2) cycles_ += 8;
        else cycles_ += (mode <= 1 || (mode == 7 && reg == 4)) ? 8 : 6;
        return;
    }

    const int size = sizeFromBits(opmode & 3);

    // ADDX / SUBX / CMPM.
    if ((group == 0x9 || group == 0xD) && opmode >= 4 && mode <= 1) {
        const bool isAdd = group == 0xD;
        if (mode == 0) {
            const uint32_t r = isAdd ? add(d[reg], d[rx], size, true) : sub(d[reg], d[rx], size, true);
            write({Ea::DataReg, static_cast<uint8_t>(rx), 0}, size, r);
            cycles_ += size == 4 ? 8 : 4;
        } else {
            // Long operands are read low word first, so a misaligned one faults two
            // bytes down.
            auto predec = [&](int r) {
                if (size == 4 && (a[r] & 1)) {
                    a[r] -= 2;
                    throw AddressError{a[r], true, false};
                }
                return resolve(4, r, size, false);
            };
            const Ea src = predec(reg);
            const uint32_t s = read(src, size);
            const Ea dst = predec(rx);
            const uint32_t t = read(dst, size);
            write(dst, size, isAdd ? add(s, t, size, true) : sub(s, t, size, true));
            cycles_ += size == 4 ? 30 : 18;
        }
        return;
    }
    if (group == 0xB && opmode >= 4 && mode == 1) {
        const Ea src = resolve(3, reg, size, false);
        const uint32_t s = read(src, size);
        const Ea dst = resolve(3, rx, size, false);
        const uint32_t t = read(dst, size);
        sub(s, t, size, false, false);
        cycles_ += size == 4 ? 20 : 12;
        return;
    }

    if (opmode < 4) {
        // <ea>,Dn: OR, SUB, CMP, AND, ADD.
        const bool logic = group == 0x8 || group == 0xC;
        if (!validEa(mode, reg, logic ? kModeData : kModeAll) || (mode == 1 && size == 1)) return bad();
        const Ea ea = resolve(mode, reg, size);
        const uint32_t s = read(ea, size);
        const uint32_t t = d[rx];
        uint32_t r = 0;
        switch (group) {
            case 0x8: r = s | t; setNZ(r, size); break;
            case 0xC: r = s & t; setNZ(r, size); break;
            case 0x9: r = sub(s, t, size, false); break;
            case 0xD: r = add(s, t, size, false); break;
            default: sub(s, t, size, false, false); break;  // CMP
        }
        if (group != 0xB) write({Ea::DataReg, static_cast<uint8_t>(rx), 0}, size, r);
        if (size != 4) cycles_ += 4;
        else if (group == 0xB) cycles_ += 6;
        else cycles_ += (mode <= 1 || (mode == 7 && reg == 4)) ? 8 : 6;
        return;
    }

    // Dn,<ea>: OR, SUB, EOR, AND, ADD.
    if (group == 0xB) {  // EOR allows a data register destination
        if (!validEa(mode, reg, kModeDataAlt)) return bad();
    } else if (!validEa(mode, reg, kModeMemAlt)) {
        return bad();
    }
    const Ea ea = resolve(mode, reg, size);
    const uint32_t t = read(ea, size);
    const uint32_t s = d[rx];
    uint32_t r = 0;
    switch (group) {
        case 0x8: r = s | t; setNZ(r, size); break;
        case 0xC: r = s & t; setNZ(r, size); break;
        case 0xB: r = s ^ t; setNZ(r, size); break;
        case 0x9: r = sub(s, t, size, false); break;
        default: r = add(s, t, size, false); break;
    }
    write(ea, size, r);
    if (mode == 0) cycles_ += size == 4 ? 8 : 4;
    else cycles_ += size == 4 ? 12 : 8;
}

void M68000::shift(uint16_t op) {
    const int sizeBits = (op >> 6) & 3;
    int type, count, size;
    bool left = (op & 0x0100) != 0;
    Ea ea;
    if (sizeBits == 3) {  // memory: one bit, word
        const int mode = (op >> 3) & 7, reg = op & 7;
        if ((op & 0x0800) || !validEa(mode, reg, kModeMemAlt)) {
            illegal();
            cycles_ += 34;
            return;
        }
        type = (op >> 9) & 3;
        count = 1;
        size = 2;
        ea = resolve(mode, reg, 2);
        cycles_ += 8;
    } else {
        type = (op >> 3) & 3;
        size = sizeFromBits(sizeBits);
        const int cr = (op >> 9) & 7;
        count = (op & 0x20) ? static_cast<int>(d[cr] & 63) : (cr ? cr : 8);
        ea = {Ea::DataReg, static_cast<uint8_t>(op & 7), 0};
        cycles_ += (size == 4 ? 8 : 6) + 2 * count;
    }

    const uint32_t m = maskOf(size), hi = msbOf(size);
    uint32_t v = read(ea, size) & m;
    bool x = (sr & kX) != 0, c = false, overflow = false;
    for (int i = 0; i < count; ++i) {
        switch (type) {
            case 0:  // AS
            case 1:  // LS
                if (left) {
                    c = (v & hi) != 0;
                    v = (v << 1) & m;
                    if (type == 0 && ((v & hi) != 0) != c) overflow = true;
                } else {
                    c = (v & 1) != 0;
                    v = type == 0 ? (v >> 1) | (v & hi) : v >> 1;
                }
                x = c;
                break;
            case 2:  // ROX
                if (left) {
                    c = (v & hi) != 0;
                    v = ((v << 1) | (x ? 1 : 0)) & m;
                } else {
                    c = (v & 1) != 0;
                    v = (v >> 1) | (x ? hi : 0);
                }
                x = c;
                break;
            default:  // RO
                if (left) {
                    c = (v & hi) != 0;
                    v = ((v << 1) | (c ? 1 : 0)) & m;
                } else {
                    c = (v & 1) != 0;
                    v = (v >> 1) | (c ? hi : 0);
                }
                break;
        }
    }
    if (type == 0 && !left && count > size * 8) c = x = false;
    uint16_t f = sr & ~(kN | kZ | kV | kC);
    if (!v) f |= kZ;
    if (v & hi) f |= kN;
    if (overflow) f |= kV;
    if (count == 0) {
        if (type == 2 && x) f |= kC;
    } else {
        if (c) f |= kC;
        if (type != 3) f = static_cast<uint16_t>((f & ~kX) | (x ? kX : 0));
    }
    sr = f;
    write(ea, size, v);
}

}  // namespace plumsg
