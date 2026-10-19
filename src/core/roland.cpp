#include "roland.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "i18n.h"
#include "psg.h"
#include "ym2413.h"
#include "ym2612.h"

#define TSF_IMPLEMENTATION
#define TSF_NO_STDIO
#include "tsf.h"

namespace plumsg {

namespace {

constexpr int kBendRange = 12;  // semitones either way, so slides stay one note
constexpr int kPsgDefault = 80;  // Lead 1 (square): what a PSG tone sounds like
// A square lead on a GM module is far louder than the PSG next to the FM, and games
// use the PSG for harmonies and echoes: at full volume it buries the melody.
constexpr int kPsgVolume = 50;

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

// The equalizer: a low shelf, three peaks and a high shelf (RBJ's biquads), one gain
// each in dB. A band at 0 dB is left out.
class Equalizer {
public:
    void setRate(double rate) {
        rate_ = rate;
        design();
    }
    void setGains(const std::array<int, RolandSound::kEqBands>& gains) {
        if (gains == gains_) return;
        gains_ = gains;
        design();
    }

    void process(float* buf, int frames) {
        for (int b = 0; b < RolandSound::kEqBands; ++b) {
            if (!gains_[static_cast<size_t>(b)] || rate_ <= 0) continue;
            Band& f = bands_[static_cast<size_t>(b)];
            for (int i = 0; i < frames; ++i)
                for (int side = 0; side < 2; ++side) {
                    float& x = buf[i * 2 + side];
                    float* z = f.z[side];
                    const float y = f.b0 * x + z[0];
                    z[0] = f.b1 * x - f.a1 * y + z[1];
                    z[1] = f.b2 * x - f.a2 * y;
                    x = y;
                }
        }
    }

private:
    struct Band {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z[2][2] = {};
    };
    void design() {
        if (rate_ <= 0) return;
        for (int b = 0; b < RolandSound::kEqBands; ++b) {
            Band& f = bands_[static_cast<size_t>(b)];
            const double gain = std::pow(10.0, gains_[static_cast<size_t>(b)] / 40.0);
            const double w = 2 * 3.14159265358979 * std::min(RolandSound::kEqFrequencies[b], rate_ * 0.45) / rate_;
            const double cw = std::cos(w), sw = std::sin(w);
            double b0, b1, b2, a0, a1, a2;
            if (b == 0 || b == RolandSound::kEqBands - 1) {  // shelves, slope 1
                const double alpha = sw / 2 * std::sqrt(2.0), sq = 2 * std::sqrt(gain) * alpha;
                const double sign = b == 0 ? 1 : -1;  // the high shelf is the low one with cos negated
                b0 = gain * ((gain + 1) - sign * (gain - 1) * cw + sq);
                b1 = 2 * sign * gain * ((gain - 1) - sign * (gain + 1) * cw);
                b2 = gain * ((gain + 1) - sign * (gain - 1) * cw - sq);
                a0 = (gain + 1) + sign * (gain - 1) * cw + sq;
                a1 = -2 * sign * ((gain - 1) + sign * (gain + 1) * cw);
                a2 = (gain + 1) + sign * (gain - 1) * cw - sq;
            } else {  // peaks, Q 1: about an octave and a half wide
                const double alpha = sw / 2;
                b0 = 1 + alpha * gain;
                b1 = -2 * cw;
                b2 = 1 - alpha * gain;
                a0 = 1 + alpha / gain;
                a1 = -2 * cw;
                a2 = 1 - alpha / gain;
            }
            f.b0 = static_cast<float>(b0 / a0);
            f.b1 = static_cast<float>(b1 / a0);
            f.b2 = static_cast<float>(b2 / a0);
            f.a1 = static_cast<float>(a1 / a0);
            f.a2 = static_cast<float>(a2 / a0);
        }
    }

