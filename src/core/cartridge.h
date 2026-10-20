#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace plumsg {

enum class Region : uint8_t { Auto = 0, JapanNtsc = 1, UsaNtsc = 2, EuropePal = 3 };

// What the ROM header at 0x100 says about the game.
struct RomHeader {
    std::string system;      // "SEGA MEGA DRIVE", "SEGA GENESIS"...
    std::string title;       // overseas name, trimmed
    std::string serial;
    bool japan = false, usa = false, europe = false;  // supported regions
    bool hasSram = false;
    uint32_t sramStart = 0, sramEnd = 0;
};

// Mega Drive cartridge: plain (.bin/.md/.gen) or interleaved .smd ROMs, alone or in a
// .zip; battery SRAM (mapped by the header or at 0x200000, switched with 0xA130F1 on
// ROMs over 2 MB) and the Super Street Fighter II bank mapper (0xA130F3-0xA130FF).
class Cartridge {
public:
    bool loadFile(const std::string& path, std::string* error);
    void loadData(std::vector<uint8_t> rom);

    bool loaded() const { return !rom_.empty(); }
    size_t romSize() const { return rom_.size(); }
    const std::vector<uint8_t>& rom() const { return rom_; }
    // How much of the ROM, from its start, reads as itself whatever the game does: up to
    // where SRAM could be mapped, none at all with banks.
    uint32_t plainRomBytes() const {
        if (mapper_) return 0;
        const size_t end = sramPresent_ ? std::min<size_t>(rom_.size(), sramStart_) : rom_.size();
        return static_cast<uint32_t>(end & ~size_t{1});
    }
    const std::string& path() const { return path_; }
    const RomHeader& header() const { return header_; }
    uint32_t crc32() const { return crc_; }
    // The region the header asks for, preferring USA, then Japan, then Europe.
    Region preferredRegion() const;

    // The cartridge is a Master System (or Game Gear) one: its header is not where a Mega Drive's is,
    // so what was read there is forgotten (the title comes from the file name) and the
    // region is the one its own header asks for.
    void setMasterSystemHeader(Region region, bool gameGear);

    // Changes one byte of the ROM image (cheats); false if `offset` is past its end.
    bool patchRom(uint32_t offset, uint8_t value, uint8_t* previous);

    void reset();
    uint8_t read8(uint32_t addr) const;
    uint16_t read16(uint32_t addr) const;
    void write8(uint32_t addr, uint8_t value);
    void write16(uint32_t addr, uint16_t value);
    // 0xA130F1-0xA130FF: SRAM switch and bank registers.
    void writeTimeRegister(uint32_t addr, uint8_t value);

    bool hasSaveData() const { return sramUsed_; }
    // The battery RAM, indexed by address - sramStart (empty without one).
    const std::vector<uint8_t>& saveRam() const { return sram_; }
    // The battery RAM by offset, for cartridges that map it themselves (the Master
    // System's mapper). Past its end it reads 0xFF and ignores writes.
    uint8_t saveRamByte(uint32_t offset) const { return offset < sram_.size() ? sram_[offset] : 0xFF; }
    void writeSaveRamByte(uint32_t offset, uint8_t value) {
        if (offset >= sram_.size()) return;
        if (sram_[offset] != value) sramUsed_ = true;
        sram_[offset] = value;
    }
    bool loadSave(const std::string& path);
    bool writeSave(const std::string& path) const;

    template <class Ar>
    void state(Ar& ar) {
        ar(sram_, sramMapped_, sramWritable_, sramUsed_, banks_);
    }

private:
    bool sramAt(uint32_t addr) const;

    std::vector<uint8_t> rom_;
    std::vector<uint8_t> sram_;  // indexed by address - sramStart
    RomHeader header_;
    uint32_t sramStart_ = 0, sramEnd_ = 0;
    bool sramPresent_ = false;
    bool sramMapped_ = true;   // 0xA130F1 bit 0 (ROMs over 2 MB start with ROM mapped)
    bool sramWritable_ = true;
    bool sramUsed_ = false;
    bool mapper_ = false;
    std::array<uint8_t, 8> banks_{0, 1, 2, 3, 4, 5, 6, 7};  // 512 KB pages per slot
    uint32_t crc_ = 0;
    std::string path_;
};

}  // namespace plumsg
