#include "sms.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace plumsg {

bool isCodemasters(const std::vector<uint8_t>& rom) {
    if (rom.size() < 0x8000) return false;
    const unsigned sum = rom[0x7FE6] | rom[0x7FE7] << 8, inverse = rom[0x7FE8] | rom[0x7FE9] << 8;
    return sum != 0 && sum + inverse == 0x10000;
}

bool hasGameGearEeprom(uint32_t romCrc) {
    static constexpr uint32_t kCrcs[] = {
        0x36EBCD6D,  // Majors Pro Baseball (USA)
        0x3D8D0DD6,  // World Series Baseball (USA)
        0xBB38CFD7,  // World Series Baseball (USA) (Rev 1)
        0x578A8A38,  // World Series Baseball '95 (USA)
    };
    return std::find(std::begin(kCrcs), std::end(kCrcs), romCrc) != std::end(kCrcs);
}

bool isMasterSystemBiosFile(const std::string& path) {
    std::string name = std::filesystem::u8path(path).filename().u8string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    return name.find("bios") != std::string::npos;
}

PadType masterSystemControllerFor(uint32_t romCrc) {
    static constexpr uint32_t kPhaser[] = {
        0xFBE5CFBB,  // Missile Defense 3-D
        0xE79BB689,  // Missile Defense 3-D (BIOS)
        0x43DEF05D,  // Missile Defense 3-D (prototype)
        0x56DCB2D4,  // 3-D Gunner (prototype)
        0x861B6E79,  // Assault City (Light Phaser)
        0x5FC74D2A,  // Gangster Town
        0xE167A561,  // Hang-On / Safari Hunt
        0x91E93385,  // Hang-On / Safari Hunt (BIOS)
        0xE8EA842C,  // Marksman Shooting / Trap Shooting
        0xE8215C2E,  // Marksman Shooting / Trap Shooting / Safari Hunt
        0x205CAAE8,  // Operation Wolf
        0x23283F37,  // Operation Wolf (alt.)
        0xDA5A7013,  // Rambo III
        0x79AC8E7F,  // Rescue Mission
        0x4B051022,  // Shooting Gallery
        0xA908CFF5,  // Space Gun
        0x5359762D,  // Wanted
        0x0CA95637,  // Laser Ghost
    };
    static constexpr uint32_t kPaddle[] = {
        0xF9DBB533,  // Alex Kidd BMX Trial
        0xA6FA42D0,  // Galactic Protector
        0x29BC7FAD,  // Megumi Rescue
        0x315917D4,  // Woody Pop
    };
    if (std::find(std::begin(kPhaser), std::end(kPhaser), romCrc) != std::end(kPhaser)) return PadType::LightPhaser;
    if (std::find(std::begin(kPaddle), std::end(kPaddle), romCrc) != std::end(kPaddle)) return PadType::Paddle;
    return PadType::SixButton;
}

MasterSystem::MasterSystem(Cartridge& cart, std::array<uint8_t, 0x2000>& ram, VideoFrameMd& frame, bool gameGear)
    : cart_(cart), ram_(ram), vdp_(frame), gameGear_(gameGear) {
    vdp_.setGameGear(gameGear);
}

void MasterSystem::powerOn() {
    vdp_.reset();
    codemasters_ = isCodemasters(cart_.rom());
    eepromPresent_ = gameGear_ && hasGameGearEeprom(cart_.crc32());
    autoController_ = gameGear_ ? PadType::SixButton : masterSystemControllerFor(cart_.crc32());
    // Wanted and Gangster Town see the light a little further to the left.
    phaserOffset_ = cart_.crc32() == 0x5359762D || cart_.crc32() == 0x5FC74D2A ? 16 : 20;
    mapper_ = codemasters_ ? std::array<uint8_t, 4>{0, 0, 1, 0} : std::array<uint8_t, 4>{0, 0, 1, 2};
    reset();
}

void MasterSystem::reset() {
    ioControl_ = 0xFF;
    memoryControl_ = 0;
    pause_ = false;
    fmControl_ = 0;
    link_ = kLinkReset;
    eeprom_ = Eeprom{};
    gunLit_ = {};
    paddleHigh_ = {};
    paddleReads_ = {};
}

PadType MasterSystem::controller(int port) const {
    if (gameGear_) return PadType::SixButton;
    const PadType t = types_[static_cast<size_t>(port & 1)];
    if (isMasterSystemController(t)) return t;
    if (port == 0 && autoControllers_ && (t == PadType::ThreeButton || t == PadType::SixButton)) return autoController_;
    return PadType::SixButton;  // a pad, also for nothing plugged in or a Mega Drive gun
}

