#include "svp.h"

namespace plumsg {

// What is known of the SSP1601 and the SVP comes from Tasco Deluxe's and notaz's reverse
// engineering of Virtua Racing (as MAME documents it). Only what the game uses is here:
// of the flags only Z and N, of the conditions "always", Z and N.

bool Svp::detect(const Cartridge& cart) {
    const auto& rom = cart.rom();
    return rom.size() > 0x1C9 && rom[0x1C8] == 'S' && rom[0x1C9] == 'V';
}

void Svp::powerOn() {
    dram_.fill(0);
    iram_.fill(0);
    ram_.fill(0);
    reset();
}

void Svp::reset() {
    // The chip's own ROM checks the cartridge and jumps to its program at word 0x400,
    // with the registers cleared; it is not needed to get there.
    x_ = y_ = 0;
    a_ = 0;
    st_ = 0;
    stack_.fill(0);
    sp_ = 0;
    pc_ = 0x400;
    r_.fill(0);
    pm0_ = pm1_ = pm2_ = xst_ = pm4_ = 0;
    pmc_ = 0;
    pmcState_ = 0;
    pmac_[0].fill(0);
    pmac_[1].fill(0);
    ticks_ = 0;
}

void Svp::rebaseClock() { ticks_ = 0; }

// The DSP runs at 3/7 of the master clock (23 MHz), one cycle for most instructions.
void Svp::runUntil(uint64_t masterClock) {
    const uint64_t target = masterClock * 3;
    while (ticks_ < target) ticks_ += static_cast<uint64_t>(step()) * 7;
}

// ---------------------------------------------------------------------------------
// The 68000's side

bool Svp::read68k(uint32_t addr, uint16_t& value) {
    if (addr >= 0x300000 && addr < 0x320000) {
        value = dram_[(addr >> 1) & 0xFFFF];
        return true;
    }
    // The DRAM read as tiles: two arrangements of the same words, made for the VDP.
    if (addr >= 0x390000 && addr < 0x3A0000) {
        const uint32_t a = (addr & 0xE002) | ((addr & 0x7C) << 6) | ((addr & 0x1F80) >> 5);
        value = dram_[a >> 1];
        return true;
    }
    if (addr >= 0x3A0000 && addr < 0x3B0000) {
        const uint32_t a = (addr & 0xF002) | ((addr & 0x3C) << 6) | ((addr & 0xFC0) >> 4);
        value = dram_[a >> 1];
        return true;
    }
    if (addr >= 0xA15000 && addr < 0xA15008) {
        switch (addr & 6) {
            case 0: case 2: value = xst_; break;
            case 4:  // reading the status takes the "DSP wrote XST" bit
                value = pm0_;
                pm0_ &= ~1;
                break;
            default: value = 0; break;
        }
        return true;
    }
    return false;
}

bool Svp::write68k(uint32_t addr, uint16_t value, bool upper, bool lower) {
    if (addr >= 0x300000 && addr < 0x320000) {
        uint16_t& w = dram_[(addr >> 1) & 0xFFFF];
        if (upper) w = static_cast<uint16_t>((w & 0x00FF) | (value & 0xFF00));
        if (lower) w = static_cast<uint16_t>((w & 0xFF00) | (value & 0x00FF));
        return true;
    }
    if (addr >= 0xA15000 && addr < 0xA15008) {
        if ((addr & 6) < 4) {
            xst_ = value;
            pm0_ |= 2;
        }
        return true;  // 0xA15006 (perhaps halting the DSP) is not used by the game
    }
    return false;
}

bool Svp::dmaRead(uint32_t addr, uint16_t& value) {
    if (addr >= 0x400000) return false;
    const uint32_t a = (addr - 2) & 0x3FFFFE;
    if (a >= 0x300000 && a < 0x320000) value = dram_[(a >> 1) & 0xFFFF];
    else value = a < cart_.rom().size() ? static_cast<uint16_t>(cart_.rom()[a] << 8 | cart_.rom()[a + 1]) : 0;
    return true;
}

// ---------------------------------------------------------------------------------
// The DSP

uint16_t Svp::romWord(uint32_t index) const {
    const auto& rom = cart_.rom();
    const size_t a = static_cast<size_t>(index) * 2;
    return a + 1 < rom.size() ? static_cast<uint16_t>(rom[a] << 8 | rom[a + 1]) : 0;
}

uint16_t Svp::programWord(uint16_t addr) const { return addr < 0x400 ? iram_[addr] : romWord(addr); }

void Svp::push(uint16_t v) {
    if (sp_ >= stack_.size()) sp_ = 0;
    stack_[sp_++] = v;
}

uint16_t Svp::pop() {
    sp_ = sp_ == 0 ? static_cast<uint8_t>(stack_.size() - 1) : static_cast<uint8_t>(sp_ - 1);
    return stack_[sp_];
}

uint16_t Svp::readReg(int r) {
    switch (r) {
        case kZero: return 0xFFFF;
        case kX: return x_;
        case kY: return y_;
        case kA: return static_cast<uint16_t>(a_ >> 16);
        case kSt: return st_;
        case kStack: return pop();
        case kPc: return pc_;
        case kP: return static_cast<uint16_t>(static_cast<uint32_t>(product()) >> 16);
        case kPmc: {
            // Reading PMC also steps it, from "address" to "mode": the second read gives
            // the address with its nibbles turned round.
            const uint16_t addr = static_cast<uint16_t>(pmc_);
            if (pmcState_ == 1) {
                pmcState_ = 2;
                return static_cast<uint16_t>((addr << 4 & 0xFFF0) | (addr >> 4 & 0xF));
            }
            pmcState_ = 1;
            return addr;
        }
        case kAl:
            // "ld -, AL" cancels a PMC set-up that was not used.
            if (op_ == 0x000F) pmcState_ = 0;
            return static_cast<uint16_t>(a_);
        default: return readPm(r - kPm0);
    }
}

void Svp::writeReg(int r, uint16_t v) {
    switch (r) {
        case kZero: case kP: break;
        case kX: x_ = v; break;
        case kY: y_ = v; break;
        case kA: a_ = (a_ & 0xFFFF) | static_cast<uint32_t>(v) << 16; break;
        case kSt: st_ = v; break;
        case kStack: push(v); break;
        case kPc: pc_ = v; break;
        case kPmc:
            if (pmcState_ == 1) {
                pmcState_ = 2;
                pmc_ = (pmc_ & 0xFFFF) | static_cast<uint32_t>(v) << 16;
            } else {
                pmcState_ = 1;
                pmc_ = (pmc_ & 0xFFFF0000) | v;
            }
            break;
        case kAl: a_ = (a_ & 0xFFFF0000) | v; break;
        default: writePm(r - kPm0, v); break;
    }
}

// A PMx access once PMC is set (a "blind" ld -, PMx or ld PMx, - programs it), or, for PM4
// always and the others while ST's GP0 bits are set, one through the memory controller.
uint32_t Svp::pmAccess(int reg, bool write, uint16_t v, bool& handled) {
    handled = true;
    if (pmcState_ == 2) {
        pmcState_ = 0;
        const bool blind = (op_ & 0xFF0F) == 0 || (op_ & 0xFFF0) == 0;
        if (blind) pmac_[write][reg] = pmc_;
        return 0;
    }
    pmcState_ = 0;
    if (reg != 4 && !(st_ & 0x60)) {
        handled = false;
        return 0;
    }
    uint32_t& p = pmac_[write][reg];
    const uint16_t mode = static_cast<uint16_t>(p >> 16), addr = static_cast<uint16_t>(p);
    // The step after each access: 0, 1, 2, 4, 8, 16, 32 or 128 words, down if bit 15.
    auto step = [mode]() -> int32_t {
        int n = mode >> 11 & 7;
        if (n == 0) return 0;
        const int32_t s = 1 << (n == 7 ? 7 : n - 1);
        return (mode & 0x8000) ? -s : s;
    };
    uint32_t result = 0;
    if (write) {
        if ((mode & 0x43FF) == 0x0018 || (mode & 0xFBFF) == 0x4018) {  // DRAM
            uint16_t& w = dram_[addr];
            if (mode & 0x0400) {  // overwrite: nibbles of 0 leave the old ones
                for (uint16_t m = 0xF000; m; m >>= 4)
                    if (v & m) w = static_cast<uint16_t>((w & ~m) | (v & m));
            } else {
                w = v;
            }
            // Bit 14: "cell" order, the next word of a tile's row then the next row.
            p += (mode & 0x4000) ? ((addr & 1) ? 31 : 1) : static_cast<uint32_t>(step());
        } else if ((mode & 0x47FF) == 0x001C) {  // instruction RAM
            iram_[addr & 0x3FF] = v;
            p += static_cast<uint32_t>(step());
        }
    } else {
        if ((mode & 0xFFF0) == 0x0800) {  // ROM
            result = romWord(addr | static_cast<uint32_t>(mode & 0xF) << 16);
            ++p;
        } else if ((mode & 0x47FF) == 0x0018) {  // DRAM
            result = dram_[addr];
            p += static_cast<uint32_t>(step());
        }
    }
    pmc_ = p;  // PMC reads back the last one used
    return result;
}

uint16_t Svp::readPm(int reg) {
    bool handled = false;
    const uint16_t v = static_cast<uint16_t>(pmAccess(reg, false, 0, handled));
    if (handled) return v;
    switch (reg) {
        case 0: {  // the mailbox status; reading it takes the "68000 wrote XST" bit
            const uint16_t s = pm0_;
            pm0_ &= ~2;
            return s;
        }
        case 1: return pm1_;
        case 2: return pm2_;
        case 3: return xst_;
        default: return 0;
    }
}

void Svp::writePm(int reg, uint16_t v) {
    bool handled = false;
    pmAccess(reg, true, v, handled);
    if (handled) return;
    switch (reg) {
        case 0: pm0_ = v; break;
        case 1: pm1_ = v; break;
        case 2: pm2_ = v; break;
        case 3:
            xst_ = v;
            pm0_ |= 1;
            break;
        default: break;
    }
}

// (ri): RAM0 through r0-r2 or RAM1 through r4-r6, then by `mod` left alone, stepped up
// ("+!"), down ("-") or up ("+"), the last two within the power of two that ST's low bits
// give. r3 and r7 do not move: with them `mod` picks one of the bank's first four words.
uint16_t& Svp::pointed(int ri, int bank, int mod, bool reading) {
    uint16_t* ram = &ram_[static_cast<size_t>(bank) * 0x100];
    if (ri == 3) return ram[mod];
    uint8_t& r = r_[static_cast<size_t>(bank * 4 + ri)];
    uint16_t& w = ram[r];
    if (mod == 0) return w;
    const int add = mod == 2 ? -1 : 1;
    const int rpl = st_ & 7;
    if (mod == 1 || !reading || rpl == 0) {
        r = static_cast<uint8_t>(r + add);
    } else {
        const uint8_t mask = static_cast<uint8_t>((1 << rpl) - 1);
        r = static_cast<uint8_t>((r & ~mask) | ((r + add) & mask));
    }
    return w;
}

// ((ri)): the program word at the address in (ri), which steps up.
uint16_t Svp::readProgramPointer(uint16_t op) {
    const int ri = op & 3, bank = op >> 8 & 1;
    uint16_t* ram = &ram_[static_cast<size_t>(bank) * 0x100];
    uint16_t& w = ri == 3 ? ram[op >> 2 & 3] : ram[r_[static_cast<size_t>(bank * 4 + ri)]];
    return programWord(w++);
}

bool Svp::condition(uint16_t op) const {
    switch (op & 0xF0) {
        case 0x00: return true;
        case 0x50: return ((st_ & kFlagZ) != 0) == ((op & 0x100) != 0);
        case 0x70: return ((st_ & kFlagN) != 0) == ((op & 0x100) != 0);
        default: return false;
    }
}

// The accumulator operations, by the top 3 bits of the opcode.
void Svp::alu(int op, uint32_t v) {
    switch (op) {
        case 1: a_ -= v; setFlags(a_); break;                  // sub
        case 3: setFlags(a_ - v); break;                       // cmp
        case 4: a_ += v; setFlags(a_); break;                  // add
        case 5: a_ &= v; setZn(); break;                       // and
        case 6: a_ |= v; setZn(); break;                       // or
        case 7: a_ ^= v; setZn(); break;                       // eor
        default: break;
    }
}

int Svp::step() {
    const uint16_t op = op_ = fetch();
    int cycles = 1;
    const int d = op >> 4 & 0xF, s = op & 0xF;
    switch (op >> 9) {
        case 0x00:  // ld d, s ("ld A, P" moves all 32 bits)
            if (op == 0) break;
            if (d == kA && s == kP) a_ = static_cast<uint32_t>(product());
            else writeReg(d, readReg(s));
            break;
        case 0x01: writeReg(d, readPointer(op)); break;                        // ld d, (ri)
        case 0x02: writePointer(op, readReg(d)); break;                        // ld (ri), s
        case 0x04: writeReg(d, fetch()); ++cycles; break;                      // ldi d, imm
        case 0x05: writeReg(d, readProgramPointer(op)); cycles += 2; break;    // ld d, ((ri))
        case 0x06: writePointer(op, fetch()); ++cycles; break;                 // ldi (ri), imm
        case 0x07: ram_[op & 0x1FF] = static_cast<uint16_t>(a_ >> 16); break;  // ld adr, A
        case 0x09: writeReg(d, r_[(op & 3) | (op >> 6 & 4)]); break;            // ld d, ri
        case 0x0A: r_[(op & 3) | (op >> 6 & 4)] = static_cast<uint8_t>(readReg(d)); break;  // ld ri, s
        case 0x0C: case 0x0D: case 0x0E: case 0x0F:                            // ldi ri, simm
            r_[op >> 8 & 7] = static_cast<uint8_t>(op);
            break;
        case 0x24: {  // call cond, addr
            const uint16_t to = fetch();
            if (condition(op)) {
                push(pc_);
                pc_ = to;
            }
            ++cycles;
            break;
        }
        case 0x25: writeReg(d, programWord(static_cast<uint16_t>(a_ >> 16))); cycles += 2; break;  // ld d, (A)
        case 0x26: {  // bra cond, addr
            const uint16_t to = fetch();
            if (condition(op)) pc_ = to;
            ++cycles;
            break;
        }
        case 0x48:  // mod cond, op
            if (condition(op)) {
                switch (op & 7) {
                    case 2: a_ = static_cast<uint32_t>(static_cast<int32_t>(a_) >> 1); break;  // shr
                    case 3: a_ <<= 1; break;                                                   // shl
                    case 6: a_ = 0u - a_; break;                                               // neg
                    case 7: if (static_cast<int32_t>(a_) < 0) a_ = 0u - a_; break;             // abs
                    default: break;
                }
                setZn();
            }
            break;
        case 0x1B: case 0x4B: case 0x5B: {  // mpys, mpya, mld (rj), (ri): then X = (ri), Y = (rj)
            if ((op >> 9) == 0x5B) {
                a_ = 0;
                st_ = static_cast<uint16_t>((st_ & 0x0FFF) | kFlagZ);
            } else {
                a_ += (op >> 9) == 0x4B ? static_cast<uint32_t>(product()) : 0u - static_cast<uint32_t>(product());
                setZn();
            }
            x_ = pointed(op & 3, 0, op >> 2 & 3, true);
            y_ = pointed(op >> 4 & 3, 1, op >> 6 & 3, true);
            break;
        }
        default: {
            // Accumulator operations: the low 4 bits of the top 7 say on what, the top 3 which.
            const int kind = op >> 9 & 0xF, alu3 = op >> 13;
            if (alu3 == 0 && kind != 3) break;  // nothing else is known
            uint32_t v = 0;
            switch (kind) {
                case 0x0:  // OP A, s (P and A in full)
                    if (s == kP) {
                        alu(alu3, static_cast<uint32_t>(product()));
                        return cycles;
                    }
                    if (s == kA) {
                        alu(alu3, a_);
                        return cycles;
                    }
                    v = readReg(s);
                    break;
                case 0x1: v = readPointer(op); break;                       // OP A, (ri)
                case 0x3:                                                   // OP A, adr
                    v = ram_[op & 0x1FF];
                    if (alu3 == 0) {  // ld A, adr: the top half, no flags
                        a_ = (a_ & 0xFFFF) | v << 16;
                        return cycles;
                    }
                    break;
                case 0x4: v = fetch(); ++cycles; break;                     // OP A, imm
                case 0x5: v = readProgramPointer(op); cycles += 2; break;   // OP A, ((ri))
                case 0x9: v = r_[(op & 3) | (op >> 6 & 4)]; break;          // OP A, ri
                case 0xC: v = op & 0xFF; break;                             // OP A, simm
                default: return cycles;
            }
            alu(alu3, v << 16);
            break;
        }
    }
    return cycles;
}

}  // namespace plumsg
