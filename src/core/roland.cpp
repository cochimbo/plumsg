#include "roland.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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
    channels_ = {};
    return true;
}

bool RolandSound::shareSoundfont(const RolandSound& other) {
    if (!other.synth_->font) return false;
    tsf* font = tsf_copy(other.synth_->font);
    if (!font) return false;
    if (synth_->font) tsf_close(synth_->font);
    synth_->font = font;
    synth_->rate = 0;
    voices_ = {};
    channels_ = {};
    return true;
}

bool RolandSound::ready() const { return synth_->font != nullptr; }

void RolandSound::setEffects(int reverb, int chorus) { synth_->effects.setAmounts(reverb, chorus); }

namespace {

// What the PSG noise sounds like as a drum: white noise a snare or a hi-hat by its
// pitch, periodic noise a bass drum.
int noiseDrum(int control) { return !(control & 4) ? 36 : (control & 3) >= 2 ? 38 : 42; }

const char* const kPrograms[128] = {
    "Piano de cola", "Piano brillante", "Piano eléctrico de cola", "Piano de cantina", "Piano eléctrico 1",
    "Piano eléctrico 2", "Clavecín", "Clavinet", "Celesta", "Glockenspiel", "Caja de música", "Vibráfono",
    "Marimba", "Xilófono", "Campanas tubulares", "Dulcémele", "Órgano de barras", "Órgano de percusión",
    "Órgano de rock", "Órgano de iglesia", "Armonio", "Acordeón", "Armónica", "Bandoneón", "Guitarra de nailon",
    "Guitarra de acero", "Guitarra eléctrica de jazz", "Guitarra eléctrica limpia", "Guitarra eléctrica apagada",
    "Guitarra saturada", "Guitarra distorsionada", "Armónicos de guitarra", "Bajo acústico", "Bajo eléctrico (dedo)",
    "Bajo eléctrico (púa)", "Bajo sin trastes", "Bajo slap 1", "Bajo slap 2", "Bajo sintetizado 1",
    "Bajo sintetizado 2", "Violín", "Viola", "Violonchelo", "Contrabajo", "Cuerdas en trémolo",
    "Cuerdas en pizzicato", "Arpa", "Timbales", "Cuerdas 1", "Cuerdas 2", "Cuerdas sintetizadas 1",
    "Cuerdas sintetizadas 2", "Coro \"aah\"", "Voz \"ooh\"", "Voz sintetizada", "Golpe de orquesta", "Trompeta",
    "Trombón", "Tuba", "Trompeta con sordina", "Trompa", "Sección de metales", "Metales sintetizados 1",
    "Metales sintetizados 2", "Saxo soprano", "Saxo alto", "Saxo tenor", "Saxo barítono", "Oboe", "Corno inglés",
    "Fagot", "Clarinete", "Flautín", "Flauta", "Flauta dulce", "Flauta de pan", "Botella soplada", "Shakuhachi",
    "Silbido", "Ocarina", "Solista cuadrada", "Solista diente de sierra", "Solista calíope", "Solista chiff",
    "Solista charango", "Solista voz", "Solista quintas", "Solista bajo y melodía", "Fondo new age", "Fondo cálido",
    "Fondo polisintetizador", "Fondo coro", "Fondo arco", "Fondo metálico", "Fondo halo", "Fondo barrido",
    "Efecto lluvia", "Efecto banda sonora", "Efecto cristal", "Efecto atmósfera", "Efecto brillo",
    "Efecto duendes", "Efecto ecos", "Efecto ciencia ficción", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba",
    "Gaita", "Violín celta", "Shanai", "Campanilla", "Agogó", "Tambores metálicos", "Caja china", "Taiko",
    "Tom melódico", "Tambor sintetizado", "Platillo al revés", "Traste de guitarra", "Respiración", "Olas",
    "Pájaros", "Teléfono", "Helicóptero", "Aplausos", "Disparo"};

const char* const kDrumNames[47] = {
    "Bombo acústico", "Bombo", "Golpe de aro", "Caja", "Palmada", "Caja eléctrica", "Tom grave de suelo",
    "Charles cerrado", "Tom agudo de suelo", "Charles con pedal", "Tom grave", "Charles abierto",
    "Tom medio grave", "Tom medio agudo", "Platillo crash 1", "Tom agudo", "Platillo ride 1", "Platillo chino",
    "Campana del ride", "Pandereta", "Platillo splash", "Cencerro", "Platillo crash 2", "Vibraslap",
    "Platillo ride 2", "Bongó agudo", "Bongó grave", "Conga aguda apagada", "Conga aguda abierta", "Conga grave",
    "Timbal agudo", "Timbal grave", "Agogó agudo", "Agogó grave", "Cabasa", "Maracas", "Silbato corto",
    "Silbato largo", "Güiro corto", "Güiro largo", "Claves", "Caja china aguda", "Caja china grave",
    "Cuíca apagada", "Cuíca abierta", "Triángulo apagado", "Triángulo abierto"};

}  // namespace