uint8_t MasterSystem::romByte(int bank, uint16_t offset) const {
    const std::vector<uint8_t>& rom = cart_.rom();
    const size_t banks = (rom.size() + 0x3FFF) / 0x4000;
    const size_t at = static_cast<size_t>(bank) % banks * 0x4000 + offset;
    return at < rom.size() ? rom[at] : 0xFF;
}

uint8_t MasterSystem::read(uint16_t addr) const {
    if (codemasters_) {
        if (addr < 0x4000) return romByte(mapper_[1], addr);
        if (addr < 0x8000) return romByte(mapper_[2], addr & 0x3FFF);
        if (addr < 0xC000) {
            if ((mapper_[0] & 0x80) && addr >= 0xA000) return cart_.saveRamByte(addr & 0x1FFF);
            return romByte(mapper_[3], addr & 0x3FFF);
        }
        return ram_[addr & 0x1FFF];
    }
    if (bios_ && (memoryControl_ & 0x08) && addr < 0xC000) return 0xFF;
    if (addr < 0x0400) return cart_.rom()[addr];
    if (addr < 0x4000) return romByte(mapper_[1], addr);
    if (addr < 0x8000) return romByte(mapper_[2], addr & 0x3FFF);
    if (addr < 0xC000) {
        if (eepromPresent_) return addr == 0x8000 && eeprom_.enabled ? eepromRead() : romByte(mapper_[3], addr & 0x3FFF);
        if (mapper_[0] & 0x08) return cart_.saveRamByte(((mapper_[0] >> 2) & 1) * 0x4000u + (addr & 0x3FFF));
        return romByte(mapper_[3], addr & 0x3FFF);
    }
    return ram_[addr & 0x1FFF];
}

void MasterSystem::write(uint16_t addr, uint8_t value) {
    if (codemasters_) {
        if (addr == 0x0000) mapper_[1] = value;
        else if (addr == 0x4000) {
            mapper_[2] = value & 0x7F;
            mapper_[0] = value & 0x80;
        } else if (addr == 0x8000) mapper_[3] = value;
        else if (addr >= 0xA000 && addr < 0xC000 && (mapper_[0] & 0x80)) cart_.writeSaveRamByte(addr & 0x1FFF, value);
        else if (addr >= 0xC000) ram_[addr & 0x1FFF] = value;
        return;
    }
    if (addr >= 0xC000) {
        ram_[addr & 0x1FFF] = value;
        if (addr >= 0xFFFC) mapper_[addr - 0xFFFC] = value;
        if (addr == 0xFFFC && eepromPresent_) {
            eeprom_.enabled = (value & 0x08) != 0;
            if (value & 0x80) eeprom_ = Eeprom{eeprom_.enabled};
        }
    } else if (eepromPresent_) {
        if (addr == 0x8000 && eeprom_.enabled) eepromWrite(value);
    } else if (addr >= 0x8000 && (mapper_[0] & 0x08)) {
        cart_.writeSaveRamByte(((mapper_[0] >> 2) & 1) * 0x4000u + (addr & 0x3FFF), value);
    }
}

// The 93C46 takes a bit on each rise of the clock while selected: a start bit, then 8
// bits of opcode and address, then for a write 16 bits of data; a read shifts its word
// out a bit a clock, and goes on to the next word. Dropping the chip select ends a
// command. The words are kept little-endian in the save RAM.
void MasterSystem::eepromWrite(uint8_t value) {
    Eeprom& e = eeprom_;
    const bool cs = value & 0x04, clk = value & 0x02, in = value & 0x01;
    if (!cs) {
        if (e.cs) {
            e.data = true;
            e.state = Eeprom::WaitStart;
        }
    } else if (clk && !e.clk) {
        switch (e.state) {
            case Eeprom::WaitStart:
                if (in) {
                    e.opcode = 0;
                    e.cycles = 0;
                    e.state = Eeprom::Opcode;
                }
                break;
            case Eeprom::Opcode:
                e.opcode = static_cast<uint8_t>(e.opcode | in << (7 - e.cycles));
                if (++e.cycles < 8) break;
                e.cycles = 0;
                e.buffer = 0;
                e.state = Eeprom::Standby;
                switch (e.opcode >> 6) {
                    case 1: e.state = Eeprom::Write; break;  // WRITE
                    case 2:                                   // READ
                        e.buffer = eepromWord(e.opcode & 0x3F);
                        e.data = false;  // a dummy 0 bit comes first
                        e.state = Eeprom::Read;
                        break;
                    case 3:  // ERASE
                        if (e.writeEnable) setEepromWord(e.opcode & 0x3F, 0xFFFF);
                        break;
                    default:
                        switch ((e.opcode >> 4) & 3) {
                            case 1: e.state = Eeprom::Write; break;  // WRITE ALL
                            case 2:                                   // ERASE ALL
                                if (e.writeEnable)
                                    for (int i = 0; i < 64; ++i) setEepromWord(i, 0xFFFF);
                                break;
                            default: e.writeEnable = (e.opcode >> 4) & 1; break;  // EWEN / EWDS
                        }
                        break;
                }
                break;
            case Eeprom::Write:
                e.buffer = static_cast<uint16_t>(e.buffer | in << (15 - e.cycles));
                if (++e.cycles < 16) break;
                if (e.writeEnable) {
                    if (e.opcode & 0x40) setEepromWord(e.opcode & 0x3F, e.buffer);
                    else
                        for (int i = 0; i < 64; ++i) setEepromWord(i, e.buffer);
                }
                e.state = Eeprom::Standby;
                break;
            case Eeprom::Read:
                e.data = (e.buffer >> (15 - e.cycles)) & 1;
                if (++e.cycles < 16) break;
                e.cycles = 0;
                e.opcode = static_cast<uint8_t>((e.opcode & 0xC0) | ((e.opcode + 1) & 0x3F));
                e.buffer = eepromWord(e.opcode & 0x3F);
                break;
            default: break;
        }
    }
    e.cs = cs;
    e.clk = clk;
}

