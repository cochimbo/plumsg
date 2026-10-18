#include "roland.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "psg.h"
#include "ym2612.h"

#define TSF_IMPLEMENTATION
#define TSF_NO_STDIO
#include "tsf.h"

namespace plumsg {

namespace {

constexpr int kBendRange = 12;  // semitones either way, so slides stay one note
constexpr int kPsgDefault = 80;  // Lead 1 (square): what a PSG tone sounds like

// The register of channel `ch` (0-5) for a per-channel base (0xA0, 0xA4, 0xB0, 0xB4).
int chReg(int ch, int base) { return (ch / 3) * 0x100 + base + ch % 3; }
// The register of operator `op` (0-3 = operators 1-4) of channel `ch` for a base
// (0x30 ... 0x90). The chip orders them 1, 3, 2, 4.
int opReg(int ch, int op, int base) {
    static constexpr int kSlot[4] = {0, 8, 4, 12};
    return (ch / 3) * 0x100 + base + kSlot[op] + ch % 3;
}
// Which operators are carriers (heard) for each algorithm.
constexpr uint8_t kCarriers[8] = {0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF};

// A Schroeder reverb (four combs and two all-pass filters per side, Freeverb's
// tunings) and a chorus (one delay line swept by a slow sine), like the SC-55's
// own effects. Both are sends: a part of the dry sound goes in, the result is added.
class Effects {
public:
    void setRate(double rate) {
        if (rate == rate_) return;
        rate_ = rate;
        const double scale = rate / 44100.0;
        static constexpr int kCombs[4] = {1116, 1188, 1277, 1356};
        static constexpr int kAllPasses[2] = {556, 441};
        for (int side = 0; side < 2; ++side) {
            const int spread = side ? 23 : 0;
            for (int i = 0; i < 4; ++i) combs_[side][i].assign(static_cast<size_t>((kCombs[i] + spread) * scale), 0.0f);
            for (int i = 0; i < 2; ++i) allPasses_[side][i].assign(static_cast<size_t>((kAllPasses[i] + spread) * scale), 0.0f);
        }
        combPos_ = {};
        allPassPos_ = {};
        combFilter_ = {};
        chorus_.assign(static_cast<size_t>(rate * 0.05) * 2, 0.0f);
        chorusPos_ = 0;
        phase_ = 0;
    }
    void setAmounts(int reverb, int chorus) {
        reverb_ = std::clamp(reverb, 0, 100) / 100.0f;
        chorusAmount_ = std::clamp(chorus, 0, 100) / 100.0f;
    }

    void process(float* buf, int frames) {
        if (rate_ <= 0) return;
        const size_t chorusLen = chorus_.size() / 2;
        for (int i = 0; i < frames; ++i) {
            float& l = buf[i * 2];
            float& r = buf[i * 2 + 1];
            // Chorus: a copy 10-20 ms late, its delay swaying at 0.6 Hz.
            if (chorusAmount_ > 0 && chorusLen > 0) {
                chorus_[chorusPos_ * 2] = l;
                chorus_[chorusPos_ * 2 + 1] = r;
                const double delay = rate_ * (0.015 + 0.005 * std::sin(phase_));
                phase_ += 2 * 3.14159265358979 * 0.6 / rate_;
                if (phase_ > 2 * 3.14159265358979) phase_ -= 2 * 3.14159265358979;
                double at = static_cast<double>(chorusPos_) - delay;
                while (at < 0) at += static_cast<double>(chorusLen);
                const size_t a = static_cast<size_t>(at) % chorusLen, b = (a + 1) % chorusLen;
                const float f = static_cast<float>(at - std::floor(at));
                const float cl = chorus_[a * 2] + (chorus_[b * 2] - chorus_[a * 2]) * f;
                const float cr = chorus_[a * 2 + 1] + (chorus_[b * 2 + 1] - chorus_[a * 2 + 1]) * f;
                chorusPos_ = (chorusPos_ + 1) % chorusLen;
                l += cr * chorusAmount_ * 0.5f;  // crossed, for width
                r += cl * chorusAmount_ * 0.5f;
            }
            if (reverb_ <= 0) continue;
            const float in = (l + r) * 0.015f * reverb_;
            float out[2] = {0, 0};
            for (int side = 0; side < 2; ++side) {
                for (int c = 0; c < 4; ++c) {
                    std::vector<float>& line = combs_[side][c];
                    size_t& pos = combPos_[side][c];
                    const float y = line[pos];
                    float& filter = combFilter_[side][c];
                    filter = y * 0.8f + filter * 0.2f;  // damping
                    line[pos] = in + filter * 0.84f;     // room size
                    pos = (pos + 1) % line.size();
                    out[side] += y;
                }
                for (int a = 0; a < 2; ++a) {
                    std::vector<float>& line = allPasses_[side][a];
                    size_t& pos = allPassPos_[side][a];
                    const float b = line[pos];
                    line[pos] = out[side] + b * 0.5f;
                    out[side] = b - out[side];
                    pos = (pos + 1) % line.size();
                }
            }
            l += out[0] * 3.0f;
            r += out[1] * 3.0f;
        }
    }

private:
    double rate_ = 0;
    float reverb_ = 0.4f, chorusAmount_ = 0.2f;
    std::array<std::array<std::vector<float>, 4>, 2> combs_;
    std::array<std::array<std::vector<float>, 2>, 2> allPasses_;
    std::array<std::array<size_t, 4>, 2> combPos_{};
    std::array<std::array<size_t, 2>, 2> allPassPos_{};
    std::array<std::array<float, 4>, 2> combFilter_{};
    std::vector<float> chorus_;
    size_t chorusPos_ = 0;
    double phase_ = 0;
};

}  // namespace

struct RolandSound::Synth {
    tsf* font = nullptr;
    int rate = 0;  // the rate tsf renders at, fixed once set
    Effects effects;
    std::vector<float> buffer;

