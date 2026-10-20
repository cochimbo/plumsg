#include "timbre.h"

#include <algorithm>
#include <cmath>

#include "tsf.h"
#include "ym2612.h"

namespace plumsg {

namespace {

constexpr double kPi = 3.14159265358979;
constexpr double kMasterClock = 53693175.0;
constexpr double kSeconds = 0.5;
constexpr double kFontRate = 44100.0;
constexpr int kMelodic = 112;  // GM 112-127 are percussion and sound effects

double noteHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// dB from -60 to 0 as 0 to 1.
float level(double value, double reference) {
    if (reference <= 0 || value <= 0) return 0;
    return static_cast<float>(std::clamp(10 * std::log10(value / reference) / 60.0 + 1.0, 0.0, 1.0));
}

// The energy in bands a third of an octave wide, from an octave below the note up, of
// 8192 samples from `from` (Hann window, radix-2 FFT): what the sound is made of,
// whether or not its partials fall on the note's harmonics.
void bands(const std::vector<float>& x, size_t from, double rate, double hz,
           std::array<float, TimbreMatcher::kBands>& out) {
    constexpr size_t kSize = 8192;
    std::vector<double> re(kSize, 0.0), im(kSize, 0.0);
    for (size_t i = 0; i < kSize && from + i < x.size(); ++i)
        re[i] = x[from + i] * (0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / (kSize - 1)));
    for (size_t i = 1, j = 0; i < kSize; ++i) {
        size_t bit = kSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= kSize; len <<= 1) {
        const double angle = -2 * kPi / static_cast<double>(len);
        for (size_t i = 0; i < kSize; i += len)
            for (size_t k = 0; k < len / 2; ++k) {
                const double wr = std::cos(angle * static_cast<double>(k)), wi = std::sin(angle * static_cast<double>(k));
                const size_t a = i + k, b = a + len / 2;
                const double tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
    }
    std::array<double, TimbreMatcher::kBands> energy{};
    double loudest = 0;
    const double binHz = rate / kSize;
    for (int band = 0; band < TimbreMatcher::kBands; ++band) {
        const double low = hz * std::pow(2.0, (band - 3.5) / 3.0), high = low * std::pow(2.0, 1.0 / 3.0);
        // A band narrower than a bin (low notes) takes the nearest one.
        const size_t b0 = static_cast<size_t>(std::lround(low / binHz));
        const size_t b1 = std::max(b0, static_cast<size_t>(high / binHz));
        double e = 0;
        for (size_t b = b0; b <= b1 && b < kSize / 2; ++b) e += re[b] * re[b] + im[b] * im[b];
        energy[static_cast<size_t>(band)] = e;
        loudest = std::max(loudest, e);
    }
    for (int band = 0; band < TimbreMatcher::kBands; ++band)
        out[static_cast<size_t>(band)] = level(energy[static_cast<size_t>(band)], loudest);
}

}  // namespace

TimbreMatcher::Print TimbreMatcher::measure(const std::vector<float>& x, double rate, double hz) {
    Print p;
    const size_t window = static_cast<size_t>(rate * 0.025);
    std::array<double, kWindows> rms{};
    double peak = 0;
    for (int w = 0; w < kWindows; ++w) {
        double sum = 0;
        const size_t from = static_cast<size_t>(w) * window, to = std::min(x.size(), from + window);
        for (size_t i = from; i < to; ++i) sum += static_cast<double>(x[i]) * x[i];
        rms[static_cast<size_t>(w)] = to > from ? std::sqrt(sum / static_cast<double>(to - from)) : 0.0;
        peak = std::max(peak, rms[static_cast<size_t>(w)]);
    }
    if (peak < 1e-5) return p;
    p.silent = false;
    for (int w = 0; w < kWindows; ++w) p.envelope[static_cast<size_t>(w)] = level(rms[static_cast<size_t>(w)] * rms[static_cast<size_t>(w)], peak * peak);
    // Early: just past the attack. Late: what is left of the note.
    bands(x, static_cast<size_t>(rate * 0.03), rate, hz, p.early);
    bands(x, static_cast<size_t>(rate * 0.30), rate, hz, p.late);
    return p;
}

TimbreMatcher::Print TimbreMatcher::fmPrint(const std::array<uint8_t, 28>& ops, uint8_t algorithmFeedback, int note) {
    static constexpr uint8_t kCarriers[8] = {0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF};
    static constexpr int kSlot[4] = {0, 8, 4, 12};  // the chip orders operators 1, 3, 2, 4
    Ym2612 ym;
    ym.setMasterClock(kMasterClock);
    auto reg = [&](int r, int value) {
        ym.write(0, static_cast<uint8_t>(r));
        ym.write(1, static_cast<uint8_t>(value));
    };
    reg(0x22, 0);  // no LFO
    reg(0x27, 0);
    reg(0x2B, 0);  // no DAC
    const int algorithm = algorithmFeedback & 7;
    for (int op = 0; op < 4; ++op)
        for (int i = 0; i < 7; ++i) {
            int value = ops[static_cast<size_t>(op * 7 + i)];
            if (i == 1 && (kCarriers[algorithm] & (1 << op))) value = 0;  // carriers at full volume
            reg(0x30 + i * 0x10 + kSlot[op], value);
        }
    reg(0xB0, algorithmFeedback & 0x3F);
    reg(0xB4, 0xC0);
    // The F-number and block for the note, F-number kept in its upper range.
    const double chipRate = ym.sampleRate();
    const double hz = noteHz(note);
    int block = 0;
    double fnum = hz * 1048576.0 / (chipRate * 0.5);
    while (fnum >= 2048 && block < 7) {
        fnum /= 2;
        ++block;
    }
    const int f = std::clamp(static_cast<int>(std::lround(fnum)), 1, 2047);
    reg(0xA4, block << 3 | f >> 8);
    reg(0xA0, f & 0xFF);
    reg(0x28, 0xF0);
    ym.runUntil(static_cast<uint64_t>(kMasterClock * kSeconds));
    const std::vector<int16_t>& s = ym.samples();
    std::vector<float> mono(s.size() / 2);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = (s[i * 2] + s[i * 2 + 1]) / 65536.0f;
    return measure(mono, chipRate, hz);
}

TimbreMatcher::Print TimbreMatcher::fontPrint(tsf* font, int program, int note) {
    const int preset = tsf_get_presetindex(font, 0, program);
    if (preset < 0) return {};
    tsf_reset(font);
    tsf_set_output(font, TSF_MONO, static_cast<int>(kFontRate), 0.0f);
    tsf_note_on(font, preset, note, 100 / 127.0f);
    std::vector<float> mono(static_cast<size_t>(kFontRate * kSeconds));
    tsf_render_float(font, mono.data(), static_cast<int>(mono.size()), 0);
    tsf_note_off_all(font);
    tsf_reset(font);
    return measure(mono, kFontRate, noteHz(note));
}

double TimbreMatcher::distance(const Print& a, const Print& b) {
    if (a.silent || b.silent) return 1e9;
    double spectrum = 0;
    for (int k = 0; k < kBands; ++k)
        spectrum += std::fabs(a.early[static_cast<size_t>(k)] - b.early[static_cast<size_t>(k)]) +
                    0.5 * std::fabs(a.late[static_cast<size_t>(k)] - b.late[static_cast<size_t>(k)]);
    double envelope = 0;
    for (int w = 0; w < kWindows; ++w) envelope += std::fabs(a.envelope[static_cast<size_t>(w)] - b.envelope[static_cast<size_t>(w)]);
    return spectrum / (1.5 * kBands) + envelope / kWindows;
}

int TimbreMatcher::closest(tsf* font, const std::array<uint8_t, 28>& ops, uint8_t algorithmFeedback, int note,
                           int family, std::map<int, Print>* cache) {
    if (!font) return -1;
    note = std::clamp(note, 36, 96);
    const Print fm = fmPrint(ops, algorithmFeedback, note);
    if (fm.silent) return -1;
    int best = -1;
    double bestDistance = 1e9;
    const int first = family >= 0 ? family * 8 : 0, last = family >= 0 ? family * 8 + 8 : kMelodic;
    for (int program = first; program < std::min(last, kMelodic); ++program) {
        Print local;
        const Print* p = &local;
        if (cache) {
            auto c = cache->find(program * 128 + note);
            if (c == cache->end()) c = cache->emplace(program * 128 + note, fontPrint(font, program, note)).first;
            p = &c->second;
        } else {
            local = fontPrint(font, program, note);
        }
        const double d = distance(fm, *p);
        if (d < bestDistance) {
            bestDistance = d;
            best = program;
        }
    }
    return best;
}

TimbreMatcher::TimbreMatcher(tsf* copy) : font_(copy), worker_([this] { run(); }) {}

TimbreMatcher::~TimbreMatcher() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    worker_.join();
    if (font_) tsf_close(font_);
}

void TimbreMatcher::request(uint32_t fingerprint, const std::array<uint8_t, 28>& ops, uint8_t algorithmFeedback,
                            int note, int family) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back({fingerprint, ops, algorithmFeedback, note, family});
    }
    wake_.notify_one();
}

std::vector<std::pair<uint32_t, int>> TimbreMatcher::results() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<uint32_t, int>> out;
    out.swap(done_);
    return out;
}

void TimbreMatcher::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
}

void TimbreMatcher::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
        if (quit_) return;
        const Job job = jobs_.front();
        jobs_.pop_front();
        busy_ = true;
        lock.unlock();
        const int program = closest(font_, job.ops, job.algorithmFeedback, job.note, job.family, &cache_);
        lock.lock();
        busy_ = false;
        if (program >= 0) done_.push_back({job.fingerprint, program});
        if (jobs_.empty()) idle_.notify_all();
    }
}

}  // namespace plumsg
