#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace plumsg {

// Yamaha YM2612 (OPN2): six 4-operator FM channels, channel 3 special mode, the DAC on
// channel 6, LFO, SSG-EG, and timers A and B with their status flags. The synthesis is
// Aaron Giles's ymfm (BSD-3); this class drives it from the console's master clock and
// collects stereo samples at the chip's own rate (clock / 144, about 53.3 kHz).
class Ym2612 {
public:
    Ym2612();
    ~Ym2612();
    Ym2612(const Ym2612&) = delete;
    Ym2612& operator=(const Ym2612&) = delete;

    // The chip runs off the 68000 clock: master / 7.
    void setMasterClock(double hz) { masterClock_ = hz; }
    double sampleRate() const { return masterClock_ / 7.0 / 144.0; }

    void reset();
    // Brings the chip up to `masterClocks` (a running count), producing samples.
    void runUntil(uint64_t masterClocks);
    // offset 0..3: address/data for ports 0 and 1. Run the chip up to now first.
    void write(int offset, uint8_t value);
    uint8_t readStatus();

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
    std::vector<int16_t> samples_;
};

}  // namespace plumsg
