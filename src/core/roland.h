#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace plumsg {

class Ym2612;
class Ym2413;
class Psg;

// The Roland sound: plays what the YM2612 (or a Master System's YM2413) and the PSG
// are playing as General MIDI
// notes on a SoundFont, like a Roland SC-55 would. It watches the register writes:
// a key on is a note on, the frequency is the note plus pitch bend, the carriers'
// total level is the expression, and each FM instrument (its operator settings) is
// given a General MIDI instrument. The DAC, which plays voices and drums from
// samples, is left to the console. It only adds sound: the console runs exactly the
// same with it or without it.
class RolandSound {
public:
    // The MIDI channels used: one per FM channel, one per PSG tone channel, and the
    // drum channel for the PSG noise and for FM sounds played as drums.
    static constexpr int kFmChannels = 6;
    static constexpr int kPsgFirst = 6;
    static constexpr int kDrums = 9;
    // The console's sound channels, as bits of the masks below: FM 1-6 (bits 0-5), PSG
    // tone 1-3 (bits 6-8) and the PSG noise (bit 9). Each one is a voice. A YM2413 has
    // three more channels (FM 7-9, bits 11-13), and in rhythm mode, in their place, five
    // drums (bits 14-18: bass drum, snare, tom, cymbal, hi-hat). Bit 10 is left to the
    // players for the DAC.
    static constexpr int kVoices = 19;
    static constexpr int kNoise = 9;
    static constexpr int kOpllHigh = 11;  // FM 7
    static constexpr int kRhythmFirst = 14;
    static constexpr uint32_t kAllVoices = ((1u << kVoices) - 1) & ~(1u << 10);
    // The YM2413 channels (bit 0 = channel 1) of which every voice is in `voices`.
    static uint32_t opllChannels(uint32_t voices);

    // What an instrument plays instead of its own sound. Every instrument is known by its
    // fingerprint: for FM, a hash of its operator settings (not its volume); for the PSG,
    // the tone channel or the noise setting.
    struct Assignment {
        int program = -1;    // General MIDI instrument 0-127
        int drum = -1;       // or a piece of the GM drum kit (note 35-81), played on the drum channel
        bool original = false;  // or none: it keeps its own sound from the chip
        int octave = 0;      // -3..3, added to every note
        int volume = 100;    // percent of the note's own loudness
        bool operator==(const Assignment& o) const {
            return program == o.program && drum == o.drum && original == o.original && octave == o.octave &&
                   volume == o.volume;
        }
        bool operator!=(const Assignment& o) const { return !(*this == o); }
    };
    using Assignments = std::map<uint32_t, Assignment>;
    // A YM2413 instrument is FM: one of its 15 built in (opllFingerprint) or a hash of
    // the game's own; its drums are Rhythm.
    enum class Kind : uint8_t { Fm, PsgTone, PsgNoise, Rhythm };
    static Kind kindOf(uint32_t fingerprint);
    static uint32_t psgToneFingerprint(int channel) { return 0x50534700u + static_cast<uint32_t>(channel); }
    static uint32_t noiseFingerprint(int control) { return 0x4E4F4900u + static_cast<uint32_t>(control & 7); }
    static uint32_t opllFingerprint(int instrument) { return 0x4F504C00u + static_cast<uint32_t>(instrument & 15); }
    static uint32_t rhythmFingerprint(int drum) { return 0x4F524800u + static_cast<uint32_t>(drum); }  // 0-4
    // The YM2413's built-in instrument of a fingerprint (1-15), or 0.
    static int opllInstrument(uint32_t fingerprint);
    static const char* opllInstrumentName(int instrument);  // 1-15
    static const char* rhythmName(int drum);                // 0-4

