#include "psg.h"

#include <cmath>

namespace plumsg {

namespace {

constexpr int kCyclesPerTick = 16;

// Each attenuation step is 2 dB; 15 is silence. Four channels at full volume
// together just fit in a 16-bit sample.
struct VolumeTable {
    std::array<int, 16> level{};
    VolumeTable() {
        for (int i = 0; i < 15; ++i) level[i] = static_cast<int>(8000.0 * std::pow(10.0, -i / 10.0));
        level[15] = 0;
    }
};
const VolumeTable kVolume;

}  // namespace

void Psg::reset() {
    period_.fill(0);
    attenuation_.fill(15);
    counter_.fill(0);
    output_.fill(1);
    lfsr_ = 0x8000;
    latched_ = 0;
    stereo_ = 0xFF;
    cycleRemainder_ = 0;
    samplePhase_ = 0;
    sumLeft_ = sumRight_ = 0;
    ticksInSample_ = 0;
    samples_.clear();
}

void Psg::write(uint8_t value) {
    if (value & 0x80) latched_ = (value >> 4) & 7;
    const int channel = latched_ >> 1;
    if (latched_ & 1) {
        attenuation_[channel] = value & 0x0F;
    } else if (channel == 3) {
        period_[3] = value & 0x07;  // bit 2: white noise, bits 0-1: rate
        lfsr_ = 0x8000;
    } else if (value & 0x80) {
        period_[channel] = static_cast<uint16_t>((period_[channel] & 0x3F0) | (value & 0x0F));
    } else {
        period_[channel] = static_cast<uint16_t>((period_[channel] & 0x00F) | ((value & 0x3F) << 4));
    }
}

void Psg::tick() {
    for (int ch = 0; ch < 3; ++ch) {
        if (--counter_[ch] > 0) continue;
        counter_[ch] = period_[ch];
        // Periods 0 and 1 hold the output high; games use that to play samples
        // by changing the volume.
        output_[ch] = period_[ch] <= 1 ? 1 : -output_[ch];
    }
    if (--counter_[3] <= 0) {
        const int rate = period_[3] & 3;
        counter_[3] = rate == 3 ? period_[2] : (0x10 << rate);
        output_[3] = -output_[3];
        // The shift register steps on each rising edge of the noise clock.
        if (output_[3] > 0) {
            const bool white = (period_[3] & 4) != 0;
            const uint16_t feedback = white ? ((lfsr_ ^ (lfsr_ >> 3)) & 1) : (lfsr_ & 1);
            lfsr_ = static_cast<uint16_t>((lfsr_ >> 1) | (feedback << 15));
        }
    }

    int left = 0, right = 0;
    for (int ch = 0; ch < 4; ++ch) {
        const int out = ch == 3 ? ((lfsr_ & 1) ? 1 : -1) : output_[ch];
        int v = out * kVolume.level[attenuation_[ch]];
        if (muted_ & (1u << ch)) v = v * mutedLevel_ / 256;
        if (stereo_ & (0x10 << ch)) left += v;
        if (stereo_ & (0x01 << ch)) right += v;
    }
    sumLeft_ += left;
    sumRight_ += right;
    ++ticksInSample_;
}

void Psg::run(int cycles) {
    cycleRemainder_ += cycles;
    while (cycleRemainder_ >= kCyclesPerTick) {
        cycleRemainder_ -= kCyclesPerTick;
        tick();
        // Each sample is the average of the chip ticks it covers, which keeps the
        // high tones from aliasing into noise.
        samplePhase_ += sampleRate_;
        const double tickRate = clock_ / kCyclesPerTick;
        if (samplePhase_ >= tickRate) {
            samplePhase_ -= tickRate;
            samples_.push_back(static_cast<int16_t>(sumLeft_ / ticksInSample_));
            samples_.push_back(static_cast<int16_t>(sumRight_ / ticksInSample_));
            sumLeft_ = sumRight_ = 0;
            ticksInSample_ = 0;
        }
    }
}

}  // namespace plumsg
