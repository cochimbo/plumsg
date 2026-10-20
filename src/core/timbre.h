#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

struct tsf;

namespace plumsg {

// Picks the General MIDI instrument of a SoundFont that sounds most like an FM
// instrument: both are played, unheard, on the same note, and compared by what each
// one sounds like (how its energy spreads over the spectrum early in the note and later
// on, and how its loudness goes over the note).
class TimbreMatcher {
public:
    static constexpr int kBands = 18;    // a third of an octave each, from an octave below the note
    static constexpr int kWindows = 18;  // of 25 ms: the first 450 ms of the note (500 are played)
    // What a note sounds like, each value from 0 to 1.
    struct Print {
        std::array<float, kBands> early{}, late{};  // energy, 60 dB below the loudest band is 0
        std::array<float, kWindows> envelope{};      // loudness, 60 dB below its peak is 0
        bool silent = true;
    };
    static Print measure(const std::vector<float>& mono, double rate, double hz);
    // An FM instrument (the 28 operator bytes, as RolandSound::Seen has them, and the
    // algorithm/feedback byte) playing MIDI note `note` on a YM2612.
    static Print fmPrint(const std::array<uint8_t, 28>& ops, uint8_t algorithmFeedback, int note);
    // A program of the SoundFont (bank 0) playing the same note; silent if it has none.
    static Print fontPrint(tsf* font, int program, int note);
    // 0 for the same sound; the larger, the less alike.
    static double distance(const Print& a, const Print& b);
    // The program of the SoundFont closest to the FM instrument, or -1: one of General
    // MIDI's family `family` (0-13: programs family * 8 to family * 8 + 7), or with -1 any
    // melodic one (0-111: the percussion and sound effects are left out). `cache` keeps
    // the SoundFont's prints.
    static int closest(tsf* font, const std::array<uint8_t, 28>& ops, uint8_t algorithmFeedback, int note, int family,
                       std::map<int, Print>* cache = nullptr);

    // In the background, so the console does not wait: takes a copy of the SoundFont
    // (tsf_copy, made by the caller), closed when this is destroyed.
    explicit TimbreMatcher(tsf* copy);
    ~TimbreMatcher();
    TimbreMatcher(const TimbreMatcher&) = delete;
    TimbreMatcher& operator=(const TimbreMatcher&) = delete;
    void request(uint32_t fingerprint, const std::array<uint8_t, 28>& ops, uint8_t algorithmFeedback, int note,
                 int family);
    // The instruments finished since the last call: fingerprint and program.
    std::vector<std::pair<uint32_t, int>> results();
    // Waits for every request to be finished (for the tests and the headless runs).
    void wait();

private:
    struct Job {
        uint32_t fingerprint;
        std::array<uint8_t, 28> ops;
        uint8_t algorithmFeedback;
        int note;
        int family;
    };
    void run();

    tsf* font_;
    std::map<int, Print> cache_;  // the worker's: program * 128 + note
    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::deque<Job> jobs_;
    std::vector<std::pair<uint32_t, int>> done_;
    bool busy_ = false, quit_ = false;
    std::thread worker_;
};

}  // namespace plumsg