    double rate_ = 0;
    std::array<int, RolandSound::kEqBands> gains_{};
    std::array<Band, RolandSound::kEqBands> bands_{};
};

}  // namespace

struct RolandSound::Synth {
    tsf* font = nullptr;
    int rate = 0;  // the rate tsf renders at, fixed once set
    Effects effects;
    Equalizer equalizer;
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

void RolandSound::setEqualizer(const std::array<int, kEqBands>& gains) {
    std::array<int, kEqBands> clamped = gains;
    for (int& g : clamped) g = std::clamp(g, -kEqRange, kEqRange);
    synth_->equalizer.setGains(clamped);
}

namespace {

// What the PSG noise sounds like as a drum: white noise a snare or a hi-hat by its
// pitch, periodic noise a bass drum.
int noiseDrum(int control) { return !(control & 4) ? 36 : (control & 3) >= 2 ? 38 : 42; }

const char* const kPrograms[128] = {
    N_("Piano de cola"), N_("Piano brillante"), N_("Piano eléctrico de cola"), N_("Piano de cantina"), N_("Piano eléctrico 1"),
    N_("Piano eléctrico 2"), N_("Clavecín"), N_("Clavinet"), N_("Celesta"), N_("Glockenspiel"), N_("Caja de música"), N_("Vibráfono"),
    N_("Marimba"), N_("Xilófono"), N_("Campanas tubulares"), N_("Dulcémele"), N_("Órgano de barras"), N_("Órgano de percusión"),
    N_("Órgano de rock"), N_("Órgano de iglesia"), N_("Armonio"), N_("Acordeón"), N_("Armónica"), N_("Bandoneón"), N_("Guitarra de nailon"),
    N_("Guitarra de acero"), N_("Guitarra eléctrica de jazz"), N_("Guitarra eléctrica limpia"), N_("Guitarra eléctrica apagada"),
    N_("Guitarra saturada"), N_("Guitarra distorsionada"), N_("Armónicos de guitarra"), N_("Bajo acústico"), N_("Bajo eléctrico (dedo)"),
    N_("Bajo eléctrico (púa)"), N_("Bajo sin trastes"), N_("Bajo slap 1"), N_("Bajo slap 2"), N_("Bajo sintetizado 1"),
    N_("Bajo sintetizado 2"), N_("Violín"), N_("Viola"), N_("Violonchelo"), N_("Contrabajo"), N_("Cuerdas en trémolo"),
    N_("Cuerdas en pizzicato"), N_("Arpa"), N_("Timbales"), N_("Cuerdas 1"), N_("Cuerdas 2"), N_("Cuerdas sintetizadas 1"),
    N_("Cuerdas sintetizadas 2"), N_("Coro \"aah\""), N_("Voz \"ooh\""), N_("Voz sintetizada"), N_("Golpe de orquesta"), N_("Trompeta"),
    N_("Trombón"), N_("Tuba"), N_("Trompeta con sordina"), N_("Trompa"), N_("Sección de metales"), N_("Metales sintetizados 1"),
    N_("Metales sintetizados 2"), N_("Saxo soprano"), N_("Saxo alto"), N_("Saxo tenor"), N_("Saxo barítono"), N_("Oboe"), N_("Corno inglés"),
    N_("Fagot"), N_("Clarinete"), N_("Flautín"), N_("Flauta"), N_("Flauta dulce"), N_("Flauta de pan"), N_("Botella soplada"), N_("Shakuhachi"),
    N_("Silbido"), N_("Ocarina"), N_("Solista cuadrada"), N_("Solista diente de sierra"), N_("Solista calíope"), N_("Solista chiff"),
    N_("Solista charango"), N_("Solista voz"), N_("Solista quintas"), N_("Solista bajo y melodía"), N_("Fondo new age"), N_("Fondo cálido"),
    N_("Fondo polisintetizador"), N_("Fondo coro"), N_("Fondo arco"), N_("Fondo metálico"), N_("Fondo halo"), N_("Fondo barrido"),
    N_("Efecto lluvia"), N_("Efecto banda sonora"), N_("Efecto cristal"), N_("Efecto atmósfera"), N_("Efecto brillo"),
    N_("Efecto duendes"), N_("Efecto ecos"), N_("Efecto ciencia ficción"), N_("Sitar"), N_("Banjo"), N_("Shamisen"), N_("Koto"), N_("Kalimba"),
    N_("Gaita"), N_("Violín celta"), N_("Shanai"), N_("Campanilla"), N_("Agogó"), N_("Tambores metálicos"), N_("Caja china"), N_("Taiko"),
    N_("Tom melódico"), N_("Tambor sintetizado"), N_("Platillo al revés"), N_("Traste de guitarra"), N_("Respiración"), N_("Olas"),
    N_("Pájaros"), N_("Teléfono"), N_("Helicóptero"), N_("Aplausos"), N_("Disparo")};

const char* const kDrumNames[47] = {
    N_("Bombo acústico"), N_("Bombo"), N_("Golpe de aro"), N_("Caja"), N_("Palmada"), N_("Caja eléctrica"), N_("Tom grave de suelo"),
    N_("Charles cerrado"), N_("Tom agudo de suelo"), N_("Charles con pedal"), N_("Tom grave"), N_("Charles abierto"),
    N_("Tom medio grave"), N_("Tom medio agudo"), N_("Platillo crash 1"), N_("Tom agudo"), N_("Platillo ride 1"), N_("Platillo chino"),
    N_("Campana del ride"), N_("Pandereta"), N_("Platillo splash"), N_("Cencerro"), N_("Platillo crash 2"), N_("Vibraslap"),
    N_("Platillo ride 2"), N_("Bongó agudo"), N_("Bongó grave"), N_("Conga aguda apagada"), N_("Conga aguda abierta"), N_("Conga grave"),
    N_("Timbal agudo"), N_("Timbal grave"), N_("Agogó agudo"), N_("Agogó grave"), N_("Cabasa"), N_("Maracas"), N_("Silbato corto"),
    N_("Silbato largo"), N_("Güiro corto"), N_("Güiro largo"), N_("Claves"), N_("Caja china aguda"), N_("Caja china grave"),
    N_("Cuíca apagada"), N_("Cuíca abierta"), N_("Triángulo apagado"), N_("Triángulo abierto")};

// The YM2413's built-in instruments, and the General MIDI instrument each one is.
const char* const kOpllNames[15] = {N_("Violín"),  N_("Guitarra"), N_("Piano"),        N_("Flauta"),      N_("Clarinete"),
                                    N_("Oboe"),    N_("Trompeta"), N_("Órgano"),       N_("Trompa"),      N_("Sintetizador"),
                                    N_("Clavecín"), N_("Vibráfono"), N_("Bajo sintetizado"), N_("Bajo acústico"),
                                    N_("Guitarra eléctrica")};
constexpr int kOpllPrograms[15] = {40, 24, 0, 73, 71, 68, 56, 16, 60, 81, 6, 11, 38, 32, 27};
// Its rhythm sounds (bits 4 to 0 of register 0x0E), the drum each one is, and the
// channel that plays it.
const char* const kRhythmNames[5] = {N_("Bombo"), N_("Caja"), N_("Tom"), N_("Platillo"), N_("Charles")};
constexpr int kRhythmDrums[5] = {36, 38, 45, 49, 42};
constexpr int kRhythmChannel[5] = {6, 7, 8, 8, 7};

// The voice of a YM2413 channel (0-8).
int opllVoice(int ch) { return ch < 6 ? ch : RolandSound::kOpllHigh + ch - 6; }

}  // namespace

int RolandSound::opllInstrument(uint32_t fingerprint) {
    return (fingerprint & ~0xFFu) == opllFingerprint(0) && (fingerprint & 0xFF) >= 1 && (fingerprint & 0xFF) < 16
               ? static_cast<int>(fingerprint & 0xFF)
               : 0;
}

const char* RolandSound::opllInstrumentName(int instrument) {
    return instrument >= 1 && instrument <= 15 ? tr(kOpllNames[instrument - 1]) : "";
}

const char* RolandSound::rhythmName(int drum) { return drum >= 0 && drum < 5 ? tr(kRhythmNames[drum]) : ""; }

uint32_t RolandSound::opllChannels(uint32_t voices) {
    uint32_t channels = 0;
    for (int ch = 0; ch < 9; ++ch) {
        bool all = (voices >> opllVoice(ch)) & 1;
        for (int d = 0; d < 5; ++d)
            if (kRhythmChannel[d] == ch) all = all && ((voices >> (kRhythmFirst + d)) & 1);
        if (all) channels |= 1u << ch;
    }
    return channels;
}

const char* RolandSound::programName(int program) { return program >= 0 && program < 128 ? tr(kPrograms[program]) : ""; }

const char* RolandSound::drumName(int note) { return note >= 35 && note <= 81 ? tr(kDrumNames[note - 35]) : nullptr; }

RolandSound::Kind RolandSound::kindOf(uint32_t fingerprint) {
    if ((fingerprint & ~0xFFu) == psgToneFingerprint(0) && (fingerprint & 0xFF) < 3) return Kind::PsgTone;
    if ((fingerprint & ~0xFFu) == noiseFingerprint(0) && (fingerprint & 0xFF) < 8) return Kind::PsgNoise;
    if ((fingerprint & ~0xFFu) == rhythmFingerprint(0) && (fingerprint & 0xFF) < 5) return Kind::Rhythm;
    return Kind::Fm;
}

std::vector<uint32_t> RolandSound::aliases(uint32_t fingerprint) const {
    auto l = legacy_.find(fingerprint);
    return l == legacy_.end() ? std::vector<uint32_t>{} : std::vector<uint32_t>(l->second.begin(), l->second.end());
}

uint32_t RolandSound::assignedAs(uint32_t fingerprint) const {
    if (assigned_.count(fingerprint)) return fingerprint;
    for (uint32_t old : aliases(fingerprint))
        if (assigned_.count(old)) return old;
    return 0;
}

bool RolandSound::isSoundEffect(uint32_t fingerprint) const {
    if (effects_.count(fingerprint)) return true;
    for (uint32_t old : aliases(fingerprint))
        if (effects_.count(old)) return true;
    return false;
}

RolandSound::Assignment RolandSound::assignmentFor(uint32_t fingerprint) const {
    if (const uint32_t key = assignedAs(fingerprint)) return assigned_.at(key);
    Assignment rules;
    if (effectsOriginal_ && isSoundEffect(fingerprint)) {
        rules.original = true;
        return rules;
    }
    switch (kindOf(fingerprint)) {
        case Kind::PsgTone:
            rules.program = kPsgDefault;
            rules.volume = kPsgVolume;
            break;
        case Kind::PsgNoise: rules.drum = noiseDrum(static_cast<int>(fingerprint & 7)); break;
        case Kind::Rhythm: rules.drum = kRhythmDrums[fingerprint & 0xFF]; break;
        case Kind::Fm: {
            if (const int i = opllInstrument(fingerprint)) {
                rules.program = kOpllPrograms[i - 1];
                break;
            }
            auto r = rules_.find(fingerprint);
            rules.program = r != rules_.end() ? r->second : 81;
            break;
        }
    }
    return rules;
}

void RolandSound::setSoundEffects(std::set<uint32_t> effects, bool original) {
    effects_ = std::move(effects);
    effectsOriginal_ = original;
    updateChipChannels();
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

double RolandSound::opllNote(int fnum, int block, double chipClock) {
    if (fnum <= 0) return 0;
    const double hz = fnum * std::ldexp(1.0, block) * (chipClock / 72.0) / 524288.0;
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
    if (carriers >= 2) return 62;               // Synth Brass 1: Brass Section starts too softly to lead
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
    // The noise and the YM2413's drums only play drums.
    v.midi = !quiet && (v.drum || (a.program >= 0 && voice != kNoise && voice < kRhythmFirst));
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

uint32_t RolandSound::fmFingerprint(int ch, const Ym2612& ym, std::array<uint8_t, 28>* ops, uint32_t* legacy) const {
    const uint8_t algorithmFeedback = ym.reg(chReg(ch, 0xB0));
    const int algorithm = algorithmFeedback & 7;
    // FNV-1a. Before 0.0.23 every bit written went in, release rate included: drivers
    // like Sonic's set it to the fastest to cut a note or make way for a sound effect,
    // and the same instrument came out as another one, with no assignment. Those old
    // fingerprints are still worked out, for the files that use them.
    uint32_t hash = 2166136261u, old = 2166136261u;
    auto add = [](uint32_t& h, uint8_t b) { h = (h ^ b) * 16777619u; };
    add(hash, algorithmFeedback & 0x3F);
    add(old, algorithmFeedback & 0x3F);
    // The bits the chip uses of each register, 0x30 to 0x90.
    static constexpr uint8_t kUsed[7] = {0x7F, 0x7F, 0xDF, 0x9F, 0x1F, 0xF0, 0x0F};
    for (int op = 0; op < 4; ++op) {
        for (int i = 0; i < 7; ++i) {
            uint8_t b = ym.reg(opReg(ch, op, 0x30 + i * 0x10));
            if (ops) (*ops)[static_cast<size_t>(op * 7 + i)] = b;
            // A carrier's total level is its volume, not its sound.
            if (i == 1 && (kCarriers[algorithm] & (1 << op))) b = 0;
            add(old, b);
            add(hash, b & kUsed[i]);  // and the release rate (0x80's low half) is how a note is let go
        }
    }
    // Clear of the PSG's fingerprints, which are not hashes.
    if (kindOf(hash) != Kind::Fm || hash == 0) hash ^= 0x80000001u;
    if (kindOf(old) != Kind::Fm || old == 0) old ^= 0x80000001u;
    if (legacy) *legacy = old;
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
    uint32_t legacy = 0;
    const uint32_t fp = fmFingerprint(ch, ym, &ops, &legacy);
    if (legacy != fp) legacy_[fp].insert(legacy);
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
            rules.volume = kPsgVolume;
            see(voice, fp, rules, static_cast<int>(std::lround(note)), nullptr, 0);
            // A Game Gear can send each channel left, right or both.
            const bool left = (psg.stereo() >> (4 + c)) & 1, right = (psg.stereo() >> c) & 1;
            noteOn(voice, fp, assignmentFor(fp), note, amplitude, left == right ? 64 : left ? 0 : 127, clock);
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

double RolandSound::opllNoteOf(int ch, const Ym2413& opll) const {
    const uint8_t hi = opll.reg(0x20 + ch);
    return opllNote((hi & 1) << 8 | opll.reg(0x10 + ch), (hi >> 1) & 7, opllClock_);
}

void RolandSound::opllKey(int ch, const Ym2413& opll, uint64_t clock) {
    const int voice = opllVoice(ch);
    Voice& v = voices_[static_cast<size_t>(voice)];
    const bool on = (opll.reg(0x20 + ch) & 0x10) && !(ch >= 6 && (opll.reg(0x0E) & 0x20));
    if (on == v.on) {
        if (on) pitchTo(voice, opllNoteOf(ch, opll), clock);
        return;
    }
    if (!on) {
        noteOff(voice, clock);
        return;
    }
    const double note = opllNoteOf(ch, opll);
    if (note <= 0) return;
    const int instrument = opll.reg(0x30 + ch) >> 4;
    uint32_t fp = opllFingerprint(instrument);
    std::array<uint8_t, 28> ops{};
    if (instrument == 0) {  // the game's own: a hash of its eight registers
        uint32_t hash = 2166136261u;
        for (int r = 0; r < 8; ++r) {
            ops[static_cast<size_t>(r)] = opll.reg(r);
            hash = (hash ^ opll.reg(r)) * 16777619u;
        }
        if (kindOf(hash) != Kind::Fm || opllInstrument(hash) || hash == 0) hash ^= 0x80000001u;
        fp = hash;
        if (!rules_.count(fp)) rules_.emplace(fp, note < 48 ? 38 : 81);
    }
    Assignment rules;
    rules.program = instrument ? kOpllPrograms[instrument - 1] : rules_[fp];
    see(voice, fp, rules, static_cast<int>(std::lround(note)), instrument == 0 ? &ops : nullptr, 0);
    const double amplitude = std::pow(10.0, -3.0 * (opll.reg(0x30 + ch) & 0x0F) / 20.0);  // 3 dB a step
    noteOn(voice, fp, assignmentFor(fp), note, amplitude, 64, clock);
}

void RolandSound::opllRhythm(const Ym2413& opll, uint64_t clock) {
    const uint8_t r = opll.reg(0x0E);
    const uint8_t keys = (r & 0x20) ? (r & 0x1F) : 0;
    // Each drum's volume: 0x36 low (bass drum), 0x37 high (hi-hat) and low (snare), 0x38
    // high (tom) and low (cymbal).
    const int volumes[5] = {opll.reg(0x36) & 15, opll.reg(0x37) & 15, opll.reg(0x38) >> 4, opll.reg(0x38) & 15,
                            opll.reg(0x37) >> 4};
    for (int d = 0; d < 5; ++d) {
        const uint8_t bit = static_cast<uint8_t>(0x10 >> d);
        const int voice = kRhythmFirst + d;
        if (!(keys & bit)) {
            noteOff(voice, clock);
            continue;
        }
        if (rhythmKeys_ & bit) continue;  // still held: one hit
        const uint32_t fp = rhythmFingerprint(d);
        Assignment rules;
        rules.drum = kRhythmDrums[d];
        see(voice, fp, rules, rules.drum, nullptr, 0);
        noteOn(voice, fp, assignmentFor(fp), 0, std::pow(10.0, -3.0 * volumes[d] / 20.0), 64, clock);
    }
    rhythmKeys_ = keys;
}

void RolandSound::opllWrite(int offset, uint8_t value, const Ym2413& opll, uint64_t clock) {
    opllClock_ = opll.sampleRate() * 72.0;
    if (!(offset & 1)) {
        opllAddress_ = value;
        return;
    }
    const int reg = opllAddress_ & 0x3F;
    if (reg == 0x0E) {
        // Rhythm mode takes channels 7-9 from the melody.
        if (value & 0x20)
            for (int ch = 6; ch < 9; ++ch) noteOff(opllVoice(ch), clock);
        opllRhythm(opll, clock);
        return;
    }
    const int ch = reg & 0x0F;
    if (ch > 8) return;
    if (reg >= 0x10 && reg < 0x30) {
        opllKey(ch, opll, clock);
    } else if (reg >= 0x30 && reg < 0x39) {
        const int voice = opllVoice(ch);
        if (voices_[static_cast<size_t>(voice)].on && !(ch >= 6 && (opll.reg(0x0E) & 0x20)))
            expression(voice, std::pow(10.0, -3.0 * (value & 0x0F) / 20.0), clock);
    }
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
    synth_->equalizer.process(buf.data(), frames);
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
        s.equalizer.setRate(s.rate);
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