    ~Synth() {
        if (font) tsf_close(font);
    }
};

RolandSound::RolandSound() : synth_(std::make_unique<Synth>()) {}

RolandSound::~RolandSound() = default;

bool RolandSound::loadSoundfont(const std::vector<uint8_t>& data) {
    tsf* font = data.empty() ? nullptr : tsf_load_memory(data.data(), static_cast<int>(data.size()));
    if (!font) return false;
    if (synth_->font) tsf_close(synth_->font);
    synth_->font = font;
    synth_->rate = 0;
    voices_ = {};
    return true;
}

bool RolandSound::ready() const { return synth_->font != nullptr; }

void RolandSound::setEffects(int reverb, int chorus) { synth_->effects.setAmounts(reverb, chorus); }

void RolandSound::setAssignment(uint32_t fingerprint, int program) {
    if (program < 0) assigned_.erase(fingerprint);
    else assigned_[fingerprint] = std::clamp(program, 0, 127);
}

double RolandSound::fmNote(int fnum, int block, double chipClock) {
    if (fnum <= 0) return 0;
    const double hz = fnum * std::ldexp(1.0, block - 1) * (chipClock / 144.0) / 1048576.0;
    return 69.0 + 12.0 * std::log2(hz / 440.0);
}

double RolandSound::psgNote(int period, double psgClock) {
    if (period <= 0) return 0;
    const double hz = psgClock / (32.0 * period);
    return 69.0 + 12.0 * std::log2(hz / 440.0);
}

// The rules, until an instrument is assigned by hand: low notes are a bass, every
// operator heard at once is an organ, a slow attack is strings, a sound that dies away
// is a piano or a bell, two or three carriers held is brass, and the rest a lead.
int RolandSound::programFor(const std::array<uint8_t, 28>& ops, uint8_t algorithmFeedback, int note) {
    const int algorithm = algorithmFeedback & 7;
    const int feedback = (algorithmFeedback >> 3) & 7;
    // ops: for each operator 1-4, its bytes at 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90.
    int attack = 31, decay = 0, sustainLevel = 0, carriers = 0;
    for (int op = 0; op < 4; ++op) {
        if (!(kCarriers[algorithm] & (1 << op))) continue;
        const uint8_t* o = &ops[static_cast<size_t>(op) * 7];
        attack = std::min(attack, o[2] & 0x1F);
        decay = std::max(decay, std::max(o[3] & 0x1F, o[4] & 0x1F));
        sustainLevel = std::max(sustainLevel, o[5] >> 4);
        ++carriers;
    }
    const bool diesAway = decay >= 14 && sustainLevel >= 6;
    if (note < 48) return diesAway ? 34 : 38;  // Electric Bass (pick), Synth Bass 1
    if (algorithm == 7) return 16;              // Drawbar Organ
    if (attack < 18) return 48;                 // String Ensemble 1
    if (diesAway) return note >= 84 ? 9 : 4;    // Glockenspiel, Electric Piano 1
    if (carriers >= 2) return 61;               // Brass Section
    return feedback >= 6 ? 30 : 81;             // Distortion Guitar, Lead 2 (sawtooth)
}

void RolandSound::emit(uint64_t clock, Ev type, int channel, int value, int extra) {
    events_.push_back({clock, type, static_cast<uint8_t>(channel), value, extra});
    if (!log_) return;
    static const char* kNames[] = {"on", "off", "program", "bend", "pan", "expression"};
    char line[64];
    std::snprintf(line, sizeof line, "%d %s %d", channel, kNames[static_cast<int>(type)], value);
    log_->push_back(line);
}

void RolandSound::beginFrame(uint64_t clock) { frameStart_ = clock; }

void RolandSound::allNotesOff() {
    events_.clear();
    for (Voice& v : voices_) v.on = false;
    if (synth_->font) tsf_note_off_all(synth_->font);
}

void RolandSound::keysOff(uint64_t clock) {
    for (int ch = 0; ch < kFmChannels; ++ch) {
        Voice& v = voices_[static_cast<size_t>(ch)];
        if (!v.on) continue;
        v.on = false;
        emit(clock, Ev::NoteOff, ch, v.note);
    }
}

uint32_t RolandSound::fmFingerprint(int ch, const Ym2612& ym, std::array<uint8_t, 28>* ops) const {
    const uint8_t algorithmFeedback = ym.reg(chReg(ch, 0xB0));
    const int algorithm = algorithmFeedback & 7;
    uint32_t hash = 2166136261u;  // FNV-1a
    auto add = [&](uint8_t b) { hash = (hash ^ b) * 16777619u; };
    add(algorithmFeedback & 0x3F);
    for (int op = 0; op < 4; ++op) {
        for (int i = 0; i < 7; ++i) {
            uint8_t b = ym.reg(opReg(ch, op, 0x30 + i * 0x10));
            if (ops) (*ops)[static_cast<size_t>(op * 7 + i)] = b;
            // A carrier's total level is its volume, not its sound.
            if (i == 1 && (kCarriers[algorithm] & (1 << op))) b = 0;
            add(b);
        }
    }
    return hash;
}

double RolandSound::fmAmplitude(int ch, const Ym2612& ym) const {
    const int algorithm = ym.reg(chReg(ch, 0xB0)) & 7;
    int level = 127;
    for (int op = 0; op < 4; ++op)
        if (kCarriers[algorithm] & (1 << op)) level = std::min(level, ym.reg(opReg(ch, op, 0x40)) & 0x7F);
    return std::pow(10.0, -0.75 * level / 20.0);  // 0.75 dB a step
}

void RolandSound::pitchTo(int ch, double note, uint64_t clock, int velocity) {
    Voice& v = voices_[static_cast<size_t>(ch)];
    if (note <= 0) return;
    // Within the bend range the note bends; further, it is played again at the new note.
    if (v.on && std::fabs(note - v.note) > kBendRange) {
        emit(clock, Ev::NoteOff, ch, v.note);
        v.note = static_cast<int>(std::lround(note));
        emit(clock, Ev::NoteOn, ch, v.note, velocity);
    }
    const int bend = std::clamp(8192 + static_cast<int>(std::lround((note - v.note) * 8192.0 / kBendRange)), 0, 16383);
    if (bend != v.bend) {
        v.bend = bend;
        emit(clock, Ev::Bend, ch, bend);
    }
}

void RolandSound::fmKey(int ch, bool on, const Ym2612& ym, uint64_t clock) {
    Voice& v = voices_[static_cast<size_t>(ch)];
    if (ch == 5 && ym.dacEnabled()) on = false;  // channel 6 is playing samples
    if (on == v.on) return;
    if (!on) {
        v.on = false;
        emit(clock, Ev::NoteOff, ch, v.note);
        return;
    }
    const uint8_t hi = ym.reg(chReg(ch, 0xA4));
    const double note = fmNote((hi & 7) << 8 | ym.reg(chReg(ch, 0xA0)), (hi >> 3) & 7, chipClock_);
    if (note <= 0) return;
    std::array<uint8_t, 28> ops{};
    const uint32_t fp = fmFingerprint(ch, ym, &ops);
    int program;
    if (auto a = assigned_.find(fp); a != assigned_.end()) {
        program = a->second;
    } else {
        auto r = rules_.find(fp);
        if (r == rules_.end()) r = rules_.emplace(fp, programFor(ops, ym.reg(chReg(ch, 0xB0)), static_cast<int>(note))).first;
        program = r->second;
    }
    if (program != v.program) {
        v.program = program;
        emit(clock, Ev::Program, ch, program);
    }
    const uint8_t panBits = ym.reg(chReg(ch, 0xB4)) >> 6;  // bit 1 left, bit 0 right
    const int pan = panBits == 2 ? 0 : panBits == 1 ? 127 : 64;
    if (pan != v.pan) {
        v.pan = pan;
        emit(clock, Ev::Pan, ch, pan);
    }
    v.amplitude = fmAmplitude(ch, ym);
    if (v.expression != 127) {
        v.expression = 127;
        emit(clock, Ev::Expression, ch, 127);
    }
    v.note = static_cast<int>(std::lround(note));
    v.on = true;
    pitchTo(ch, note, clock, 0);
    emit(clock, Ev::NoteOn, ch, v.note, std::clamp(static_cast<int>(127 * std::sqrt(v.amplitude)), 1, 127));
}

void RolandSound::fmPitch(int ch, const Ym2612& ym, uint64_t clock) {
    Voice& v = voices_[static_cast<size_t>(ch)];
    if (!v.on) return;
    const uint8_t hi = ym.reg(chReg(ch, 0xA4));
    pitchTo(ch, fmNote((hi & 7) << 8 | ym.reg(chReg(ch, 0xA0)), (hi >> 3) & 7, chipClock_), clock,
            std::clamp(static_cast<int>(127 * std::sqrt(v.amplitude)), 1, 127));
}

void RolandSound::fmLevel(int ch, const Ym2612& ym, uint64_t clock) {
    Voice& v = voices_[static_cast<size_t>(ch)];
    if (!v.on || v.amplitude <= 0) return;
    const int expression = std::clamp(static_cast<int>(127 * std::sqrt(fmAmplitude(ch, ym) / v.amplitude)), 0, 127);
    if (expression == v.expression) return;
    v.expression = expression;
    emit(clock, Ev::Expression, ch, expression);
}

void RolandSound::fmWrite(int offset, uint8_t value, const Ym2612& ym, uint64_t clock) {
    chipClock_ = ym.sampleRate() * 144.0;
    const int port = (offset >> 1) & 1;
    if (!(offset & 1)) {
        address_[static_cast<size_t>(port)] = value;
        return;
    }
    const int reg = address_[static_cast<size_t>(port)];
    if (port == 0 && reg == 0x28) {
        if ((value & 3) == 3) return;
        fmKey((value & 3) + ((value & 4) ? 3 : 0), (value & 0xF0) != 0, ym, clock);
        return;
    }
    if (port == 0 && reg == 0x2B) {
        if ((value & 0x80) && voices_[5].on) fmKey(5, false, ym, clock);
        return;
    }
    if (reg < 0x30) return;
    const int slot = reg & 3;
    if (slot == 3) return;
    const int ch = port * 3 + slot;
    if (reg >= 0xA0 && reg <= 0xA6) fmPitch(ch, ym, clock);  // A0 commits the latched A4
    else if (reg >= 0x40 && reg < 0x50) fmLevel(ch, ym, clock);
}

void RolandSound::psgWrite(const Psg& psg, uint64_t clock) {
    for (int c = 0; c < 3; ++c) {
        const int ch = kPsgFirst + c;
        Voice& v = voices_[static_cast<size_t>(ch)];
        const int att = psg.attenuation(c);
        const double note = psgNote(psg.period(c), psg.clock());
        const bool sounding = att < 15 && note > 20 && note < 120;
        const double amplitude = std::pow(10.0, -2.0 * att / 20.0);  // 2 dB a step
        if (!sounding) {
            if (v.on) {
                v.on = false;
                emit(clock, Ev::NoteOff, ch, v.note);
            }
            continue;
        }
        if (!v.on) {
            if (v.program != kPsgDefault) {
                v.program = kPsgDefault;
                emit(clock, Ev::Program, ch, kPsgDefault);
            }
            if (v.expression != 127) {
                v.expression = 127;
                emit(clock, Ev::Expression, ch, 127);
            }
            v.amplitude = amplitude;
            v.note = static_cast<int>(std::lround(note));
            v.on = true;
            pitchTo(ch, note, clock, 0);
            emit(clock, Ev::NoteOn, ch, v.note, std::clamp(static_cast<int>(127 * std::sqrt(amplitude)), 1, 127));
            continue;
        }
        pitchTo(ch, note, clock, std::clamp(static_cast<int>(127 * std::sqrt(v.amplitude)), 1, 127));
        // Games shape PSG notes by rewriting the volume: that is the expression.
        const int expression = std::clamp(static_cast<int>(127 * std::sqrt(amplitude / v.amplitude)), 0, 127);
        if (expression != v.expression) {
            v.expression = expression;
            emit(clock, Ev::Expression, ch, expression);
        }
    }
    // The noise channel is the drums: white noise a snare or a hi-hat by its pitch,
    // periodic noise a bass drum. A jump in volume is a new hit.
    Voice& d = voices_[kDrums];
    const int att = psg.attenuation(3);
    if (att >= 15) {
        d.on = false;
        d.amplitude = 0;
        return;
    }
    const double amplitude = std::pow(10.0, -2.0 * att / 20.0);
    if (d.on && amplitude < d.amplitude * 1.9) {
        d.amplitude = std::min(d.amplitude, amplitude);
        return;
    }
    const int control = psg.period(3);
    const int drum = !(control & 4) ? 36 : (control & 3) >= 2 ? 38 : 42;  // bass drum, snare, closed hi-hat
    d.on = true;
    d.amplitude = amplitude;
    d.note = drum;
    emit(clock, Ev::NoteOn, kDrums, drum, std::clamp(static_cast<int>(127 * std::sqrt(amplitude)), 1, 127));
}

void RolandSound::apply(const Event& e) {
    tsf* f = synth_->font;
    const int ch = e.channel;
    switch (e.type) {
        case Ev::NoteOn: tsf_channel_note_on(f, ch, e.value, static_cast<float>(e.extra) / 127.0f); break;
        case Ev::NoteOff: tsf_channel_note_off(f, ch, e.value); break;
        case Ev::Program: tsf_channel_set_presetnumber(f, ch, e.value, ch == kDrums); break;
        case Ev::Bend: tsf_channel_set_pitchwheel(f, ch, e.value); break;
        case Ev::Pan: tsf_channel_set_pan(f, ch, static_cast<float>(e.value) / 127.0f); break;
        case Ev::Expression: tsf_channel_midi_control(f, ch, 11, e.value); break;
    }
}

void RolandSound::render(int16_t* out, int frames) {
    if (frames <= 0) return;
    std::vector<float>& buf = synth_->buffer;
    buf.assign(static_cast<size_t>(frames) * 2, 0.0f);
    tsf_render_float(synth_->font, buf.data(), frames, 0);
    synth_->effects.process(buf.data(), frames);
    const float gain = 9000.0f * static_cast<float>(volume_) / 100.0f;
    for (int i = 0; i < frames * 2; ++i)
        out[i] = static_cast<int16_t>(std::clamp(static_cast<int>(out[i]) + static_cast<int>(buf[static_cast<size_t>(i)] * gain), -32768, 32767));
}

void RolandSound::endFrame(uint64_t clock, std::vector<int16_t>& out, size_t from, double rate) {
    Synth& s = *synth_;
    if (!s.font) {
        events_.clear();
        frameStart_ = clock;
        return;
    }
    if (s.rate == 0) {
        // The output rate moves a little every frame to keep the queue level; the synth
        // keeps the first one, which is near enough.
        s.rate = static_cast<int>(std::lround(rate));
        tsf_set_output(s.font, TSF_STEREO_INTERLEAVED, s.rate, 0.0f);
        tsf_set_max_voices(s.font, 64);
        for (int ch = 0; ch < 16; ++ch) {
            tsf_channel_set_pitchrange(s.font, ch, static_cast<float>(kBendRange));
            tsf_channel_set_presetnumber(s.font, ch, 0, ch == kDrums);
        }
        s.effects.setRate(s.rate);
    }
    const int frames = static_cast<int>((out.size() - std::min(out.size(), from * 2)) / 2);
    const double span = clock > frameStart_ ? static_cast<double>(clock - frameStart_) : 1.0;
    int done = 0;
    for (const Event& e : events_) {
        const double t = e.clock > frameStart_ ? static_cast<double>(e.clock - frameStart_) / span : 0.0;
        const int at = std::clamp(static_cast<int>(t * frames), done, frames);
        render(out.data() + (from + static_cast<size_t>(done)) * 2, at - done);
        done = at;
        apply(e);
    }
    render(out.data() + (from + static_cast<size_t>(done)) * 2, frames - done);
    events_.clear();
    frameStart_ = clock;
}

}  // namespace plumsg
