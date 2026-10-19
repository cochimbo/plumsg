#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace plumsg {

// Yamaha YM2413 (OPLL), the FM sound unit of the Japanese Master System and of the
// Mark III's FM add-on: nine 2-operator channels with 15 built-in instruments and one
// of the game's own, or six channels and five rhythm sounds. The synthesis is ymfm's;
// this class drives it from the console's master clock (it runs off the Z80 clock,
// master / 15) and collects stereo samples at its rate (clock / 72, about 49.7 kHz).
class Ym2413 {
public:
    Ym2413();
    ~Ym2413();
    Ym2413(const Ym2413&) = delete;
    Ym2413& operator=(const Ym2413&) = delete;

    void setMasterClock(double hz) { masterClock_ = hz; }
    double sampleRate() const { return masterClock_ / 15.0 / 72.0; }

    void reset();
    // Brings the chip up to `masterClocks` (a running count), producing samples.
    void runUntil(uint64_t masterClocks);
    // offset 0: register address, 1: data. Run the chip up to now first.
    void write(int offset, uint8_t value);
    // The registers as last written (0x00-0x3F).
    uint8_t reg(int index) const { return regs_[static_cast<size_t>(index & 0x3F)]; }
    // Turns the output down to percent (the console's audio control, port 0xF2).
    void setVolume(int percent) { volume_ = percent; }

    // Interleaved left/right samples at sampleRate().
    std::vector<int16_t>& samples() { return samples_; }

    void saveState(std::vector<uint8_t>& out);
    bool loadState(const std::vector<uint8_t>& in);
    uint64_t clockPosition() const { return position_; }
    void setClockPosition(uint64_t p) { position_ = p; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    double masterClock_ = 53693175.0;
    uint64_t position_ = 0;  // master clocks already turned into samples
    uint8_t address_ = 0;
    uint8_t regs_[64] = {};
    int volume_ = 100;
    std::vector<int16_t> samples_;
};

}  // namespace plumsg
