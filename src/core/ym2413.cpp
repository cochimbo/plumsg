#include "ym2413.h"

#include <algorithm>
#include <cstring>

#include "ymfm_opl.h"

namespace plumsg {

namespace {

constexpr uint64_t kMasterPerSample = 15 * 72;  // one sample every 72 chip clocks

// ymfm's YM2413 with a way to turn some channels down, as for the YM2612: the mix is
// ymfm's own opll_base::generate, with each muted channel scaled by level/256.
class Chip : public ymfm::ym2413 {
public:
    using ymfm::ym2413::ym2413;

    void generateMuted(output_data* output, uint32_t muted, int32_t level) {
        m_fm.clock(fm_engine::ALL_CHANNELS);
        output->clear();
        for (uint32_t chan = 0; chan < 9; ++chan) {
            output_data temp;
            m_fm.output(temp.clear(), 5, 256, 1u << chan);
            const int32_t scale = (muted & (1u << chan)) ? level : 256;
            for (uint32_t i = 0; i < OUTPUTS; ++i) output->data[i] += temp.data[i] * scale / 256;
        }
        for (uint32_t i = 0; i < OUTPUTS; ++i) output->data[i] = output->data[i] * 128 / 9;
    }
};

}  // namespace

// The OPLL has no timers and no busy flag: the interface is only there for ymfm.
struct Ym2413::Impl : public ymfm::ymfm_interface {
    Chip chip{*this};
};

Ym2413::Ym2413() : impl_(std::make_unique<Impl>()) { reset(); }

Ym2413::~Ym2413() = default;

void Ym2413::reset() {
    impl_->chip.reset();
    address_ = 0;
    std::memset(regs_, 0, sizeof regs_);
}

void Ym2413::runUntil(uint64_t masterClocks) {
    while (position_ + kMasterPerSample <= masterClocks) {
        position_ += kMasterPerSample;
        ymfm::ym2413::output_data out;
        if (muted_) impl_->chip.generateMuted(&out, muted_, mutedLevel_);
        else impl_->chip.generate(&out, 1);
        // Melody and rhythm come out apart; the console adds them.
        const int32_t v = (out.data[0] + out.data[1]) * volume_ / 100;
        const int16_t s = static_cast<int16_t>(std::clamp<int32_t>(v, -32768, 32767));
        samples_.push_back(s);
        samples_.push_back(s);
    }
}

void Ym2413::write(int offset, uint8_t value) {
    if (offset & 1) regs_[address_ & 0x3F] = value;
    else address_ = value;
    impl_->chip.write(static_cast<uint32_t>(offset & 1), value);
}

void Ym2413::saveState(std::vector<uint8_t>& out) {
    ymfm::ymfm_saved_state st(out, true);
    impl_->chip.save_restore(st);
    st.save_restore(address_);
    for (uint8_t& r : regs_) st.save_restore(r);
}

bool Ym2413::loadState(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> current;
    saveState(current);
    if (current.size() != in.size()) return false;  // another ymfm version or garbage
    std::vector<uint8_t> copy = in;
    ymfm::ymfm_saved_state st(copy, false);
    impl_->chip.save_restore(st);
    st.save_restore(address_);
    for (uint8_t& r : regs_) st.save_restore(r);
    impl_->chip.invalidate_caches();
    return true;
}

}  // namespace plumsg