const char* RolandSound::programName(int program) { return program >= 0 && program < 128 ? kPrograms[program] : ""; }

const char* RolandSound::drumName(int note) { return note >= 35 && note <= 81 ? kDrumNames[note - 35] : nullptr; }

RolandSound::Kind RolandSound::kindOf(uint32_t fingerprint) {
    if ((fingerprint & ~0xFFu) == psgToneFingerprint(0) && (fingerprint & 0xFF) < 3) return Kind::PsgTone;
    if ((fingerprint & ~0xFFu) == noiseFingerprint(0) && (fingerprint & 0xFF) < 8) return Kind::PsgNoise;
    return Kind::Fm;
}

RolandSound::Assignment RolandSound::assignmentFor(uint32_t fingerprint) const {
    if (auto a = assigned_.find(fingerprint); a != assigned_.end()) return a->second;
    Assignment rules;
    switch (kindOf(fingerprint)) {
        case Kind::PsgTone: rules.program = kPsgDefault; break;
        case Kind::PsgNoise: rules.drum = noiseDrum(static_cast<int>(fingerprint & 7)); break;
        case Kind::Fm: {
            auto r = rules_.find(fingerprint);
            rules.program = r != rules_.end() ? r->second : 81;
            break;
        }
    }
    return rules;
}

void RolandSound::setAssignments(Assignments assigned) {
    assigned_ = std::move(assigned);
    updateChipChannels();
}

void RolandSound::setListening(uint32_t mutedVoices, std::set<uint32_t> silenced) {
    mutedVoices_ = mutedVoices;
    silenced_ = std::move(silenced);
    // What is sounding and should not be stops now.
    for (int i = 0; i < kVoices; ++i) {
        Voice& v = voices_[static_cast<size_t>(i)];
        if (v.midi && ((mutedVoices_ >> i) & 1 || silenced_.count(v.fingerprint))) {
            if (!v.drum) emit(frameStart_, Ev::NoteOff, i, v.note);
            v.midi = false;
        }
    }
    updateChipChannels();
}

void RolandSound::updateChipChannels() {
    chipChannels_ = 0;
    for (int i = 0; i < kVoices; ++i) {
        const Voice& v = voices_[static_cast<size_t>(i)];
        if (v.fingerprint && assignmentFor(v.fingerprint).original && !((mutedVoices_ >> i) & 1) &&
            !silenced_.count(v.fingerprint))
            chipChannels_ |= 1u << i;
    }
}

uint32_t RolandSound::activeVoices() const {
    uint32_t mask = 0;
    for (int i = 0; i < kVoices; ++i)
        if (voices_[static_cast<size_t>(i)].on) mask |= 1u << i;
    return mask;
}

