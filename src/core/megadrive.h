#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio_mixer.h"
#include "cartridge.h"
#include "io.h"
#include "m68k.h"
#include "psg.h"
#include "vdp_md.h"
#include "ym2612.h"
#include "z80.h"

namespace plumsg {

// The whole console: 68000 and Z80 with their buses and memory maps, VDP, YM2612, PSG,
// I/O and cartridge, plus the scheduler that interleaves them line by line. It knows
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

    bool loadRom(const std::string& path, std::string* error);  // .bin/.md/.gen/.smd/.zip
    void loadRomData(std::vector<uint8_t> rom);                 // for tests and tools
    void reset();       // the console's reset button: CPUs, VDP and sound; RAM is kept
    void powerCycle();  // also clears memory

    // Auto follows the ROM header. Takes effect on the next reset or power cycle.
    void setRegion(Region r);
    Region region() const { return effectiveRegion_; }
    bool pal() const { return effectiveRegion_ == Region::EuropePal; }
    void setPads(const PadState& p1, const PadState& p2) {
        io_.setPad(0, p1);
        io_.setPad(1, p2);
    }
    void setPadType(int port, PadType t) { io_.setPadType(port, t); }

    void runFrame();  // up to the start of the next frame (just after VBlank ends)

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
    // CPU view of memory, for tools and tests.
    uint8_t peek(uint32_t addr) { return read8(addr); }
    uint16_t peek16(uint32_t addr) { return read16(addr & ~1u); }
    const std::array<uint8_t, 0x10000>& workRam() const { return ram_; }

private:
    // 68000 bus.
    uint8_t read8(uint32_t addr) override;
    uint16_t read16(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t value) override;
    void write16(uint32_t addr, uint16_t value) override;
    void irqAcknowledge(int level) override;
    bool tasWrites() const override { return false; }
    // Z80 bus.
    uint8_t read(uint16_t addr) override;
    void write(uint16_t addr, uint8_t value) override;
    uint8_t in(uint16_t) override { return 0xFF; }
    void out(uint16_t, uint8_t) override {}

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
    int lineClock() const { return static_cast<int>(now_ - lineStart_); }

    template <class Ar>
    void busState(Ar& ar);

    M68000 cpu_{*this};
    Z80 z80_{*this};
    VdpMd vdp_;
    Ym2612 ym_;
    Psg psg_;
    Io io_;
    Cartridge cart_;
    AudioMixer mixer_;

    std::array<uint8_t, 0x10000> ram_{};
    std::array<uint8_t, 0x2000> zram_{};
    uint16_t z80Bank_ = 0;
    bool z80BusReq_ = false;
    bool z80Reset_ = true;

    Region region_ = Region::Auto;
    Region effectiveRegion_ = Region::UsaNtsc;
    uint64_t now_ = 0;         // master clock of whoever is running
    uint64_t lineStart_ = 0;
    uint64_t m68kClock_ = 0;   // master clocks the 68000 has reached
    uint64_t z80Clock_ = 0;
    uint64_t psgClock_ = 0;
    int psgRemainder_ = 0;     // master clocks not yet worth a PSG cycle
    uint64_t frames_ = 0;
    std::vector<int16_t> psgBuffer_;
    std::vector<uint8_t> loadedThumbnail_;  // THMB of the last loaded state, until a frame runs
};

}  // namespace plumsg
