#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace plumsg {

class Ym2612;
class Psg;

// The Roland sound: plays what the YM2612 and the PSG are playing as General MIDI
// notes on a SoundFont, like a Roland SC-55 would. It watches the register writes:
// a key on is a note on, the frequency is the note plus pitch bend, the carriers'
// total level is the expression, and each FM instrument (its operator settings) is
// given a General MIDI instrument. The DAC, which plays voices and drums from
// samples, is left to the console. It only adds sound: the console runs exactly the
// same with it or without it.
class RolandSound {
public:
    // The MIDI channels used: one per FM channel, one per PSG tone channel, and the
    // drum channel for the PSG noise.
    static constexpr int kFmChannels = 6;
    static constexpr int kPsgFirst = 6;
    static constexpr int kDrums = 9;

    RolandSound();
    ~RolandSound();
    RolandSound(const RolandSound&) = delete;
    RolandSound& operator=(const RolandSound&) = delete;

    // A SoundFont 2 file's contents. False if it is not one.
    bool loadSoundfont(const std::vector<uint8_t>& data);
    bool ready() const;

    // 0..100 each.
    void setEffects(int reverb, int chorus);
    void setVolume(int percent) { volume_ = percent; }

    // The instrument an FM sound gets, by its fingerprint; -1 goes back to the rules.
    void setAssignment(uint32_t fingerprint, int program);

    // From the console, as it runs. `clock` is the master clock of the write.
    void beginFrame(uint64_t clock);
    void fmWrite(int offset, uint8_t value, const Ym2612& ym, uint64_t clock);
    void psgWrite(const Psg& psg, uint64_t clock);
    // Adds this frame's notes into `out` (interleaved stereo at `rate`), from sample
    // pair `from` to the end: they cover the frame that ends at `clock`.
    void endFrame(uint64_t clock, std::vector<int16_t>& out, size_t from, double rate);
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
    struct Voice {
        bool on = false;
        int note = 0;
        int program = -1;
        int bend = 8192;
        int pan = 64;
        int expression = 127;
        double amplitude = 1.0;  // at note on, for the expression after it
    };

    void emit(uint64_t clock, Ev type, int channel, int value, int extra = 0);
    void fmKey(int ch, bool on, const Ym2612& ym, uint64_t clock);
    void fmPitch(int ch, const Ym2612& ym, uint64_t clock);
    void fmLevel(int ch, const Ym2612& ym, uint64_t clock);
    double fmAmplitude(int ch, const Ym2612& ym) const;
    uint32_t fmFingerprint(int ch, const Ym2612& ym, std::array<uint8_t, 28>* ops) const;
    void pitchTo(int ch, double note, uint64_t clock, int velocity);
    void apply(const Event& e);
    void render(int16_t* out, int frames);

    struct Synth;
    std::unique_ptr<Synth> synth_;
    std::array<Voice, 16> voices_{};
    std::array<uint8_t, 2> address_{};  // the register each YM2612 port points at
    std::map<uint32_t, int> rules_;     // fingerprint -> program the rules gave it
    std::map<uint32_t, int> assigned_;  // fingerprint -> program chosen by hand
    std::vector<Event> events_;
    uint64_t frameStart_ = 0;
    int volume_ = 100;
    double chipClock_ = 53693175.0 / 7.0;
    std::vector<std::string>* log_ = nullptr;
};

}  // namespace plumsg
