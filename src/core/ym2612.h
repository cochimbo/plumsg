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

    // The chip's reset line. Samples already made are kept: the Z80's reset, which
    // games pulse mid-frame, resets the YM2612 too, and that sound has played.
    void reset();
    // Brings the chip up to `masterClocks` (a running count), producing samples.
    void runUntil(uint64_t masterClocks);
    // offset 0..3: address/data for ports 0 and 1. Run the chip up to now first.
    void write(int offset, uint8_t value);
    uint8_t readStatus();
    // The chip's registers as last written (0x000-0x0FF port 0, 0x100-0x1FF port 1).
    uint8_t reg(int index) const;
    bool dacEnabled() const;
    // The operators keyed on in each channel (0-5), as last written to register 0x28
    // (bits 4-7). Not kept in savestates.
    uint8_t keys(int channel) const { return keys_[static_cast<size_t>(channel)]; }
    // Turns the FM channels in `mask` (bit 0 = channel 1) down to percent of their
    // volume, and the DAC with bit 6. For the Roland sound, which plays them instead,
    // and for listening to some channels alone.
    void setMuted(uint32_t mask, int percent) {
        muted_ = mask & 0x7F;
        mutedLevel_ = percent * 256 / 100;
    }

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
    int32_t subSample() const;
    uint64_t position_ = 0;  // master clocks already turned into samples
    uint64_t now_ = 0;       // where the last runUntil brought it, maybe partway into a sample
    uint32_t muted_ = 0;
    std::array<uint8_t, 2> address_{};
    std::array<uint8_t, 6> keys_{};
    int32_t mutedLevel_ = 0;  // out of 256
    std::vector<int16_t> samples_;
};

}  // namespace plumsg
