#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../core/notation.h"
#include "../core/roland.h"

namespace plumsg {

// The music the score shows: the instrument editor's track (all of it, known
// beforehand) or the game's (what it has played so far).
struct ScoreMusic {
    const RolandSound::Score* notes = nullptr;
    double clockRate = RolandSound::kScoreClock;  // the notes' clock, ticks per second
    double now = 0;        // seconds: where the music is
    double length = 0;     // seconds of the whole track; 0 for a game
    bool playing = false;
    bool track = false;    // a track: it stops and goes back to the start, and the playhead seeks
    bool canSpeed = true;  // x0.5 and x2 can be chosen (not in hardcore, nor in a netplay game)
    const RolandSound* sound = nullptr;          // what each instrument plays, for its name
    const std::set<uint32_t>* silenced = nullptr;  // the instruments switched off
};

// What the score's buttons ask for this frame.
struct ScoreRequests {
    bool togglePlay = false;
    bool stop = false;
    double seek = -1;           // seconds, a track only
    uint32_t toggleSound = 0;   // switch this instrument off, or back on
};

// The score (Sonido > Partitura), like GarageBand's: a track per instrument, each with
// its staff (treble or bass clef, by where its notes lie) in 4/4 at the tempo guessed
// from the music (or set by hand), its notes written as figures (notation::transcribe)
// with their stems, flags, beams, dots, ties and accidentals, the rests, and the bar
// lines. A playhead runs over them; on a track it can be dragged to go elsewhere. The
// view follows the playhead, or stays where it was dragged to until Seguir. Each
// instrument can be switched off.
class ScoreWindow {
public:
    bool isOpen() const { return open_; }
    void setOpen(bool open) { open_ = open; }
    // The speed the music plays at (0.5, 1 or 2) while the window is open.
    double speed() const { return open_ ? speed_ : 1.0; }
    // Draws the window; call between ImGui::NewFrame and ImGui::Render. `music` is what
    // plays now (null for nothing).
    void draw(const ScoreMusic* music, ScoreRequests& out);

    // Where a MIDI note is written: its step on the staff (C4 = 28, a step per line or
    // space) and whether it carries a sharp. Public for the tests.
    static int staffStep(int note, bool* sharp);

private:
    struct Row {
        uint32_t fingerprint = 0;
        uint32_t voices = 0;  // the channels that played it
        bool treble = true;
        int index = 0;        // for its colour
        std::vector<notation::Played> played;  // its notes; end < 0 while one still sounds
    };
    void updateRows(const ScoreMusic& music);
    void drawToolbar(const ScoreMusic& music, ScoreRequests& out);
    void updateMeter(const ScoreMusic& music);

    bool open_ = false;
    double speed_ = 1.0;
    float seconds_ = 5.0f;    // how much of the music the view spans
    bool follow_ = true;      // the view keeps the playhead in sight
    double viewStart_ = 0;    // seconds at its left edge, when not following
    double dragSeek_ = -1;    // where the playhead is being dragged to
    bool panned_ = false;     // the staff being pressed was dragged (else it is a click)
    // The tracks, made again when the notes change.
    std::vector<Row> rows_;
    const RolandSound::Score* rowsFor_ = nullptr;
    size_t rowsNotes_ = 0;
    uint64_t rowsLast_ = 0;
    // The tempo and where the bars start: guessed, or the tempo given by hand.
    notation::Meter meter_, guessed_;
    bool tempoAuto_ = true;
    int bpm_ = 120;
    double meterAt_ = -1;  // music.now when the guess was last made (a game's is made again)
    const RolandSound::Score* meterFor_ = nullptr;  // and of which notes
    size_t meterNotes_ = 0;
};

}  // namespace plumsg
