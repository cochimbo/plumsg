#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "audio_mixer.h"
#include "cartridge.h"
#include "cheats.h"
#include "io.h"
#include "m68k.h"
#include "mars.h"
#include "megacd.h"
#include "paprium.h"
#include "psg.h"
#include "roland.h"
#include "sms.h"
#include "ym2413.h"
#include "vgm.h"
#include "vdp_md.h"
#include "ym2612.h"
#include "z80.h"

namespace plumsg {

// The whole console: 68000 and Z80 with their buses and memory maps, VDP, YM2612, PSG,
// I/O and cartridge, plus the scheduler that interleaves them line by line. With a 32X
// game it also has the 32X, whose two SH-2s run alongside the 68000, and with a disc
// the Mega CD, whose BIOS and memory take the cartridge's place.
// With a Master System cartridge it runs in the console's Master System mode instead:
// only the Z80, the PSG and the Master System's own VDP and memory map. It knows
// nothing about windows, rendering or audio devices: frontends call runFrame(), then
// read frame() and audio().samples(), and feed the pads in.
class MegaDrive : private M68kBus, private Z80Bus {
public:
    static constexpr double kMasterNtsc = 53693175.0;
    static constexpr double kMasterPal = 53203424.0;
    static constexpr int kMasterPerLine = VdpMd::kMasterPerLine;

    MegaDrive();
    MegaDrive(const MegaDrive&) = delete;
    MegaDrive& operator=(const MegaDrive&) = delete;

    bool loadRom(const std::string& path, std::string* error);  // .bin/.md/.gen/.smd/.32x/.zip, or a disc
    void loadRomData(std::vector<uint8_t> rom);                 // for tests and tools
    // Mega CD discs (.cue, .iso, .chd, or a .bin with one data track) need the BIOS of
    // their region, handed over first (128 KB each; empty for none).
    void setCdBios(Region region, std::vector<uint8_t> bios);
    bool loadDisc(const std::string& path, std::string* error);
    // A disc already open (tests and tools); false without a BIOS for its region.
    bool loadDiscImage(std::unique_ptr<CdImage> disc, std::string* error);
    // MSU-MD: a cartridge patched to play its music from a CD. loadRom() does it by
    // itself when a .cue of the same name sits next to the cartridge (Sonic.md and
    // Sonic.cue): the cartridge boots with the Mega CD attached, which needs a BIOS of
    // any region. Without one the cartridge still loads, on its own, and loadNotice()
    // says why there is no CD music.
    void loadRomDataWithDisc(std::vector<uint8_t> rom, std::unique_ptr<CdImage> disc);  // tests and tools
    bool hasMsuMd() const { return mcd_ && mcd_->cartridgeMode(); }
    const std::string& loadNotice() const { return loadNotice_; }  // empty when all went well
    void reset();       // the console's reset button: CPUs, VDP and sound; RAM is kept
    void powerCycle();  // also clears memory

    // Auto follows the ROM header. Takes effect on the next reset or power cycle.
    // Mega CD discs always run in their own region, whatever this says.
    void setRegion(Region r);
    Region region() const { return effectiveRegion_; }
    bool pal() const { return effectiveRegion_ == Region::EuropePal; }
    void setPads(const PadState& p1, const PadState& p2) {
        io_.setPad(0, p1);
        io_.setPad(1, p2);
        if (sms_) sms_->setPads(p1, p2);
    }
    void setPadType(int port, PadType t) { io_.setPadType(port, t); }

    // Replaces the active cheats. ROM patches are applied now and undone by the next
    // call (or by loading another ROM); RAM writes are made at the start of every frame.
    void setCheats(const std::vector<CheatPatch>& cheats);

    // Overclock: the 68000 runs at `percent` of its speed (kMinOverclock to kMaxOverclock),
    // so games that slow down when there is much on screen get each frame done in time.
    // The rest of the console (Z80, VDP, sound, the Mega CD and 32X CPUs) keeps its speed.
    // Unlike the sprite limit the game does run differently, so it is part of the state:
    // a netplay guest runs at the host's speed, and states without it load at 100.
    static constexpr int kMinOverclock = 100, kMaxOverclock = 400;
    void setOverclock(int percent) { overclock_ = std::clamp(percent, kMinOverclock, kMaxOverclock); }
    int overclock() const { return overclock_; }

    void runFrame();  // up to the start of the next frame (just after VBlank ends)
    // Run-ahead, to take frames of input lag away: runs `frames` more frames with the
    // pads as they are, without sound, the Roland sound or the VGM recording, so that
    // frame() shows the last of them (drawn even under setDrawing(false)), and then
    // puts the console back as it was. Only the picture is left from the future;
    // false if the state could not be put back.
    bool runAhead(int frames);