uint8_t MasterSystem::eepromRead() const {
    return static_cast<uint8_t>((eeprom_.cs ? 0x04 : 0) | 0x02 | (eeprom_.data ? 0x01 : 0));
}

uint16_t MasterSystem::eepromWord(int index) const {
    return static_cast<uint16_t>(cart_.saveRamByte(index * 2u) | cart_.saveRamByte(index * 2u + 1) << 8);
}

void MasterSystem::setEepromWord(int index, uint16_t value) {
    cart_.writeSaveRamByte(index * 2u, static_cast<uint8_t>(value));
    cart_.writeSaveRamByte(index * 2u + 1, static_cast<uint8_t>(value >> 8));
}

uint8_t MasterSystem::in(uint8_t port, int cycle) {
    if (gameGear_ && port < 0x06) {
        // START (active low), export console, NTSC; then the link port.
        if (port == 0) return static_cast<uint8_t>((pads_[0].start ? 0x00 : 0x80) | (japanese_ ? 0x00 : 0x40));
        // Its outputs read back what was written, its inputs high (issue #78: Primal Rage
        // writes and reads it back to look for the cable, and waits till it works).
        if (port == 1) return static_cast<uint8_t>((link_[1] & ~(link_[2] & 0x7F)) | (link_[2] & 0x7F));
        return link_[port];
    }
    // The ports are decoded by address lines 7, 6 and 0 only.
    switch (port & 0xC1) {
        case 0x40: return vdp_.readVcounter(cycle);
        case 0x41: return vdp_.hcounter();
        case 0x80: return vdp_.readData();
        case 0x81: return vdp_.readStatus(cycle);
        case 0xC0: return portA(cycle);
        case 0xC1: return portB(cycle);
        default: return 0xFF;
    }
}

void MasterSystem::out(uint8_t port, uint8_t value, int cycle) {
    if (gameGear_ && port < 0x06) {
        // The link port's own registers, not the I/O and memory control of 0x3E/0x3F.
        if (port == 1 || port == 2 || port == 3) link_[port] = value;
        else if (port == 5) link_[5] = value & 0xF8;  // the low bits are its status
        return;
    }
    switch (port & 0xC1) {
        case 0x00: memoryControl_ = value; break;
        case 0x01: {
            // A TH line going from low to high latches the H counter.
            const uint8_t before = thLevels();
            ioControl_ = value;
            if (~before & thLevels()) vdp_.latchHcounter(cycle);
            break;
        }
        case 0x80: vdp_.writeData(value); break;
        case 0x81: vdp_.writeControl(value, cycle); break;
        default: break;
    }
}

// The TH lines of both ports as they are driven (bit 0 port A, bit 1 port B): an input
// is pulled high.
uint8_t MasterSystem::thLevels() const {
    uint8_t v = 0;
    if ((ioControl_ & 0x02) || (ioControl_ & 0x20)) v |= 1;
    if ((ioControl_ & 0x08) || (ioControl_ & 0x80)) v |= 2;
    return v;
}

// TH of a port as the console reads it: an input is pulled high (but for what drives
// it); an output reads back what was written on an export console, and the opposite on
// a Japanese one.
bool MasterSystem::thPin(int port) const {
    const int dirBit = port ? 0x08 : 0x02, levelBit = port ? 0x80 : 0x20;
    if (ioControl_ & dirBit) return true;
    const bool level = (ioControl_ & levelBit) != 0;
    return japanese_ ? !level : level;
}

