#include "ym2612.h"

#include <algorithm>

#include "ymfm_opn.h"

namespace plumsg {

namespace {

constexpr uint64_t kMasterPerSample = 7 * 144;  // one FM sample every 144 chip clocks

}  // namespace

// ymfm talks to the outside world through this interface: timers are counted in chip
// clocks and advanced as samples are generated.
struct Ym2612::Impl : public ymfm::ymfm_interface {
    ymfm::ym2612 chip{*this};
    std::array<int32_t, 2> timer{-1, -1};  // chip clocks left, -1 = stopped
    int32_t busy = 0;

    void ymfm_set_timer(uint32_t tnum, int32_t duration) override {
        if (tnum < 2) timer[tnum] = duration;
    }
    void ymfm_set_busy_end(uint32_t clocks) override { busy = static_cast<int32_t>(clocks); }
    bool ymfm_is_busy() override { return busy > 0; }

    void advance(int32_t clocks) {
        busy = std::max(0, busy - clocks);
        for (uint32_t t = 0; t < 2; ++t) {
            if (timer[t] < 0) continue;
            timer[t] -= clocks;
            if (timer[t] <= 0) {
                timer[t] = -1;
                m_engine->engine_timer_expired(t);  // may re-arm it through ymfm_set_timer
            }
        }
    }
};

Ym2612::Ym2612() : impl_(std::make_unique<Impl>()) { reset(); }

Ym2612::~Ym2612() = default;

void Ym2612::reset() {
    impl_->chip.reset();
    impl_->timer = {-1, -1};
    impl_->busy = 0;
    samples_.clear();
}

void Ym2612::runUntil(uint64_t masterClocks) {
    while (position_ + kMasterPerSample <= masterClocks) {
        position_ += kMasterPerSample;
        ymfm::ym2612::output_data out;
        impl_->chip.generate(&out, 1);
        impl_->advance(144);
        samples_.push_back(static_cast<int16_t>(std::clamp<int32_t>(out.data[0], -32768, 32767)));
        samples_.push_back(static_cast<int16_t>(std::clamp<int32_t>(out.data[1], -32768, 32767)));
    }
}

void Ym2612::write(int offset, uint8_t value) { impl_->chip.write(static_cast<uint32_t>(offset & 3), value); }

uint8_t Ym2612::readStatus() { return impl_->chip.read_status(); }

void Ym2612::saveState(std::vector<uint8_t>& out) {
    ymfm::ymfm_saved_state st(out, true);
    impl_->chip.save_restore(st);
    for (auto& t : impl_->timer) st.save_restore(t);
    st.save_restore(impl_->busy);
}

bool Ym2612::loadState(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> current;
    saveState(current);
    if (current.size() != in.size()) return false;  // another ymfm version or garbage
    std::vector<uint8_t> copy = in;
    ymfm::ymfm_saved_state st(copy, false);
    impl_->chip.save_restore(st);
    for (auto& t : impl_->timer) st.save_restore(t);
    st.save_restore(impl_->busy);
    impl_->chip.invalidate_caches();
    return true;
}

}  // namespace plumsg
