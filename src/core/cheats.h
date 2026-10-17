#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plumsg {

// One write a cheat code asks for. Addresses below 0x400000 patch the cartridge ROM
// (what the Game Genie does); addresses in work RAM (0xE00000-0xFFFFFF) are written
// again every frame (what the Action Replay does).
struct CheatPatch {
    uint32_t address = 0;
    uint16_t value = 0;
    bool byte = false;  // write only the low byte of value
};

// A code as the user typed it, with a name and whether it is on.
struct Cheat {
    std::string code;
    std::string name;
    bool enabled = true;
};

// Game Genie ("ABCD-EFGH", 8 letters of its 32-character alphabet) or Action Replay
// ("FF0A12:0009" word, "FF0A12:09" byte; the separator is optional). False, with a
// message in Spanish, if the code is neither or points outside the ROM and RAM.
bool decodeCheat(const std::string& code, CheatPatch* out, std::string* error);
// The Game Genie code for a ROM word patch (address must be even and below 0x400000).
std::string encodeGameGenie(uint32_t address, uint16_t value);
// The code in upper case without spaces, the way the cheat list shows it.
std::string normalizeCheat(const std::string& code);

// A game's cheat file: one line per code, "si" or "no", the code and an optional name.
std::string formatCheats(const std::vector<Cheat>& cheats);
std::vector<Cheat> parseCheats(const std::string& text);

}  // namespace plumsg