// The Light Phaser sees the light while the beam goes by near where it points: a few
// lines up or down, and a wide stretch of the line around it, as its lens is not sharp.
// The H counter runs in pairs of pixels from 0 at the picture's left edge, and skips
// from 0x93 to 0xE9 in the blanking, so past that it is left of the picture.
bool MasterSystem::phaserSeesLight(int port, int cycle) const {
    const GunState& g = guns_[static_cast<size_t>(port)];
    if (!g.onScreen(256, vdp_.activeLines())) return false;
    int h = VdpSms::hcounterAt(cycle);
    if (h > 0x93) h -= 0x100;
    return std::abs(g.y - vdp_.line()) <= 5 && std::abs(g.x - h * 2) <= 60;
}

uint8_t MasterSystem::portPins(int port, int cycle, bool thRead) {
    static const PadState kNone{};
    const size_t p = static_cast<size_t>(port);
    const PadState& pad = gameGear_ && port == 1 ? kNone : pads_[p];
    const int dirBit = port ? 0x08 : 0x02, levelBit = port ? 0x80 : 0x20;
    switch (controller(port)) {
        case PadType::Paddle: {
            // Bits 0-3: four bits of the knob, the high ones with TR high; TL the button.
            // With TH an output (export consoles' way) the game picks them with TH; the
            // Japanese paddle switches on its own, here after every few reads of the
            // port that has them, as both ports take two reads.
            if (thRead == (port == 1)) {
                if (!(ioControl_ & dirBit)) paddleHigh_[p] = (ioControl_ & levelBit) != 0;
                else if (++paddleReads_[p] > 2) {
                    paddleReads_[p] = 0;
                    paddleHigh_[p] = !paddleHigh_[p];
                }
            }
            const PaddleState& paddle = paddles_[p];
            uint8_t v = paddleHigh_[p] ? static_cast<uint8_t>(0x20 | (paddle.position >> 4)) : (paddle.position & 0x0F);
            if (!paddle.button) v |= 0x10;
            if (thPin(port)) v |= 0x40;
            return v;
        }
        case PadType::LightPhaser: {
            // The gun drives TL (its trigger) and TH (low while it sees the light); the
            // pad's other buttons go on showing, for the games that also take a pad there
            // (Hang-On, on the same cartridge as Safari Hunt).
            uint8_t v = 0x7F;
            if (pad.up) v &= ~0x01;
            if (pad.down) v &= ~0x02;
            if (pad.left) v &= ~0x04;
            if (pad.right) v &= ~0x08;
            if (guns_[p].trigger || pad.a || pad.b) v &= ~0x10;
            if (pad.c) v &= ~0x20;
            if (!thPin(port)) v &= ~0x40;
            if (thRead && (ioControl_ & dirBit)) {
                const bool lit = phaserSeesLight(port, cycle);
                if (lit) {
                    v &= ~0x40;
                    // Seeing it latches the H counter, where the games expect the gun's x.
                    if (!gunLit_[p]) vdp_.setHcounterLatch(static_cast<uint8_t>(phaserOffset_ + guns_[p].x / 2));
                }
                gunLit_[p] = lit;
            }
            return v;
        }
        default: {
            uint8_t v = 0x7F;
            if (pad.up) v &= ~0x01;
            if (pad.down) v &= ~0x02;
            if (pad.left) v &= ~0x04;
            if (pad.right) v &= ~0x08;
            if (pad.a || pad.b) v &= ~0x10;
            if (pad.c) v &= ~0x20;
            if (!thPin(port)) v &= ~0x40;
            return v;
        }
    }
}

// Port A: the first port's pins but TH, and the second one's up and down.
uint8_t MasterSystem::portA(int cycle) {
    const uint8_t a = portPins(0, cycle, false);
    const uint8_t b = gameGear_ ? 0x7F : portPins(1, cycle, false);
    return static_cast<uint8_t>((a & 0x3F) | (b & 0x03) << 6);
}

// Port B: the second port's other pins, the reset button (not pressed) and both THs.
uint8_t MasterSystem::portB(int cycle) {
    const uint8_t a = gameGear_ ? 0x7F : portPins(0, cycle, true);
    const uint8_t b = gameGear_ ? 0x7F : portPins(1, cycle, true);
    uint8_t v = static_cast<uint8_t>(0x30 | ((b >> 2) & 0x0F));
    if (gameGear_) {
        // TH of each port as the console drives it; nothing is plugged in.
        if (thPin(0)) v |= 0x40;
        if (thPin(1)) v |= 0x80;
        return v;
    }
    return static_cast<uint8_t>(v | (a & 0x40) | (b & 0x40) << 1);
}

}  // namespace plumsg
