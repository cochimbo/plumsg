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
    // Master System and Game Gear: the ROM byte must be this for the patch to apply
    // (-1: always). Their Game Genie checks it, so that a patch meant for one bank
    // leaves the others alone.
    int compare = -1;
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
//
// With `masterSystem`, the codes of the Master System and the Game Gear instead: Action
// Replay "00C0-1205" (00, a RAM address C000-DFFF, a byte; the dash is optional) and the
// Game Gear's Game Genie "3A8-6CF" or "3A8-6CF-E66" (with the byte it replaces). Their
// addresses are the Z80's: RAM from 0xC000, ROM below 0xC000.
bool decodeCheat(const std::string& code, CheatPatch* out, std::string* error, bool masterSystem = false);
// The same for a code made of several joined with '+' ("ABCD-EFGH+FF0A12:0009"), the
// way cheat lists write a cheat that needs more than one write: all its writes, or
// false with the message of the first part that is wrong.
bool decodeCheatCodes(const std::string& code, std::vector<CheatPatch>* out, std::string* error,
                      bool masterSystem = false);
// The Game Genie code for a ROM word patch (address must be even and below 0x400000).
std::string encodeGameGenie(uint32_t address, uint16_t value);
// The code in upper case without spaces, the way the cheat list shows it.
std::string normalizeCheat(const std::string& code);

// A game's cheat file: one line per code, "si" or "no", the code and an optional name.
std::string formatCheats(const std::vector<Cheat>& cheats);
std::vector<Cheat> parseCheats(const std::string& text);
// The cheats of a RetroArch .cht file (the libretro-database lists): each cheatN_code
// with cheatN_desc as its name, all off. Entries with no code are left out.
std::vector<Cheat> parseChtFile(const std::string& text);

}  // namespace plumsg
