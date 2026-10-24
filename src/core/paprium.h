#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "cartridge.h"
#include "music_track.h"

namespace plumsg {

// Paprium's cartridge (T-574120): besides 8 MB of ROM it has a microcontroller that
// does much of the game's work. The 68000 sees 64 KB of the chip's RAM where the
// first 64 KB of ROM would be. It leaves arguments at 0x1E10 on and a command in
// 0x1FEA, and the chip answers in that RAM: it unpacks graphics (read a block at a
// time from 0xC000), builds the sprite table and the DMA list that loads the sprites'
// tiles, scales the rotating picture in the menus and keeps the saves. It also plays
// the sound effects, samples in the ROM, at 48 kHz on 8 voices. The music is not in the
// ROM: the cartridge plays recordings of it, so it is read from files the player brings
// (see setMusicFolder).
class Paprium {
public:
    // The cartridge's own header names it.
    static bool detect(const Cartridge& cart);

    explicit Paprium(Cartridge& cart) : cart_(cart) {}
    void reset();

    // 0x000000-0x00FFFF: the chip's RAM.
    static bool inWindow(uint32_t addr) { return addr < 0x10000; }
    uint8_t read8(uint32_t addr);
    uint16_t read16(uint32_t addr);
    void write8(uint32_t addr, uint8_t value);
    void write16(uint32_t addr, uint16_t value);

    // Where the music's files are: a folder with the album's tracks, each file's name
    // starting with its number on the album ("01 Theme of Paprium.ogg"). How many it found.
    int setMusicFolder(const std::filesystem::path& folder);

    // Whether the console should be reset now (see command 0x84); asking clears it.
    bool takeRestart() {
        const bool r = restart_;
        restart_ = false;
        return r;
    }

    // Adds `count` stereo samples at `rate` Hz to samples().
    void runAudio(size_t count, double rate);
    std::vector<int16_t>& samples() { return samples_; }

    template <class Ar>
    void state(Ar& ar) {
        ar(ram_, unpacked_, scaler_, extraSprites_, streamMode_, streamPos_, streamLeft_, tileSrc_, tileDst_,
           frame_);
        for (Voice& v : voices_)
            ar(v.ptr, v.left, v.tick, v.phase, v.age, v.volume, v.pan, v.type, v.number, v.flags, v.loop, v.echoLeft);
        ar(echoL_, echoR_, echoPos_, echoSide_, sfxVolume_, audioFlags_, audioPhase_, sfxPtr_, spritePtr_, tilePtr_);
        ar(musicTrack_, musicPos_, musicVolume_, restart_);
        if constexpr (Ar::kLoading) {
            loadObjects();
            openMusic();
        }
    }

private:
    uint16_t word(uint32_t addr) const { return static_cast<uint16_t>(ram_[addr & 0xFFFF] << 8 | ram_[(addr + 1) & 0xFFFF]); }
    void setWord(uint32_t addr, uint16_t v) {
        ram_[addr & 0xFFFF] = static_cast<uint8_t>(v >> 8);
        ram_[(addr + 1) & 0xFFFF] = static_cast<uint8_t>(v);
    }
    uint32_t arg32(uint32_t addr) const { return static_cast<uint32_t>(word(addr)) << 16 | word(addr + 2); }
    uint8_t romByte(uint32_t addr) const { return addr < cart_.rom().size() ? cart_.rom()[addr] : 0; }
    uint32_t romLong(uint32_t addr) const {
        return static_cast<uint32_t>(romByte(addr)) << 24 | romByte(addr + 1) << 16 | romByte(addr + 2) << 8 | romByte(addr + 3);
    }

    void command(uint16_t value);
    // Unpacks the block at `src` (a type byte, then the data) into `out`; its size.
    size_t unpack(uint32_t src, std::vector<uint8_t>& out) const;
    void loadObjects();
    void drawObject(int index);
    void startSprites();
    void endSprites(int arg);
    void pauseSprites();
    void scalerLoad();
    void scale();
    void saveRead(int bank);
    void saveWrite(int bank);
    void playSound(int number);
    void loopSound(int mask);
    void stopSound(int mask);
    void soundSample(int& l, int& r);
    void playMusic(int track);
    void openMusic();
    void musicSample(int& l, int& r);

    Cartridge& cart_;
    std::array<uint8_t, 0x10000> ram_{};
    std::array<uint8_t, 0x10000> unpacked_{};  // the unpacker's output, read out at 0xC000
    std::array<uint8_t, 0x1000> scaler_{};     // the scaler's picture, one pixel per byte
    std::array<uint8_t, 14 * 8> extraSprites_{};  // sprites past the 80th, shown in another pass
    uint8_t streamMode_ = 0;
    uint32_t streamPos_ = 0, streamLeft_ = 0;
    uint32_t tileSrc_ = 0, tileDst_ = 0;  // where the next sprite's tiles go in RAM and VRAM
    std::array<uint32_t, 0x31> frame_{};  // each object's place in its animation

    struct Voice {
        uint32_t ptr = 0, left = 0;  // the next sample, and how many there are to go
        int32_t tick = 0;            // 16.16: 48 kHz ticks towards the next sample
        uint8_t phase = 0;           // 4-bit samples: which half of the byte
        uint32_t age = 0;            // samples played: the oldest voice is taken first
        int32_t volume = 0, pan = 0x80;
        uint8_t type = 0;            // rate (high nibble) and bits per sample (8 or 4)
        uint16_t number = 0, flags = 0;
        bool loop = false, echoLeft = false;
    };
    std::array<Voice, 8> voices_{};
    static constexpr int kEcho = 8000;  // 1/6 s at 48 kHz
    std::array<int32_t, kEcho> echoL_{}, echoR_{};
    int32_t echoPos_ = 0;
    uint8_t echoSide_ = 0;
    int32_t sfxVolume_ = 0;
    uint8_t audioFlags_ = 0;
    double audioPhase_ = 0;  // 48 kHz samples owed to the output
    uint32_t sfxPtr_ = 0;
    std::vector<int16_t> samples_;
    uint32_t spritePtr_ = 0, tilePtr_ = 0;
    std::vector<uint8_t> objects_;  // the object and animation tables, unpacked from spritePtr_

    std::array<std::filesystem::path, 53> musicFiles_;  // by number on the album
    uint8_t musicTrack_ = 0;  // the chip's number for what plays, 0 for nothing
    uint64_t musicPos_ = 0;   // 32.32: where it is in the file
    int32_t musicVolume_ = 0;
    std::unique_ptr<MusicTrack> music_;
    uint8_t musicOpen_ = 0;  // which track music_ is
    bool restart_ = false;
};

}  // namespace plumsg
