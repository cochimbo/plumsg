#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "audio_mixer.h"
#include "psg.h"
#include "ym2413.h"
#include "ym2612.h"

namespace plumsg {

class RolandSound;

// VGM is the usual file for game music as the sound chips played it: every register
// write with the waits between them, at 44100 ticks a second (vgmrips.net has the music
// of most Mega Drive games). The instrument editor records the game to one and plays
// them back.

// Records the YM2612 and the PSG as the console writes to them; on a Master System, the
// PSG, the YM2413 when the console has one and the Game Gear's stereo.
class VgmWriter {
public:
    // Starts at master clock `clock`, writing the chips' registers as they are now so the
    // file plays the same sounds from its first note.
    void begin(const Ym2612& ym, const Psg& psg, uint64_t clock, double masterClock);
    // The same for a Master System (`opll` null without its FM unit) or a Game Gear.
    void beginSms(const Psg& psg, const Ym2413* opll, bool gameGear, uint64_t clock, double masterClock);
    bool recording() const { return recording_; }
    // An address or data write to the YM2612 (offset 0-3, as written to the chip).
    void fm(int offset, uint8_t value, uint64_t clock);
    void psg(uint8_t value, uint64_t clock);
    // An address or data write to the YM2413 (offset 0 or 1), and the Game Gear's stereo.
    void opll(int offset, uint8_t value, uint64_t clock);
    void ggStereo(uint8_t value, uint64_t clock);
    // The YM2612 was reset: its notes stop.
    void fmReset(uint64_t clock);
    double seconds() const { return static_cast<double>(samples_) / 44100.0; }
    // How long it has been recording at master clock `clock`.
    double secondsAt(uint64_t clock) const;
    // Ends the recording and returns the file, gzip-compressed (.vgz) when `compress`.
    // `game` goes in the file's tag.
    std::vector<uint8_t> finish(uint64_t clock, const std::string& game, bool compress = true);

    // A point of the recording, taken at master clock `clock`: to end the file there, or
    // to loop back to it.
    struct Mark {
        size_t bytes = 0;
        uint64_t samples = 0;
    };
    Mark mark(uint64_t clock);
    // The commands written so far.
    const std::vector<uint8_t>& commands() const { return data_; }
    struct Tag {
        std::string title, game, notes = "Grabado jugando";
    };
    // Like finish() above, ending at `end` (at `clock` when null) and looping back to
    // `loop` (when not null).
    std::vector<uint8_t> finish(uint64_t clock, const Tag& tag, bool compress, const Mark* end, const Mark* loop);

private:
    void wait(uint64_t clock);
    void ymWrite(int port, uint8_t reg, uint8_t value);
    void beginPsg(const Psg& psg, uint64_t clock, double masterClock);

    bool recording_ = false;
    enum class Machine : uint8_t { MegaDrive, MasterSystem, GameGear };
    Machine machine_ = Machine::MegaDrive;
    double opllClock_ = 0;  // 0 without a YM2413
    uint8_t opllAddress_ = 0;
    std::vector<uint8_t> data_;
    uint64_t start_ = 0;
    uint64_t samples_ = 0;  // 44100 Hz ticks written so far
    double master_ = 53693175.0;
    double psgClock_ = Psg::kClockNtsc;
    std::array<uint8_t, 2> address_{};
};

// A VGM file's music, and what its tag says.
struct VgmFile {
    std::vector<uint8_t> data;  // uncompressed
    uint32_t start = 0;         // first command
    uint32_t loop = 0;          // where it loops back to, 0 if it does not
    uint64_t samples = 0;       // length in 44100 Hz ticks, without repeating
    uint64_t loopSamples = 0;   // of which the looped part
    double ymClock = 0;         // 0 if the music has no YM2612
    double opllClock = 0;       // 0 if it has no YM2413 (Master System FM)
    double psgClock = 0;        // 0 if it has no PSG
    std::string title, game;
    // From a .vgm or .vgz file's contents; false (and why in *error) if it is not one.
    bool parse(std::vector<uint8_t> bytes, std::string* error);
    double seconds() const { return static_cast<double>(samples) / 44100.0; }
};

// Plays a VGM through a YM2612 and a PSG of its own, and through a Roland translator
// when it has one: the instrument editor's player.
class VgmPlayer {
public:
    VgmPlayer();
    void load(VgmFile file);
    const VgmFile& file() const { return file_; }
    bool loaded() const { return !file_.data.empty(); }

