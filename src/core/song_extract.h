#pragma once

#include <cstdint>
#include <functional>
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
    // `game` is what goes in the files' tags. False, and why in *error, when the ROM
    // cannot be opened or its sound driver does not take requests this way.
    bool run(const std::string& romPath, const std::string& game, const ProgressFn& progress, const SongFn& song,
             std::string* error);

private:
    LogFn log_;
};

}  // namespace plumsg