    // The Roland sound (not owned; null for the console's own). It plays the FM and PSG
    // music instead, which stays at `chipPercent` of its volume; the DAC is untouched.
    // It only changes the sound that comes out, never what the console does. After
    // loading a savestate the caller decides whether what was playing goes on (a
    // netplay rollback, which replays the same music) or stops (RolandSound::allNotesOff).
    void setRoland(RolandSound* roland, int chipPercent);
    const RolandSound* rolandSound() const { return roland_; }
    // Records the YM2612 and PSG music to a VGM from now on (not owned; null stops).
    void setVgmWriter(VgmWriter* writer);
    double masterClock() const { return pal() ? kMasterPal : kMasterNtsc; }
    uint64_t masterClocks() const { return now_; }

    const VideoFrameMd& frame() const { return vdp_.frame(); }
    AudioMixer& audio() { return mixer_; }
    // Sample rate for audio().samples(); the frontend nudges it to keep its queue level.
    void setSampleRate(double hz);
    double refreshRate() const { return pal() ? 49.701459 : 59.922743; }
    uint64_t frameCount() const { return frames_; }

    // Savestates: only between frames. The format is versioned and made of tagged
    // blocks, so newer readers can skip unknown blocks; it is deflate-compressed and
    // tied to the ROM's CRC32. loadState() leaves the console untouched on failure.
    // `quick` skips the thumbnail and compresses less, for rewind snapshots taken
    // every frame.
    bool saveState(std::vector<uint8_t>& out, bool quick = false);
    bool loadState(const std::vector<uint8_t>& in, std::string* error);
    // The half-size picture stored in a savestate (RGBA, `width` x `height`).
    static bool stateThumbnail(const std::vector<uint8_t>& state, std::vector<uint32_t>& pixels, int* width,
                               int* height);

    Cartridge& cartridge() { return cart_; }
    const Cartridge& cartridge() const { return cart_; }
    M68000& cpu() { return cpu_; }
    Z80& z80() { return z80_; }
    VdpMd& vdp() { return vdp_; }
    Ym2612& ym() { return ym_; }
    Psg& psg() { return psg_; }
    Mars* mars() { return mars_.get(); }  // null unless the game is a 32X one
    bool is32x() const { return mars_ != nullptr; }
    MasterSystem* masterSystem() { return sms_.get(); }  // null unless the game is a Master System one
    bool isMasterSystem() const { return sms_ != nullptr; }  // the Game Gear too
    bool isGameGear() const { return sms_ && sms_->gameGear(); }
    // The Master System's FM sound unit is built into Japanese consoles; this puts it in
    // the others too, for games that use it wherever they find it. Takes effect on the
    // next reset or power cycle.
    void setMasterSystemFm(bool always) {
        smsFmAlways_ = always;
        applyRegion();
    }
    Ym2413* ym2413() { return opll_.get(); }  // null unless the game is a Master System one
    MegaCd* megaCd() { return mcd_.get(); }  // null unless a disc is loaded
    bool isMegaCd() const { return mcd_ != nullptr; }
    CdImage* disc() { return disc_.get(); }  // the disc in the Mega CD, null if none
    // CPU view of memory, for tools and tests.
    uint8_t peek(uint32_t addr) { return read8(addr); }
    uint16_t peek16(uint32_t addr) { return read16(addr & ~1u); }
    const std::array<uint8_t, 0x10000>& workRam() const { return ram_; }
    const std::array<uint8_t, 0x2000>& z80Ram() const { return zram_; }

    // Song extraction (SongExtractor). The watch hears every byte written to work RAM
    // (0xFF0000 up) and to the Z80's RAM (0xA00000 up), and whether the 68000 wrote it;
    // also the 68000's writes to the YM2612, as 0xA04000-0xA04003. On a Master System:
    // the Z80's writes to its RAM, by their own address (0xC000 up), and to the sound
    // chips, as 0xA04000, all as the program's (m68k true). Empty for none.
    using MemoryWatch = std::function<void(uint32_t addr, uint8_t value, bool m68k)>;
    void setMemoryWatch(MemoryWatch watch);
    // Keeps the game from asking for music and sounds: the 68000's writes to
    // [addr, addr + count) of anything but 0 or 0x80 (of anything, if all) are dropped.
    // count 0: none.
    void lockSoundRequests(uint32_t addr, int count, bool all = false);
    // Writes a byte of work RAM or Z80 RAM (a Master System's at 0xC000) directly, past
    // the lock.
    void poke(uint32_t addr, uint8_t value);
    // Runs without drawing the picture (faster, for when only the sound matters).
    void setDrawing(bool on) { drawing_ = on; }

private:
    // 68000 bus.
    uint8_t read8(uint32_t addr) override;
    uint16_t read16(uint32_t addr) override;
    uint16_t openBus();
    void write8(uint32_t addr, uint8_t value) override;
    void write16(uint32_t addr, uint16_t value) override;
    void irqAcknowledge(int level) override;
    bool tasWrites() const override { return false; }
    // Z80 bus.
    uint8_t read(uint16_t addr) override;
    void write(uint16_t addr, uint8_t value) override;
    uint8_t in(uint16_t port) override;
    void out(uint16_t port, uint8_t value) override;
    void psgWrite(uint8_t value);