    // The translator that names the instruments and plays them as Roland (not owned).
    void setRoland(RolandSound* roland) { roland_ = roland; }
    // Roland (true) or the chips' own sound.
    void setRolandOutput(bool on);
    // Voices that stay silent (RolandSound's voice bits; bit 10 is the DAC) and
    // instruments that stay silent.
    void setListening(uint32_t mutedVoices, std::set<uint32_t> silenced);
    void setLoop(bool on) { loop_ = on; }

    // Back to the start, or to a point (in seconds) without playing what comes before.
    void restart();
    void seek(double seconds);
    double position() const { return static_cast<double>(position_) / 44100.0; }
    bool finished() const { return ended_; }

    // Plays `seconds` more, appending interleaved stereo samples at `rate` to out.
    void play(double seconds, double rate, std::vector<int16_t>& out);
    // Runs the whole track (once, silently) through the translator, so it sees every
    // instrument; then back to the start.
    void analyse();

    static constexpr uint32_t kDacVoice = 1u << 10;

private:
    struct Stream {  // a DAC stream (VGM commands 0x90-0x95)
        bool active = false;
        int reg = 0x2A;
        int port = 0;
        int step = 1;
        uint32_t frequency = 0;
        uint32_t pos = 0, end = 0, begin = 0;
        bool loop = false;
        double next = 0;  // in 44100 Hz ticks
    };
    void reset();
    // Runs the commands up to master clock `target`. Silent: no sound is made (seeking).
    void runTo(uint64_t target, bool silent);
    bool step(uint64_t clock, bool silent);  // one command; false at the end
    void syncChips(uint64_t clock, bool silent);
    void fmWrite(int port, uint8_t reg, uint8_t value, uint64_t clock, bool silent);
    void psgWrite(uint8_t value, uint64_t clock, bool silent);
    void opllWrite(uint8_t reg, uint8_t value, uint64_t clock, bool silent);
    void startStream(Stream& s, uint32_t offset, int mode, uint32_t length);
    void applyMasks();
    uint64_t clockAt(double samples) const { return static_cast<uint64_t>(samples * master_ / 44100.0); }

    VgmFile file_;
    Ym2612 ym_;
    Ym2413 opll_;
    Psg psg_;
    AudioMixer mixer_;
    RolandSound* roland_ = nullptr;
    bool rolandOutput_ = true;
    bool loop_ = true;
    uint32_t mutedVoices_ = 0;
    std::set<uint32_t> silenced_;
    double master_ = 53693175.0;
    uint32_t pc_ = 0;           // next command
    uint64_t position_ = 0;     // 44100 Hz ticks played, wrapping at the loop
    uint64_t ticks_ = 0;        // and in total, for the clock
    uint64_t waitEnd_ = 0;      // a wait in progress ends here (ticks_)
    uint64_t clock_ = 0;        // master clocks played
    double psgDone_ = 0;        // master clocks the PSG has run
    bool ended_ = false;
    std::vector<uint8_t> pcm_;  // the YM2612 DAC samples from the data blocks
    std::vector<uint32_t> blocks_;  // where each data block starts in pcm_
    uint32_t pcmPos_ = 0;
    std::array<Stream, 8> streams_;
    std::vector<int16_t> fmScratch_;
    std::vector<int16_t> psgScratch_;
};

}  // namespace plumsg
