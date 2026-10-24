#include "notation.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace plumsg {
namespace notation {

namespace {

constexpr double kPi = 3.14159265358979;

long long floorDiv(long long a, long long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

}  // namespace

long long Meter::at(double seconds) const { return std::llround((seconds - origin) / sixteenth()); }

Meter alignMeter(double quarter, const std::vector<double>& onsets, const std::vector<double>& weights) {
    Meter m;
    m.quarter = std::max(0.05, quarter);
    m.origin = 0;
    if (onsets.empty()) return m;
    const double u = m.sixteenth();
    auto weight = [&](size_t i) { return i < weights.size() ? weights[i] : 1.0; };
    // The phase of the sixteenths' grid that most notes fall on.
    double c = 0, s = 0;
    for (size_t i = 0; i < onsets.size(); ++i) {
        const double a = 2 * kPi * onsets[i] / u;
        c += weight(i) * std::cos(a);
        s += weight(i) * std::sin(a);
    }
    double origin = (c == 0 && s == 0) ? onsets.front() : std::atan2(s, c) / (2 * kPi) * u;
    // The bar starts on the sixteenth where the most (and heaviest) notes fall on its
    // beats: the first counting most, then the third, then the other two.
    double best = -1;
    int offset = 0;
    for (int b = 0; b < kBar; ++b) {
        double score = 0;
        for (size_t i = 0; i < onsets.size(); ++i) {
            const long long k = std::llround((onsets[i] - origin) / u);
            const long long inBar = ((k - b) % kBar + kBar) % kBar;
            score += weight(i) * (inBar == 0 ? 1.0 : inBar == 8 ? 0.5 : inBar % 4 == 0 ? 0.25 : 0.0);
        }
        if (score > best + 1e-9) {
            best = score;
            offset = b;
        }
    }
    origin += offset * u;
    const double bar = m.quarter * 4;
    m.origin = origin - std::floor(origin / bar) * bar;
    return m;
}

Meter guessMeter(std::vector<double> onsets, const std::vector<double>& weights) {
    std::vector<std::pair<double, double>> timed;  // onset, weight
    for (size_t i = 0; i < onsets.size(); ++i) timed.push_back({onsets[i], i < weights.size() ? weights[i] : 1.0});
    std::sort(timed.begin(), timed.end());
    // Notes within 20 ms start together.
    std::vector<double> at, w;
    for (const auto& [t, wt] : timed) {
        if (!at.empty() && t - at.back() < 0.02) {
            w.back() = std::max(w.back(), wt);
            continue;
        }
        at.push_back(t);
        w.push_back(wt);
    }
    std::vector<double> gaps;
    for (size_t i = 1; i < at.size(); ++i) {
        const double g = at[i] - at[i - 1];
        if (g >= 0.06 && g <= 2.0) gaps.push_back(g);
    }
    if (gaps.size() > 1500) gaps.erase(gaps.begin(), gaps.end() - 1500);
    if (gaps.size() < 3) return alignMeter(0.5, at, w);
    // The commonest gap, within 6%.
    double common = gaps.front();
    size_t most = 0;
    for (double g : gaps) {
        size_t n = 0;
        for (double h : gaps) n += std::fabs(h - g) <= g * 0.06;
        if (n > most || (n == most && g > common)) {
            most = n;
            common = g;
        }
    }
    double quarter = common;
    while (quarter < 0.4) quarter *= 2;
    while (quarter > 0.857) quarter /= 2;
    // Fitted to every gap that lies near a whole number of sixteenths.
    double u = quarter / 4;
    for (int pass = 0; pass < 2; ++pass) {
        double num = 0, den = 0;
        for (double g : gaps) {
            const double k = std::round(g / u);
            if (k < 1 || k > 32 || std::fabs(g / u - k) > 0.3) continue;
            num += g * k;
            den += k * k;
        }
        if (den > 0) u = num / den;
    }
    return alignMeter(u * 4, at, w);
}

std::vector<int> split(long long at, int length, bool rest) {
    std::vector<int> out;
    while (length > 0) {
        const int p = static_cast<int>(((at % kBar) + kBar) % kBar);
        int v = 1;
        // The longest figure that fits and starts where it may: a whole note on the bar,
        // a half on a beat, a quarter on an eighth... Rests are not dotted.
        for (int c : {16, 12, 8, 6, 4, 3, 2, 1}) {
            if (c > length || p + c > kBar) continue;
            if (rest && (c == 12 || c == 6 || c == 3)) continue;
            const bool fits = c == 16 ? p == 0 : c >= 8 ? p % 4 == 0 : c >= 3 ? p % 2 == 0 : c == 2 ? p % 2 == 0 : true;
            if (rest && c >= 4 && p % 4 != 0) continue;
            if (fits) {
                v = c;
                break;
            }
        }
        out.push_back(v);
        at += v;
        length -= v;
    }
    return out;
}

std::vector<Figure> transcribe(const std::vector<Played>& notes, const Meter& meter, long long from, long long to) {
    // Chords: the notes starting on the same sixteenth.
    struct Chord {
        long long start = 0, end = 0;
        std::vector<int> notes;
        int id = 0;
    };
    std::map<long long, Chord> byStart;
    for (const Played& p : notes) {
        const long long s = meter.at(p.start);
        const long long e = std::max(s + 1, meter.at(p.end));
        auto [it, fresh] = byStart.try_emplace(s);
        Chord& c = it->second;
        if (fresh) {
            c.start = s;
            c.id = p.id;
        }
        c.end = std::max(c.end, e);
        if (std::find(c.notes.begin(), c.notes.end(), p.note) == c.notes.end()) c.notes.push_back(p.note);
    }
    std::vector<Chord> chords;
    for (auto& [s, c] : byStart) chords.push_back(std::move(c));
    for (size_t i = 0; i < chords.size(); ++i) {
        Chord& c = chords[i];
        std::sort(c.notes.begin(), c.notes.end());
        if (i + 1 < chords.size()) {
            const long long next = chords[i + 1].start;
            // One line: a note ends where the next one starts, and one let go early (held
            // half the time to the next one or more) is written up to it.
            if (c.end > next || (next - c.start <= kBar && 2 * (c.end - c.start) >= next - c.start)) c.end = next;
        }
    }

    std::vector<Figure> out;
    auto write = [&](long long start, long long end, const Chord* chord) {
        // Cut at the bar lines, then into figures; the pieces of a note are tied.
        for (long long at = start; at < end;) {
            const long long barEnd = (floorDiv(at, kBar) + 1) * kBar;
            const long long stop = std::min(end, barEnd);
            for (int v : split(at, static_cast<int>(stop - at), chord == nullptr)) {
                Figure f;
                f.at = at;
                f.length = v;
                f.rest = chord == nullptr;
                if (chord) {
                    f.notes = chord->notes;
                    f.id = chord->id;
                    f.tied = at + v < end;
                }
                out.push_back(std::move(f));
                at += v;
            }
        }
    };
    long long cursor = from;
    for (const Chord& c : chords) {
        if (c.end <= from) continue;
        if (c.start >= to) break;
        if (c.start > cursor) write(cursor, c.start, nullptr);
        write(c.start, c.end, &c);
        cursor = std::max(cursor, c.end);
    }
    if (cursor < to) write(cursor, to, nullptr);
    return out;
}

}  // namespace notation
}  // namespace plumsg
