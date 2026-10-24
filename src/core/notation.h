#pragma once

#include <vector>

namespace plumsg {

// Writes notes as a score does: the music's tempo and where its bars begin, guessed from
// when the notes start, and each instrument's notes as figures (whole note to sixteenth,
// dotted, tied over the bar lines) with rests between them. For the Partitura. The
// metre is always 4/4: what most game music is in, and what cannot be told apart from a
// slower 2/4 or a faster 4/4 by the notes alone anyway.
namespace notation {

// A sixteenth is the smallest figure written; everything is counted in sixteenths.
constexpr int kBar = 16;

struct Meter {
    double quarter = 0.5;  // seconds a quarter note lasts (0.5 = 120 BPM)
    double origin = 0;     // seconds at which a bar starts
    double sixteenth() const { return quarter / 4; }
    int bpm() const { return static_cast<int>(60.0 / quarter + 0.5); }
    // The sixteenth (counted from origin) nearest a time, and a sixteenth's time.
    long long at(double seconds) const;
    double time(long long sixteenths) const { return origin + static_cast<double>(sixteenths) * sixteenth(); }
};

// The tempo and the bars that fit `onsets` (seconds, any order) best: the quarter is the
// commonest time between notes, doubled or halved into 70-150 BPM and fitted to all of
// them, and the bar starts on the beat where most notes start. `weights` (one per onset,
// or empty) count some notes more for that, the long and low ones.
Meter guessMeter(std::vector<double> onsets, const std::vector<double>& weights = {});
// Where the bars start for a tempo already known.
Meter alignMeter(double quarter, const std::vector<double>& onsets, const std::vector<double>& weights = {});

// A note as played: when it starts and ends (seconds), its pitch and who it came from.
struct Played {
    double start = 0, end = 0;
    int note = 60;
    int id = 0;  // the caller's, given back on the figures
};

// A figure: a chord (one or more notes) or a rest, at a sixteenth for some sixteenths.
struct Figure {
    long long at = 0;     // sixteenths from the meter's origin
    int length = 4;       // 1 sixteenth, 2 eighth, 3 dotted eighth, 4 quarter, 6, 8 half, 12, 16 whole
    bool rest = false;
    bool tied = false;    // tied to the next figure (the same notes go on)
    std::vector<int> notes;  // MIDI notes, low to high
    int id = 0;              // the first note's
};

// The figures of one instrument's notes (any order). Starts and ends go to the nearest
// sixteenth; a note let go a little early is written up to the next one (as played
// staccato); notes starting together are a chord; what is longer than a figure, or goes
// over a bar line, is tied; the silences are rests. From `from` to `to` (sixteenths):
// the rests are written there only.
std::vector<Figure> transcribe(const std::vector<Played>& notes, const Meter& meter, long long from, long long to);

// The figures that a length in sixteenths starting at `at` (within a bar) is written as,
// in order: each as long as fits where it starts. Rests are not dotted.
std::vector<int> split(long long at, int length, bool rest = false);

}  // namespace notation
}  // namespace plumsg
