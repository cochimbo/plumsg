#pragma once

#include <cmath>
#include <cstddef>

namespace plumsg {

// How loud a sound is, as ITU-R BS.1770 (LUFS) hears it: the samples go through its
// K-weighting (a high shelf of +4 dB above about 1.7 kHz, and a high-pass at 38 Hz
// that leaves out the deep bass and any DC), and the squares of both sides are summed. The Roland
// sound measures with it what it plays and what the chips would have played, to play
// as loud as the game.
class LoudnessMeter {
public:
    void setRate(double hz) {
        if (std::fabs(hz - rate_) < 50.0 || hz <= 0) return;  // the PSG's rate moves a little
        rate_ = hz;
        // The two filters of BS.1770, for any sample rate (as libebur128 makes them).
        for (int side = 0; side < 2; ++side) {
            shelf_[side].set(rate_, 1681.974450955533, 0.7071752369554196, 3.999843853973347, true);
            highPass_[side].set(rate_, 38.13547087602444, 0.5003270373238773, 0.0, false);
        }
    }
    double rate() const { return rate_; }

    void add(double left, double right) {
        const double l = highPass_[0].run(shelf_[0].run(left)), r = highPass_[1].run(shelf_[1].run(right));
        sum_ += l * l + r * r;
        ++count_;
    }
    // The sum of the weighted squares since clear(), and how many samples.
    double sum() const { return sum_; }
    size_t count() const { return count_; }
    double meanSquare() const { return count_ ? sum_ / static_cast<double>(count_) : 0.0; }
    void clear() {
        sum_ = 0;
        count_ = 0;
    }

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void set(double rate, double f, double q, double db, bool shelf) {
            const double k = std::tan(3.14159265358979323846 * f / rate);
            if (shelf) {
                const double vh = std::pow(10.0, db / 20.0), vb = std::pow(vh, 0.4996667741545416);
                const double a0 = 1.0 + k / q + k * k;
                b0 = (vh + vb * k / q + k * k) / a0;
                b1 = 2.0 * (k * k - vh) / a0;
                b2 = (vh - vb * k / q + k * k) / a0;
                a1 = 2.0 * (k * k - 1.0) / a0;
                a2 = (1.0 - k / q + k * k) / a0;
            } else {
                const double a0 = 1.0 + k / q + k * k;
                b0 = 1.0;
                b1 = -2.0;
                b2 = 1.0;
                a1 = 2.0 * (k * k - 1.0) / a0;
                a2 = (1.0 - k / q + k * k) / a0;
            }
            z1 = z2 = 0;
        }
        double run(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    double rate_ = 0;
    Biquad shelf_[2], highPass_[2];
    double sum_ = 0;
    size_t count_ = 0;
};

}  // namespace plumsg
