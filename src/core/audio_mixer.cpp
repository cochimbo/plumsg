#include "audio_mixer.h"

#include <algorithm>
#include <cmath>

namespace plumsg {

void AudioMixer::reset() {
    pos_ = 0;
    prevL_ = prevR_ = 0;
    fmQueue_.clear();
    psgQueue_.clear();
    out_.clear();
}

void AudioMixer::mix(std::vector<int16_t>& fm, std::vector<int16_t>& psg) {
    // Resample the FM stream: input sample 0 is the last one of the previous call.
    const size_t n = fm.size() / 2;
    const double step = fmRate_ / outRate_;
    auto at = [&](size_t i, int ch) -> int { return i == 0 ? (ch ? prevR_ : prevL_) : fm[(i - 1) * 2 + ch]; };
    while (n > 0 && pos_ + 1 <= static_cast<double>(n)) {
        const size_t i = static_cast<size_t>(pos_);
        const double f = pos_ - static_cast<double>(i);
        for (int ch = 0; ch < 2; ++ch)
            fmQueue_.push_back(static_cast<int16_t>(std::lround(at(i, ch) + (at(i + 1, ch) - at(i, ch)) * f)));
        pos_ += step;
    }
    if (n > 0) {
        pos_ -= static_cast<double>(n);
        prevL_ = fm[(n - 1) * 2];
        prevR_ = fm[(n - 1) * 2 + 1];
    }
    fm.clear();
    psgQueue_.insert(psgQueue_.end(), psg.begin(), psg.end());
    psg.clear();

    const size_t count = std::min(fmQueue_.size(), psgQueue_.size());
    for (size_t i = 0; i < count; ++i) {
        const int v = (fmQueue_[i] * fmGain_ + psgQueue_[i] * psgGain_) / 100;
        out_.push_back(static_cast<int16_t>(std::clamp(v, -32768, 32767)));
    }
    fmQueue_.erase(fmQueue_.begin(), fmQueue_.begin() + static_cast<long>(count));
    psgQueue_.erase(psgQueue_.begin(), psgQueue_.begin() + static_cast<long>(count));
    // The two streams cover the same time; keep any drift from piling up.
    const size_t limit = static_cast<size_t>(outRate_ / 10) * 2;
    if (fmQueue_.size() > limit) fmQueue_.clear();
    if (psgQueue_.size() > limit) psgQueue_.clear();
}

}  // namespace plumsg
