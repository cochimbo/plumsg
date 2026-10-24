#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "cartridge.h"

namespace plumsg {

// The consoles plumsg knows: the Mega Drive alone, with the 32X on top or with the
// Mega CD underneath, the Master System, whose games it runs as the Mega Drive does,
// and the Game Gear, a Master System in a handheld.
enum class ConsoleSystem : uint8_t { MegaDrive = 0, Mega32X = 1, MegaCD = 2, MasterSystem = 3, GameGear = 4 };
constexpr int kSystemCount = 5;

// "Mega Drive", "32X", "Mega CD", "Master System", "Game Gear".
const char* systemName(ConsoleSystem s);

// True for the files plumsg can open: cartridges (.md .gen .smd .bin .32x .sms .gg .zip)
// and discs (.cue .chd .iso).
bool isGameFile(const std::string& path);

// Which console a game file is for. Mostly by extension (.32x is 32X; .sms is Master
// System; .gg is Game Gear; .cue, .chd and .iso are Mega CD); a .bin is also looked
// into, since 32X, Master System and Game Gear cartridges and single-track discs are
// sometimes dumped as .bin, and so is a .zip (a .32x, .sms or .gg inside it).
ConsoleSystem detectSystem(const std::string& path);
// The same from the first bytes of a .bin (at least 0x8000 if there are that many, for
// the Master System's header).
ConsoleSystem detectSystemFromData(const std::vector<uint8_t>& start);
// The region a Master System or Game Gear cartridge asks for: Europe for the games that
// only run at 50 Hz, else Japan or USA as its header says.
Region smsRegion(const std::vector<uint8_t>& rom, bool gameGear = false);

// The files a .cue sheet points to (FILE lines), as full paths next to the .cue.
std::vector<std::string> cueFiles(const std::string& cuePath);
// The same from the text of a .cue whose folder is `folder` (ending in a separator or empty).
std::vector<std::string> cueFilesFromText(const std::string& text, const std::string& folder);

// The CD of an MSU-MD cartridge (one patched to play its music from CD tracks): a .cue
// of the same name next to it, as Sonic.md and Sonic.cue. Empty if there is none.
std::string msuMdCue(const std::string& romPath);

// What a file chosen as a Mega CD BIOS looks like.
struct BiosInfo {
    bool valid = false;
    Region region = Region::Auto;  // from its header, Auto if it does not say
    std::string name;              // its header's name, e.g. "MEGA-CD BOOT ROM"
    std::string problem;           // why it is not valid, in Spanish
};
BiosInfo identifyCdBios(const std::vector<uint8_t>& data);
BiosInfo identifyCdBiosFile(const std::string& path);
// Looks for Mega CD BIOS files in `folders` and their subfolders: one path per region
// (Japan, USA, Europe), empty where none was found. The region comes from the header,
// or else from the file name ("bios_CD_U.bin", "[BIOS] Sega CD (USA)...").
std::array<std::string, 3> findCdBios(const std::vector<std::string>& folders);

}  // namespace plumsg