    uint16_t readVdp(uint32_t addr);
    void writeVdp(uint32_t addr, uint16_t value, bool byte);
    uint8_t readZ80Area(uint32_t addr);
    void writeZ80Area(uint32_t addr, uint8_t value);
    void writeBusControl(uint32_t addr, bool set);
    bool z80Running() const { return !z80BusReq_ && !z80Reset_; }
    bool z80BusFree() const { return z80BusReq_ || z80Reset_; }
    void runUntil(uint64_t clock);
    void syncSound();  // brings PSG and YM2612 up to now_
    void updateIrq() { cpu_.setIrqLevel(vdp_.irqLevel()); }
    void applyRegion();
    void configureFor(Region r);  // clocks, PAL/NTSC and version register
    int cdBiosSlot(Region r) const;  // the BIOS for `r`, or with a cartridge any there is; -1 for none
    void attachCd(std::unique_ptr<CdImage> disc, bool cartridge);
    void setUpMars();             // adds or removes the 32X to suit the cartridge
    void setUpSms();              // switches to Master System mode for its cartridges, or back
    void setUpPaprium();          // adds Paprium's chip for its cartridge
    void runFrameSms();
    void runFrameMd();
    std::vector<int16_t>* addOnSamples();  // the 32X's or the Mega CD's sound
    int lineClock() const { return static_cast<int>(now_ - lineStart_); }
    int smsPortCycle();
    // When a VDP port access happens: a second access in the same instruction comes after
    // the wait the first one caused.
    uint64_t vdpClock() const { return std::max(now_, vdpFreeAt_); }

    template <class Ar>
    void busState(Ar& ar);

    M68000 cpu_{*this};
    Z80 z80_{*this};
    VdpMd vdp_;
    Ym2612 ym_;
    Psg psg_;
    RolandSound* roland_ = nullptr;
    int rolandPercent_ = 100;
    uint32_t rolandChip_ = 0;  // the channels the Roland sound leaves to the chips
    void rolandChipChannels();
    VgmWriter* vgm_ = nullptr;
    Io io_;
    Cartridge cart_;
    AudioMixer mixer_;
    std::unique_ptr<Mars> mars_;
    std::unique_ptr<CdImage> disc_;
    std::unique_ptr<MegaCd> mcd_;
    std::unique_ptr<MasterSystem> sms_;
    std::unique_ptr<Paprium> paprium_;
    uint8_t tmss_ = 1;  // what Paprium reads back from 0xA14101
    std::unique_ptr<Ym2413> opll_;  // the Master System's FM unit, with sms_
    bool smsFmAlways_ = false;
    bool smsFile_ = false;  // the file loaded is a Master System one by its name
    bool ggFile_ = false;   // ...or a Game Gear one
    std::array<std::vector<uint8_t>, 3> cdBios_;  // Japan, USA, Europe
    std::string loadNotice_;

    std::array<uint8_t, 0x10000> ram_{};
    std::array<uint8_t, 0x2000> zram_{};
    uint16_t z80Bank_ = 0;
    bool z80BusReq_ = false;
    bool z80Reset_ = true;

    Region region_ = Region::Auto;
    Region effectiveRegion_ = Region::UsaNtsc;
    Region discRegion_ = Region::UsaNtsc;  // what the disc's security code is for
    uint64_t now_ = 0;         // master clock of whoever is running
    uint64_t lineStart_ = 0;
    int smsLine_ = 0;            // the Master System line being run
    bool smsLineEarly_ = false;  // a port access has already started the next one
    uint64_t vdpFreeAt_ = 0;   // until when a VDP access made the CPU wait
    uint64_t m68kClock_ = 0;   // master clocks the 68000 has reached
    uint64_t z80Clock_ = 0;
    uint64_t psgClock_ = 0;
    int psgRemainder_ = 0;     // master clocks not yet worth a PSG cycle
    int overclock_ = 100;      // the 68000's speed, percent of the real one
    uint64_t frames_ = 0;
    std::vector<int16_t> psgBuffer_;
    std::vector<uint8_t> loadedThumbnail_;
    std::vector<uint8_t> aheadState_;  // runAhead()'s way back, kept to reuse its memory
    std::vector<CheatPatch> ramCheats_;
    std::vector<std::pair<uint32_t, uint8_t>> romOriginal_;  // bytes the ROM cheats replaced
    void applyRamCheats();
    // A write the watch or the lock looks at; false when the lock drops it.
    bool watchWrite(uint32_t addr, uint8_t value, bool m68k);
    MemoryWatch watch_;
    uint32_t lockAddr_ = 0;
    int lockCount_ = 0;
    bool lockAll_ = false;
    bool watching_ = false;  // watch_ or a lock
    bool drawing_ = true;
    bool fromM68k_ = false;  // the Z80 bus write being made comes from the 68000  // THMB of the last loaded state, until a frame runs
};

}  // namespace plumsg