    // An instrument heard since clearSeen(), for the instrument editor.
    struct Seen {
        Kind kind = Kind::Fm;
        std::array<uint8_t, 28> ops{};  // FM: the operator bytes, as programFor() takes them
        uint8_t algorithmFeedback = 0;
        uint32_t channels = 0;  // the voices that played it, as a mask
        int notes = 0;
        int low = 127, high = 0;  // its lowest and highest note
        Assignment rules;  // what the rules give it
    };

    RolandSound();
    ~RolandSound();
    RolandSound(const RolandSound&) = delete;
    RolandSound& operator=(const RolandSound&) = delete;

    // A SoundFont 2 file's contents. False if it is not one.
    bool loadSoundfont(const std::vector<uint8_t>& data);
    // The same SoundFont as `other`, sharing its samples (no second copy in memory).
    bool shareSoundfont(const RolandSound& other);
    bool ready() const;

    // 0..100 each.
    void setEffects(int reverb, int chorus);
    void setVolume(int percent) { volume_ = percent; }

    // The instruments chosen by hand; the rest follow the rules. Takes effect from each
    // instrument's next note.
    void setAssignments(Assignments assigned);
    const Assignments& assignments() const { return assigned_; }
    // The game's sound effects (FM instruments, as Sacar canciones finds them) and
    // whether they keep their own sound from the chip, unless assigned by hand.
    void setSoundEffects(std::set<uint32_t> effects, bool original);
    // What an instrument plays now: its assignment, or what the rules give it.
    Assignment assignmentFor(uint32_t fingerprint) const;

    // For listening in the editor: voices (mask) and instruments that stay silent, in
    // the Roland sound and, through chipChannels(), in the chips'.
    void setListening(uint32_t mutedVoices, std::set<uint32_t> silenced);
    // The voices whose chip sound should be heard: those playing an instrument left
    // original, and not muted.
    uint32_t chipChannels() const { return chipChannels_; }
    // The voices sounding a note now, and the instrument each one plays (0 for none yet).
    uint32_t activeVoices() const;
    uint32_t voiceInstrument(int voice) const { return voices_[static_cast<size_t>(voice)].fingerprint; }

    const std::map<uint32_t, Seen>& seen() const { return seen_; }
    void clearSeen() { seen_.clear(); }

    // Assignment files: one line per instrument, "fingerprint = what [octave volume]",
    // what being a program (0-127), "bateria N" or "original". Lines that make no sense
    // are skipped.
    static Assignments parseAssignments(const std::string& text);
    static std::string formatAssignments(const Assignments& assignments);
    // Names, for the editor and the files.
    static const char* programName(int program);
    static const char* drumName(int note);  // 35-81, else nullptr

    // From the console, as it runs. `clock` is the master clock of the write.
    void beginFrame(uint64_t clock);
    void fmWrite(int offset, uint8_t value, const Ym2612& ym, uint64_t clock);
    void psgWrite(const Psg& psg, uint64_t clock);
    // An address (offset 0) or data (1) write to a Master System's YM2413.
    void opllWrite(int offset, uint8_t value, const Ym2413& opll, uint64_t clock);
    // Adds this frame's notes into `out` (interleaved stereo at `rate`), from sample
    // pair `from` to the end: they cover the frame that ends at `clock`.
    void endFrame(uint64_t clock, std::vector<int16_t>& out, size_t from, double rate);
    // Drops this frame's notes unplayed (and stops what sounds), keeping track of the
    // music: while the chips' own sound is being heard.
    void skipFrame(uint64_t clock);
    // Silences every note: after loading a savestate or rewinding, what was playing
    // is not what the console plays now.
    void allNotesOff();
    // The YM2612 was reset mid-frame: its notes stop there.
    void keysOff(uint64_t clock);

    // Picks a General MIDI program (0-127) for an FM instrument from its registers
    // (the 30 operator bytes and the algorithm/feedback byte) and the first note it
    // played. Public for the tests.
    static int programFor(const std::array<uint8_t, 4 * 7>& ops, uint8_t algorithmFeedback, int note);
    // MIDI note (69 = A 440 Hz, fractional) of a YM2612 frequency setting.
    static double fmNote(int fnum, int block, double chipClock);
    static double psgNote(int period, double psgClock);
    static double opllNote(int fnum, int block, double chipClock);