RolandSound::Assignments RolandSound::parseAssignments(const std::string& text) {
    Assignments out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (const size_t c = line.find(';'); c != std::string::npos) line.resize(c);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        char* stop = nullptr;
        const std::string key = line.substr(0, eq);
        const unsigned long fp = std::strtoul(key.c_str(), &stop, 16);
        if (stop == key.c_str() || fp == 0) continue;
        char word[32] = "";
        int a = 0, b = 0, c = 0;
        const char* value = line.c_str() + eq + 1;
        Assignment as;
        if (std::sscanf(value, " %31s", word) != 1) continue;
        if (std::string(word) == "original") {
            as.original = true;
        } else if (std::string(word) == "bateria") {
            const int n = std::sscanf(value, " %*s %d %d %d", &a, &b, &c);
            if (n < 1 || !drumName(a)) continue;
            as.drum = a;
            if (n >= 3) as.volume = std::clamp(c, 0, 200);
        } else {
            const int n = std::sscanf(value, " %d %d %d", &a, &b, &c);
            if (n < 1 || a < 0 || a > 127) continue;
            as.program = a;
            if (n >= 2) as.octave = std::clamp(b, -3, 3);
            if (n >= 3) as.volume = std::clamp(c, 0, 200);
        }
        out[static_cast<uint32_t>(fp)] = as;
    }
    return out;
}

