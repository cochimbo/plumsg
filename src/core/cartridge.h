#pragma once

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
    const std::string& path() const { return path_; }
    const RomHeader& header() const { return header_; }
    uint32_t crc32() const { return crc_; }
    // The region the header asks for, preferring USA, then Japan, then Europe.
    Region preferredRegion() const;

    void reset();
    uint8_t read8(uint32_t addr) const;
    uint16_t read16(uint32_t addr) const;
    void write8(uint32_t addr, uint8_t value);
    void write16(uint32_t addr, uint16_t value);
    // 0xA130F1-0xA130FF: SRAM switch and bank registers.
    void writeTimeRegister(uint32_t addr, uint8_t value);

    bool hasSaveData() const { return sramUsed_; }
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
