#include "sms.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace plumsg {

bool isCodemasters(const std::vector<uint8_t>& rom) {
    if (rom.size() < 0x8000) return false;
    const unsigned sum = rom[0x7FE6] | rom[0x7FE7] << 8, inverse = rom[0x7FE8] | rom[0x7FE9] << 8;
    return sum != 0 && sum + inverse == 0x10000;
}

bool isMasterSystemBiosFile(const std::string& path) {
    std::string name = std::filesystem::u8path(path).filename().u8string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    return name.find("bios") != std::string::npos;
}

MasterSystem::MasterSystem(Cartridge& cart, std::array<uint8_t, 0x2000>& ram, VideoFrameMd& frame, bool gameGear)
    : cart_(cart), ram_(ram), vdp_(frame), gameGear_(gameGear) {
    vdp_.setGameGear(gameGear);
}

void MasterSystem::powerOn() {
    vdp_.reset();
    codemasters_ = isCodemasters(cart_.rom());
    mapper_ = codemasters_ ? std::array<uint8_t, 4>{0, 0, 1, 0} : std::array<uint8_t, 4>{0, 0, 1, 2};
    reset();
}

void MasterSystem::reset() {
    ioControl_ = 0xFF;
    memoryControl_ = 0;
    pause_ = false;
    fmControl_ = 0;
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
    } else if (addr >= 0x8000 && (mapper_[0] & 0x08)) {
        cart_.writeSaveRamByte(((mapper_[0] >> 2) & 1) * 0x4000u + (addr & 0x3FFF), value);
    }
}

uint8_t MasterSystem::in(uint8_t port, int cycle) {
    if (gameGear_ && port < 0x07) {
        // START (active low), export console, NTSC; then the link port, idle.
        static constexpr uint8_t kLink[6] = {0x7F, 0xFF, 0x00, 0xFF, 0x00, 0xFF};
        if (port == 0) return static_cast<uint8_t>((pads_[0].start ? 0x00 : 0x80) | (japanese_ ? 0x00 : 0x40));
        return kLink[port - 1];
    }
    // The ports are decoded by address lines 7, 6 and 0 only.
    switch (port & 0xC1) {
        case 0x40: return vdp_.vcounter();
        case 0x41: return vdp_.hcounter();
        case 0x80: return vdp_.readData();
        case 0x81: return vdp_.readStatus(cycle);
        case 0xC0: return portA();
        case 0xC1: return portB();
        default: return 0xFF;
    }
}

void MasterSystem::out(uint8_t port, uint8_t value, int cycle) {
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

// Both ports read active low.
uint8_t MasterSystem::portA() const {
    static const PadState kNone{};
    const PadState& a = pads_[0];
    const PadState& b = gameGear_ ? kNone : pads_[1];
    uint8_t v = 0xFF;
    if (a.up) v &= ~0x01;
    if (a.down) v &= ~0x02;
    if (a.left) v &= ~0x04;
    if (a.right) v &= ~0x08;
    if (a.a || a.b) v &= ~0x10;
    if (a.c) v &= ~0x20;
    if (b.up) v &= ~0x40;
    if (b.down) v &= ~0x80;
    return v;
}

uint8_t MasterSystem::portB() const {
    static const PadState kNone{};
    const PadState& b = gameGear_ ? kNone : pads_[1];
    uint8_t v = 0xFF;
    if (b.left) v &= ~0x01;
    if (b.right) v &= ~0x02;
    if (b.a || b.b) v &= ~0x04;
    if (b.c) v &= ~0x08;
    // TH of each port: an input reads high; an output reads back what was written on an
    // export console, and the opposite on a Japanese one.
    auto th = [&](int dirBit, int levelBit) {
        if (ioControl_ & dirBit) return true;
        const bool level = (ioControl_ & levelBit) != 0;
        return japanese_ ? !level : level;
    };
    if (!th(0x02, 0x20)) v &= ~0x40;
    if (!th(0x08, 0x80)) v &= ~0x80;
    return v;
}

}  // namespace plumsg
