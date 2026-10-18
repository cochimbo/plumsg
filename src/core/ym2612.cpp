#include "ym2612.h"

#include <algorithm>

#include "ymfm_opn.h"

namespace plumsg {

namespace {

constexpr uint64_t kMasterPerSample = 7 * 144;  // one FM sample every 144 chip clocks

// ymfm's YM2612 with a way to turn some FM channels down: the Roland sound plays their
// notes instead, while the DAC (voices and drums) stays. The mix is ymfm's own
// ym2612::generate, with each muted channel scaled by level/256.
class Chip : public ymfm::ym2612 {
public:
    using ymfm::ym2612::ym2612;

    void generateMuted(output_data* output, uint32_t muted, int32_t level) {
        m_fm.clock(ymfm::ym2612::fm_engine::ALL_CHANNELS);
        output->clear();
        output_data temp;
        const int lastFm = m_dac_enable ? 5 : 6;
        for (int chan = 0; chan < lastFm; ++chan) {
            m_fm.output(temp.clear(), 5, 256, 1 << chan);
            int32_t l = dac_discontinuity(temp.data[0]), r = dac_discontinuity(temp.data[1]);
            if (muted & (1u << chan)) {
                l = l * level / 256;
                r = r * level / 256;
            }
            output->data[0] += l;
            output->data[1] += r;
        }
        if (m_dac_enable) {
            const int32_t dacval = dac_discontinuity(int16_t(m_dac_data << 7) >> 7);
            output->data[0] += m_fm.regs().ch_output_0(0x102) ? dacval : dac_discontinuity(0);
            output->data[1] += m_fm.regs().ch_output_1(0x102) ? dacval : dac_discontinuity(0);
        }
        output->data[0] = (output->data[0] * 128) * 64 / (6 * 65);
        output->data[1] = (output->data[1] * 128) * 64 / (6 * 65);
    }

    uint8_t reg(uint16_t index) const { return const_cast<Chip*>(this)->m_fm.regs().read(index); }
    bool dacEnabled() const { return m_dac_enable != 0; }
};

}  // namespace

// ymfm talks to the outside world through this interface: timers are counted in chip
// clocks and advanced as samples are generated.
struct Ym2612::Impl : public ymfm::ymfm_interface {
    Chip chip{*this};
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
}

void Ym2612::runUntil(uint64_t masterClocks) {
    while (position_ + kMasterPerSample <= masterClocks) {
        position_ += kMasterPerSample;
        ymfm::ym2612::output_data out;
        if (muted_) impl_->chip.generateMuted(&out, muted_, mutedLevel_);
        else impl_->chip.generate(&out, 1);
        impl_->advance(144);
        samples_.push_back(static_cast<int16_t>(std::clamp<int32_t>(out.data[0], -32768, 32767)));
        samples_.push_back(static_cast<int16_t>(std::clamp<int32_t>(out.data[1], -32768, 32767)));
    }
}

void Ym2612::write(int offset, uint8_t value) { impl_->chip.write(static_cast<uint32_t>(offset & 3), value); }

uint8_t Ym2612::readStatus() { return impl_->chip.read_status(); }

uint8_t Ym2612::reg(int index) const { return impl_->chip.reg(static_cast<uint16_t>(index & 0x1FF)); }

bool Ym2612::dacEnabled() const { return impl_->chip.dacEnabled(); }

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
