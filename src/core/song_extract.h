#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace plumsg {

// Takes the music out of a game's ROM, without names: what the instrument editor's
// "Sacar canciones" does.
//
// Most Mega Drive sound drivers (SMPS, the one of Sonic and of many Sega games, and
// others like it) play what the game asks for by writing a number to a byte of RAM, the
// sound queue, which the driver reads and clears. The extractor boots the game and
// watches its memory until it sees that happen (the SEGA voice, the title music),
// checks that writing there really makes sound, and then, from just before the game's
// first request, asks for every number in turn while keeping the game's own requests
// out. Each one that plays music for a while is recorded to a VGM, until it loops back
// (one pass, with its loop point) or ends.
//
// Other drivers (Streets of Rage 2's) are asked by calling one of their routines with
// the number in a register. Those routines are found in the ROM, where the game calls
// them from many places with a different number each time; the extractor then calls
// them itself, from a moment the game was quiet.
//
// GEMS (the driver of many games of Sega of America: Sonic Spinball, Earthworm Jim) takes
// messages from a ring in the Z80's memory: the game writes a message after the last one
// (FF 10 n starts song n) and then where the next will go. The extractor finds that byte
// and the ring while the game boots, learns the message whose last byte changes most, and
// writes it itself with every number.
class SongExtractor {
public:
    struct Song {
        int number = 0;  // what the driver was asked for
        double seconds = 0;
        double loopSeconds = 0;  // 0 if it does not loop
        std::vector<uint8_t> vgz;
    };
    struct Progress {
        int tried = 0, total = 0;  // numbers asked for so far, of
        int found = 0;
        std::string doing;         // what it is doing now
    };
    // Called now and then; returning false stops.
    using ProgressFn = std::function<bool(const Progress&)>;
    // Called with each song found; its title says "Canción N".
    using SongFn = std::function<void(const Song&)>;
    // Messages about what it found, for the headless version's --verbose.
    using LogFn = std::function<void(const std::string&)>;

    void setLog(LogFn log) { log_ = std::move(log); }
    // After run(), even a stopped one: the FM instruments (RolandSound fingerprints)
    // heard in the short sounds it skipped and in none of the songs, which are the
    // game's sound effects.
    const std::set<uint32_t>& effectInstruments() const { return effects_; }
    // `game` is what goes in the files' tags. False, and why in *error, when the ROM
    // cannot be opened or its sound driver does not take requests this way.
    bool run(const std::string& romPath, const std::string& game, const ProgressFn& progress, const SongFn& song,
             std::string* error);

private:
    LogFn log_;
    std::set<uint32_t> effects_;
};

}  // namespace plumsg