std::string RolandSound::formatAssignments(const Assignments& assignments) {
    std::string out =
        "; Instrumentos del sonido Roland: huella = instrumento GM (0-127) octava volumen,\n"
        "; bateria nota 0 volumen (una pieza del kit, 35-81), u original.\n[instrumentos]\n";
    for (const auto& [fp, a] : assignments) {
        char line[160];
        if (a.original)
            std::snprintf(line, sizeof line, "%08x = original\n", fp);
        else if (a.drum >= 0)
            std::snprintf(line, sizeof line, "%08x = bateria %d 0 %d ; %s\n", fp, a.drum, a.volume, drumName(a.drum));
        else
            std::snprintf(line, sizeof line, "%08x = %d %d %d ; %s\n", fp, a.program, a.octave, a.volume,
                          programName(a.program));
        out += line;
    }
    return out;
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

void RolandSound::skipFrame(uint64_t clock) {
    // The channels' settings still go through, so the synth is ready when it is heard.
    if (synth_->font)
        for (const Event& e : events_)
            if (e.type != Ev::NoteOn) apply(e);
    events_.clear();
    frameStart_ = clock;
    if (synth_->font) tsf_note_off_all(synth_->font);
}

void RolandSound::allNotesOff() {
    events_.clear();
    for (Voice& v : voices_) v.on = v.midi = false;
    // Every channel back to its defaults, in the synth and here; the program is sent
    // again with the next note.
    channels_ = {};
    if (synth_->font && synth_->rate) {
        tsf_note_off_all(synth_->font);
        for (int ch = 0; ch < 16; ++ch) {
            tsf_channel_set_pitchwheel(synth_->font, ch, 8192);
            tsf_channel_set_pan(synth_->font, ch, 0.5f);
            tsf_channel_midi_control(synth_->font, ch, 11, 127);
        }
    }
}

void RolandSound::keysOff(uint64_t clock) {
    for (int ch = 0; ch < kFmChannels; ++ch) noteOff(ch, clock);
}

void RolandSound::see(int voice, uint32_t fingerprint, const Assignment& rules, int note,
                      const std::array<uint8_t, 28>* ops, uint8_t algorithmFeedback) {
    Seen& s = seen_[fingerprint];
    s.kind = kindOf(fingerprint);
    if (ops) s.ops = *ops;
    s.algorithmFeedback = algorithmFeedback;
    s.channels |= 1u << voice;
    ++s.notes;
    s.low = std::min(s.low, note);
    s.high = std::max(s.high, note);
    s.rules = rules;
}

int RolandSound::velocity(const Voice& v, double amplitude) {
    return std::clamp(static_cast<int>(std::lround(127 * std::sqrt(amplitude) * v.volume / 100.0)), 1, 127);
}

void RolandSound::noteOn(int voice, uint32_t fingerprint, const Assignment& a, double note, double amplitude, int pan,
                         uint64_t clock) {
    noteOff(voice, clock);
    Voice& v = voices_[static_cast<size_t>(voice)];
    v.on = true;
    v.amplitude = amplitude;
    if (v.fingerprint != fingerprint) {
        v.fingerprint = fingerprint;
        updateChipChannels();
    }
    const bool quiet = a.original || ((mutedVoices_ >> voice) & 1) || silenced_.count(fingerprint);
    v.drum = a.drum >= 0;
    v.midi = !quiet && (v.drum || (a.program >= 0 && voice != kNoise));
    if (!v.midi) return;
    v.volume = a.volume;
    if (v.drum) {
        v.note = a.drum;
        emit(clock, Ev::NoteOn, kDrums, v.note, velocity(v, amplitude));
        return;
    }
    Channel& c = channels_[static_cast<size_t>(voice)];
    if (a.program != c.program) {
        c.program = a.program;
        emit(clock, Ev::Program, voice, a.program);
    }
    if (pan != c.pan) {
        c.pan = pan;
        emit(clock, Ev::Pan, voice, pan);
    }
    if (c.expression != 127) {
        c.expression = 127;
        emit(clock, Ev::Expression, voice, 127);
    }
    v.octave = 12 * a.octave;
    v.note = std::clamp(static_cast<int>(std::lround(note)) + v.octave, 0, 127);
    pitchTo(voice, note, clock);
    emit(clock, Ev::NoteOn, voice, v.note, velocity(v, amplitude));
}

void RolandSound::noteOff(int voice, uint64_t clock) {
    Voice& v = voices_[static_cast<size_t>(voice)];
    // Drums ring out by themselves, as on a GM module.
    if (v.on && v.midi && !v.drum) emit(clock, Ev::NoteOff, voice, v.note);
    v.on = v.midi = false;
}

void RolandSound::expression(int voice, double amplitude, uint64_t clock) {
    Voice& v = voices_[static_cast<size_t>(voice)];
    if (!v.midi || v.drum || v.amplitude <= 0) return;
    Channel& c = channels_[static_cast<size_t>(voice)];
    const int e = std::clamp(static_cast<int>(127 * std::sqrt(amplitude / v.amplitude)), 0, 127);
    if (e == c.expression) return;
    c.expression = e;
    emit(clock, Ev::Expression, voice, e);
}

void RolandSound::pitchTo(int voice, double note, uint64_t clock) {
    Voice& v = voices_[static_cast<size_t>(voice)];
    if (!v.midi || v.drum || note <= 0) return;
    note += v.octave;
    Channel& c = channels_[static_cast<size_t>(voice)];
    // Within the bend range the note bends; further, it is played again at the new note.
    if (std::fabs(note - v.note) > kBendRange) {
        emit(clock, Ev::NoteOff, voice, v.note);
        v.note = std::clamp(static_cast<int>(std::lround(note)), 0, 127);
        emit(clock, Ev::NoteOn, voice, v.note, velocity(v, v.amplitude));
    }
    const int bend = std::clamp(8192 + static_cast<int>(std::lround((note - v.note) * 8192.0 / kBendRange)), 0, 16383);
    if (bend != c.bend) {
        c.bend = bend;
        emit(clock, Ev::Bend, voice, bend);
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
    // Clear of the PSG's fingerprints, which are not hashes.
    if (kindOf(hash) != Kind::Fm || hash == 0) hash ^= 0x80000001u;
    return hash;
}

double RolandSound::fmAmplitude(int ch, const Ym2612& ym) const {
    const int algorithm = ym.reg(chReg(ch, 0xB0)) & 7;
    int level = 127;
    for (int op = 0; op < 4; ++op)
        if (kCarriers[algorithm] & (1 << op)) level = std::min(level, ym.reg(opReg(ch, op, 0x40)) & 0x7F);
    return std::pow(10.0, -0.75 * level / 20.0);  // 0.75 dB a step
}

void RolandSound::fmKey(int ch, bool on, const Ym2612& ym, uint64_t clock) {
    Voice& v = voices_[static_cast<size_t>(ch)];
    if (ch == 5 && ym.dacEnabled()) on = false;  // channel 6 is playing samples
    if (on == v.on) return;
    if (!on) {
        noteOff(ch, clock);
        return;
    }
    const uint8_t hi = ym.reg(chReg(ch, 0xA4));
    const double note = fmNote((hi & 7) << 8 | ym.reg(chReg(ch, 0xA0)), (hi >> 3) & 7, chipClock_);
    if (note <= 0) return;
    std::array<uint8_t, 28> ops{};
    const uint32_t fp = fmFingerprint(ch, ym, &ops);
    const uint8_t algorithmFeedback = ym.reg(chReg(ch, 0xB0));
    auto r = rules_.find(fp);
    if (r == rules_.end()) r = rules_.emplace(fp, programFor(ops, algorithmFeedback, static_cast<int>(note))).first;
    Assignment rules;
    rules.program = r->second;
    see(ch, fp, rules, static_cast<int>(std::lround(note)), &ops, algorithmFeedback);
    const uint8_t panBits = ym.reg(chReg(ch, 0xB4)) >> 6;  // bit 1 left, bit 0 right
    const int pan = panBits == 2 ? 0 : panBits == 1 ? 127 : 64;
    noteOn(ch, fp, assignmentFor(fp), note, fmAmplitude(ch, ym), pan, clock);
}

void RolandSound::fmPitch(int ch, const Ym2612& ym, uint64_t clock) {
    if (!voices_[static_cast<size_t>(ch)].on) return;
    const uint8_t hi = ym.reg(chReg(ch, 0xA4));
    pitchTo(ch, fmNote((hi & 7) << 8 | ym.reg(chReg(ch, 0xA0)), (hi >> 3) & 7, chipClock_), clock);
}

void RolandSound::fmLevel(int ch, const Ym2612& ym, uint64_t clock) { expression(ch, fmAmplitude(ch, ym), clock); }

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
        const int voice = kPsgFirst + c;
        Voice& v = voices_[static_cast<size_t>(voice)];
        const int att = psg.attenuation(c);
        const double note = psgNote(psg.period(c), psg.clock());
        const bool sounding = att < 15 && note > 20 && note < 120;
        const double amplitude = std::pow(10.0, -2.0 * att / 20.0);  // 2 dB a step
        if (!sounding) {
            noteOff(voice, clock);
            continue;
        }
        if (!v.on) {
            const uint32_t fp = psgToneFingerprint(c);
            Assignment rules;
            rules.program = kPsgDefault;
            see(voice, fp, rules, static_cast<int>(std::lround(note)), nullptr, 0);
            noteOn(voice, fp, assignmentFor(fp), note, amplitude, 64, clock);
            continue;
        }
        pitchTo(voice, note, clock);
        // Games shape PSG notes by rewriting the volume: that is the expression.
        expression(voice, amplitude, clock);
    }
    // The noise channel is the drums. A jump in volume is a new hit.
    Voice& d = voices_[kNoise];
    const int att = psg.attenuation(3);
    if (att >= 15) {
        noteOff(kNoise, clock);
        d.amplitude = 0;
        return;
    }
    const double amplitude = std::pow(10.0, -2.0 * att / 20.0);
    if (d.on && amplitude < d.amplitude * 1.9) {
        d.amplitude = std::min(d.amplitude, amplitude);
        return;
    }
    const int control = psg.period(3) & 7;
    const uint32_t fp = noiseFingerprint(control);
    Assignment rules;
    rules.drum = noiseDrum(control);
    see(kNoise, fp, rules, rules.drum, nullptr, 0);
    noteOn(kNoise, fp, assignmentFor(fp), 0, amplitude, 64, clock);
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
        channels_ = {};
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