    // What the translator sent, for the tests: one line per event.
    void setLog(std::vector<std::string>* log) { log_ = log; }

private:
    enum class Ev : uint8_t { NoteOn, NoteOff, Program, Bend, Pan, Expression };
    struct Event {
        uint64_t clock;
        Ev type;
        uint8_t channel;
        int value;  // note, program, bend (0..16383), pan (0..127) or expression (0..127)
        int extra;  // note on: velocity
    };
    // One of the console's sound channels and the note it is playing.
    struct Voice {
        bool on = false;      // a note is sounding (on the chip)
        bool midi = false;    // and it went to the synth (not left original, nor silenced)
        bool drum = false;    // as a piece of the drum kit, which does not bend
        int note = 0;         // the MIDI note sent
        int octave = 0;       // semitones added to the chip's pitch
        int volume = 100;
        uint32_t fingerprint = 0;
        double amplitude = 1.0;  // at note on, for the expression after it
    };
    // What each MIDI channel was last set to.
    struct Channel {
        int program = -1;
        int bend = 8192;
        int pan = 64;
        int expression = 127;
    };

    void emit(uint64_t clock, Ev type, int channel, int value, int extra = 0);
    void noteOn(int voice, uint32_t fingerprint, const Assignment& a, double note, double amplitude, int pan,
                uint64_t clock);
    void noteOff(int voice, uint64_t clock);
    static int velocity(const Voice& v, double amplitude);
    void updateChipChannels();
    void expression(int voice, double amplitude, uint64_t clock);
    void see(int voice, uint32_t fingerprint, const Assignment& rules, int note, const std::array<uint8_t, 28>* ops,
             uint8_t algorithmFeedback);
    void fmKey(int ch, bool on, const Ym2612& ym, uint64_t clock);
    void fmPitch(int ch, const Ym2612& ym, uint64_t clock);
    void fmLevel(int ch, const Ym2612& ym, uint64_t clock);
    double fmAmplitude(int ch, const Ym2612& ym) const;
    uint32_t fmFingerprint(int ch, const Ym2612& ym, std::array<uint8_t, 28>* ops) const;
    void pitchTo(int voice, double note, uint64_t clock);
    void opllKey(int ch, const Ym2413& opll, uint64_t clock);
    double opllNoteOf(int ch, const Ym2413& opll) const;
    void opllRhythm(const Ym2413& opll, uint64_t clock);
    void apply(const Event& e);
    void render(int16_t* out, int frames);

    struct Synth;
    std::unique_ptr<Synth> synth_;
    std::array<Voice, kVoices> voices_{};
    std::array<Channel, 16> channels_{};
    std::array<uint8_t, 2> address_{};  // the register each YM2612 port points at
    uint8_t opllAddress_ = 0;
    uint8_t rhythmKeys_ = 0;            // the YM2413's rhythm keys, as last written (0x0E)
    double opllClock_ = 53693175.0 / 15.0;
    std::map<uint32_t, int> rules_;     // FM fingerprint -> program the rules gave it
    Assignments assigned_;              // chosen by hand
    std::set<uint32_t> effects_;        // the game's sound effects
    bool effectsOriginal_ = false;      // and they sound as on the chip
    std::map<uint32_t, Seen> seen_;
    uint32_t mutedVoices_ = 0;
    std::set<uint32_t> silenced_;
    uint32_t chipChannels_ = 0;
    std::vector<Event> events_;
    uint64_t frameStart_ = 0;
    int volume_ = 100;
    double chipClock_ = 53693175.0 / 7.0;
    std::vector<std::string>* log_ = nullptr;
};

}  // namespace plumsg
