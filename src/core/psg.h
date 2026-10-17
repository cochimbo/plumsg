#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace plumsg {

// Texas Instruments SN76489 (the PSG inside the Mega Drive's VDP): three square-wave
// tone channels and one noise channel. Runs off the Z80 clock and produces interleaved
// 16-bit stereo samples at a rate the frontend chooses, so it can nudge the rate to
// keep its queue level. The per-channel stereo switch is a Game Gear leftover; the
// Mega Drive leaves it at both sides.
class Psg {
public:
    static constexpr double kClockNtsc = 3579545.0;  // same clock as the Z80
    static constexpr double kClockPal = 3546894.0;

    Psg() { reset(); }

    void reset();
    void write(uint8_t value);                       // ports 0x40-0x7F
    void setStereo(uint8_t value) { stereo_ = value; }  // port 0x06: bits 4-7 left, 0-3 right
    void setSampleRate(double hz) { sampleRate_ = hz; }
    void setClock(double hz) { clock_ = hz; }

    // Advances by this many Z80 cycles, appending samples as they fall due.
    void run(int cycles);
    // Interleaved left/right samples produced since the last call to clearSamples().
    const std::vector<int16_t>& samples() const { return samples_; }
    void clearSamples() { samples_.clear(); }

    template <class Ar>
    void state(Ar& ar) {
        ar(period_, attenuation_, counter_, output_, lfsr_, latched_, stereo_, cycleRemainder_, samplePhase_,
           sumLeft_, sumRight_, ticksInSample_);
    }

private:
    void tick();  // one step of the chip's internal clock (Z80 clock / 16)

    std::array<uint16_t, 4> period_{};    // 10-bit tone periods; [3] is the noise control
    std::array<uint8_t, 4> attenuation_{};  // 0 loudest .. 15 silent
    std::array<int, 4> counter_{};
    std::array<int, 4> output_{};          // +1 or -1
    uint16_t lfsr_ = 0x8000;
    int latched_ = 0;       // register selected by the last latch byte: channel * 2 + volume
    uint8_t stereo_ = 0xFF;

    int cycleRemainder_ = 0;
    double sampleRate_ = 44100.0;
    double clock_ = kClockNtsc;
    double samplePhase_ = 0;  // chip ticks accumulated toward the next sample, scaled by sampleRate_
    int32_t sumLeft_ = 0, sumRight_ = 0;
    int ticksInSample_ = 0;
    std::vector<int16_t> samples_;
};

}  // namespace plumsg
