#pragma once

#include <cstdint>
#include <vector>

namespace plumsg {

// Mixes the two sound chips into one stream at the frontend's rate. The YM2612 runs at
// its own rate (about 53.3 kHz) and is resampled here with linear interpolation; the
// PSG already produces samples at the output rate. Each frame the console hands over
// what both produced; whatever one has more of waits for the next frame.
class AudioMixer {
public:
    void setOutputRate(double hz) { outRate_ = hz; }
    double outputRate() const { return outRate_; }
    void setFmRate(double hz) { fmRate_ = hz; }
    // Relative volumes in percent. The PSG is quieter than the FM on real hardware.
    void setGains(int fm, int psg) {
        fmGain_ = fm;
        psgGain_ = psg;
    }

    // Consumes (and clears) interleaved stereo samples from both chips.
    void mix(std::vector<int16_t>& fm, std::vector<int16_t>& psg);
    void reset();

    // Interleaved stereo samples at outputRate() produced since clearSamples().
    const std::vector<int16_t>& samples() const { return out_; }
    void clearSamples() { out_.clear(); }

    // The resampler's phase and what waits in the queues, so sound continues exactly
    // the same after loading a savestate.
    template <class Ar>
    void state(Ar& ar) {
        ar(pos_, prevL_, prevR_, fmQueue_, psgQueue_);
    }

private:
    double outRate_ = 44100.0;
    double fmRate_ = 53267.0;
    int fmGain_ = 100, psgGain_ = 50;
    double pos_ = 0;  // position in FM input samples after prev_
    int16_t prevL_ = 0, prevR_ = 0;
    std::vector<int16_t> fmQueue_, psgQueue_, out_;
};

}  // namespace plumsg
