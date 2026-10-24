#include "score_view.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "../core/i18n.h"

namespace plumsg {

namespace {

// The same names as the instrument editor's channels.
const char* const kVoiceNames[RolandSound::kVoices] = {
    "FM 1",   "FM 2",   "FM 3",       "FM 4",     "FM 5",         "FM 6",          "PSG 1",
    "PSG 2",  "PSG 3",  N_("Ruido"),  "DAC",      "FM 7",         "FM 8",          "FM 9",
    N_("Bombo"), N_("Caja"), N_("Tom"), N_("Platillo"), N_("Charles")};

std::string noteName(int note) {
    static const char* kNames[12] = {N_("Do"), N_("Do#"), N_("Re"), N_("Re#"), N_("Mi"), N_("Fa"), N_("Fa#"), N_("Sol"), N_("Sol#"), N_("La"), N_("La#"), N_("Si")};
    note = std::clamp(note, 0, 127);
    return std::string(tr(kNames[note % 12])) + std::to_string(note / 12 - 1);
}

std::string minutes(double seconds) {
    const int s = static_cast<int>(std::max(0.0, seconds));
    char text[16];
    std::snprintf(text, sizeof text, "%d:%02d", s / 60, s % 60);
    return text;
}

// Each track's colour, spread around the hue circle so that neighbours differ.
ImU32 trackColor(int index, float alpha) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(std::fmod(0.58f + index * 0.17f, 1.0f), 0.62f, 0.85f, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, alpha));
}

constexpr float kPi = 3.14159265f;
constexpr ImU32 kPaper = IM_COL32(250, 247, 238, 255);
constexpr ImU32 kInk = IM_COL32(40, 36, 32, 255);
constexpr ImU32 kFaint = IM_COL32(40, 36, 32, 45);
constexpr ImU32 kPlayhead = IM_COL32(215, 55, 45, 230);
constexpr float kHeaderWidth = 190;

// A staff: the y of a step (C4 = 28) given the one of its middle line.
struct Staff {
    float middle;     // y of the middle line
    float half;       // pixels per step
    int center;       // the middle line's step: B4 (34) in treble clef, D3 (22) in bass
    float y(int step) const { return middle - static_cast<float>(step - center) * half; }
};

void drawSharp(ImDrawList* d, ImVec2 c, float s, ImU32 color) {
    // Two uprights and two rising bars.
    const float w = s * 0.32f, h = s * 0.75f, t = std::max(1.0f, s * 0.12f);
    d->AddLine(ImVec2(c.x - w * 0.45f, c.y - h), ImVec2(c.x - w * 0.45f, c.y + h), color, t);
    d->AddLine(ImVec2(c.x + w * 0.45f, c.y - h * 1.1f), ImVec2(c.x + w * 0.45f, c.y + h * 0.9f), color, t);
    d->AddLine(ImVec2(c.x - w, c.y - h * 0.2f), ImVec2(c.x + w, c.y - h * 0.45f), color, t * 1.8f);
    d->AddLine(ImVec2(c.x - w, c.y + h * 0.45f), ImVec2(c.x + w, c.y + h * 0.2f), color, t * 1.8f);
}

// A treble clef around x, its curl on G4's line.
void drawTrebleClef(ImDrawList* d, float x, const Staff& st, float s, ImU32 color) {
    const float t = std::max(1.2f, s * 0.14f);
    const float g = st.y(32);
    d->PathClear();
    d->PathArcTo(ImVec2(x + s * 0.1f, g), s * 0.5f, 1.2f * kPi, 2.9f * kPi, 16);
    d->PathStroke(color, 0, t);
    d->PathClear();
    d->PathArcTo(ImVec2(x, g + s * 0.1f), s * 1.0f, 0.1f * kPi, 1.35f * kPi, 20);
    d->PathStroke(color, 0, t);
    const ImVec2 bottom(x - s * 0.15f, st.y(27));
    const ImVec2 top(x + s * 0.2f, st.y(40));
    d->AddBezierCubic(ImVec2(x - s * 0.7f, g - s * 0.6f), ImVec2(x + s * 0.9f, st.y(36)), ImVec2(x + s * 0.8f, top.y), top,
                      color, t);
    d->AddBezierCubic(top, ImVec2(x - s * 0.5f, top.y + s * 0.4f), ImVec2(x + s * 0.2f, st.y(33)), bottom, color, t);
    d->AddCircleFilled(ImVec2(bottom.x - s * 0.3f, bottom.y - s * 0.05f), s * 0.25f, color);
}

// A bass clef: its dot on F3's line, the curve, and the two dots either side of it.
void drawBassClef(ImDrawList* d, float x, const Staff& st, float s, ImU32 color) {
    const float t = std::max(1.2f, s * 0.16f);
    const float f = st.y(24);
    d->AddCircleFilled(ImVec2(x - s * 0.3f, f), s * 0.28f, color);
    d->AddBezierCubic(ImVec2(x - s * 0.45f, f - s * 0.1f), ImVec2(x - s * 0.2f, st.y(27)), ImVec2(x + s * 1.2f, st.y(27)),
                      ImVec2(x + s * 1.0f, f), color, t);
    d->AddBezierCubic(ImVec2(x + s * 1.0f, f), ImVec2(x + s * 0.9f, st.y(22)), ImVec2(x + s * 0.2f, st.y(19)),
                      ImVec2(x - s * 0.5f, st.y(18)), color, t);
    d->AddCircleFilled(ImVec2(x + s * 1.5f, st.y(25)), s * 0.14f, color);
    d->AddCircleFilled(ImVec2(x + s * 1.5f, st.y(23)), s * 0.14f, color);
}

void drawHead(ImDrawList* d, ImVec2 c, float s, ImU32 fill) {
    // A slanted oval.
    d->PathClear();
    for (int i = 0; i < 18; ++i) {
        const float a = static_cast<float>(i) / 18.0f * 2.0f * kPi;
        const float ex = std::cos(a) * s * 0.62f, ey = std::sin(a) * s * 0.42f;
        d->PathLineTo(ImVec2(c.x + ex * 0.94f + ey * 0.34f, c.y + ey * 0.94f - ex * 0.34f));
    }
    d->PathFillConvex(fill);
}

// A hollow head: a half or a whole note.
void drawHollowHead(ImDrawList* d, ImVec2 c, float s, ImU32 color, bool whole) {
    d->PathClear();
    for (int i = 0; i < 20; ++i) {
        const float a = static_cast<float>(i) / 20.0f * 2.0f * kPi;
        const float ex = std::cos(a) * s * (whole ? 0.72f : 0.62f), ey = std::sin(a) * s * 0.42f;
        d->PathLineTo(whole ? ImVec2(c.x + ex, c.y + ey) : ImVec2(c.x + ex * 0.94f + ey * 0.34f, c.y + ey * 0.94f - ex * 0.34f));
    }
    d->PathFillConvex(kPaper);
    d->PathClear();
    for (int i = 0; i < 20; ++i) {
        const float a = static_cast<float>(i) / 20.0f * 2.0f * kPi;
        const float ex = std::cos(a) * s * (whole ? 0.72f : 0.62f), ey = std::sin(a) * s * 0.42f;
        d->PathLineTo(whole ? ImVec2(c.x + ex, c.y + ey) : ImVec2(c.x + ex * 0.94f + ey * 0.34f, c.y + ey * 0.94f - ex * 0.34f));
    }
    d->PathStroke(color, ImDrawFlags_Closed, std::max(1.5f, s * 0.2f));
}

void drawNatural(ImDrawList* d, ImVec2 c, float s, ImU32 color) {
    const float w = s * 0.25f, h = s * 0.8f, t = std::max(1.0f, s * 0.12f);
    d->AddLine(ImVec2(c.x - w, c.y - h), ImVec2(c.x - w, c.y + h * 0.45f), color, t);
    d->AddLine(ImVec2(c.x + w, c.y - h * 0.45f), ImVec2(c.x + w, c.y + h), color, t);
    d->AddLine(ImVec2(c.x - w, c.y - h * 0.15f), ImVec2(c.x + w, c.y - h * 0.4f), color, t * 1.8f);
    d->AddLine(ImVec2(c.x - w, c.y + h * 0.45f), ImVec2(c.x + w, c.y + h * 0.2f), color, t * 1.8f);
}

// A flag (or two) hanging from a stem's end, down from an up stem and up from a down one.
void drawFlags(ImDrawList* d, ImVec2 tip, bool up, int count, float s, ImU32 color) {
    const float dir = up ? 1.0f : -1.0f;
    for (int i = 0; i < count; ++i) {
        const ImVec2 a(tip.x, tip.y + dir * s * 0.8f * static_cast<float>(i));
        d->AddBezierCubic(a, ImVec2(a.x + s * 0.2f, a.y + dir * s * 0.8f), ImVec2(a.x + s * 1.1f, a.y + dir * s * 1.0f),
                          ImVec2(a.x + s * 0.75f, a.y + dir * s * 2.2f), color, std::max(1.2f, s * 0.18f));
    }
}

// A rest of `length` sixteenths, about the staff's middle line (y of the middle line).
void drawRest(ImDrawList* d, float x, const Staff& st, int length, float s, ImU32 color) {
    const float t = std::max(1.2f, s * 0.16f);
    switch (length) {
        case 16:  // hanging from the fourth line
            d->AddRectFilled(ImVec2(x - s * 0.6f, st.y(st.center + 2)), ImVec2(x + s * 0.6f, st.y(st.center + 2) + s * 0.5f), color);
            break;
        case 8:  // sitting on the middle line
            d->AddRectFilled(ImVec2(x - s * 0.6f, st.y(st.center) - s * 0.5f), ImVec2(x + s * 0.6f, st.y(st.center)), color);
            break;
        case 4: {
            // A zigzag and a hook.
            const float y0 = st.y(st.center + 3), y1 = st.y(st.center - 3);
            const float h = (y1 - y0);
            d->AddLine(ImVec2(x - s * 0.25f, y0), ImVec2(x + s * 0.3f, y0 + h * 0.3f), color, t);
            d->AddLine(ImVec2(x + s * 0.3f, y0 + h * 0.3f), ImVec2(x - s * 0.2f, y0 + h * 0.55f), color, t * 1.8f);
            d->AddLine(ImVec2(x - s * 0.2f, y0 + h * 0.55f), ImVec2(x + s * 0.3f, y0 + h * 0.8f), color, t);
            d->AddBezierCubic(ImVec2(x + s * 0.3f, y0 + h * 0.8f), ImVec2(x - s * 0.5f, y0 + h * 0.65f),
                              ImVec2(x - s * 0.5f, y1), ImVec2(x + s * 0.05f, y1), color, t);
            break;
        }
        default: {
            // An eighth's: a dot and a stroke; a sixteenth's: two.
            const int dots = length == 1 ? 2 : 1;
            const float top = st.y(st.center + 1);
            d->AddLine(ImVec2(x + s * 0.45f, top), ImVec2(x - s * 0.15f, top + s * (1.6f + 0.9f * static_cast<float>(dots - 1))), color, t);
            for (int i = 0; i < dots; ++i) {
                const float y = top + s * 0.9f * static_cast<float>(i);
                d->AddCircleFilled(ImVec2(x - s * 0.15f, y + s * 0.1f), s * 0.22f, color);
                d->AddLine(ImVec2(x - s * 0.15f, y + s * 0.25f), ImVec2(x + s * 0.45f - s * 0.25f * static_cast<float>(i), y), color, t);
            }
            break;
        }
    }
}

enum class Icon { Play, Pause, Stop, SoundOn, SoundOff, Follow };

// A square button with a drawn icon (the fonts may lack the symbols).
bool iconButton(const char* id, Icon icon, bool highlighted = false) {
    const float size = ImGui::GetFrameHeight();
    if (highlighted) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool pressed = ImGui::Button(id, ImVec2(size * 1.25f, size));
    if (highlighted) ImGui::PopStyleColor();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImVec2 c((a.x + b.x) / 2, (a.y + b.y) / 2);
    const float r = size * 0.28f;
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* d = ImGui::GetWindowDrawList();
    switch (icon) {
        case Icon::Play:
            d->AddTriangleFilled(ImVec2(c.x - r * 0.75f, c.y - r), ImVec2(c.x - r * 0.75f, c.y + r), ImVec2(c.x + r, c.y), col);
            break;
        case Icon::Pause:
            d->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.25f, c.y + r), col);
            d->AddRectFilled(ImVec2(c.x + r * 0.25f, c.y - r), ImVec2(c.x + r * 0.8f, c.y + r), col);
            break;
        case Icon::Stop: d->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r * 0.8f), ImVec2(c.x + r * 0.8f, c.y + r * 0.8f), col); break;
        case Icon::SoundOn:
        case Icon::SoundOff: {
            // A speaker, with its waves or crossed out.
            const float x = c.x - r * 0.5f;
            d->AddRectFilled(ImVec2(x - r * 0.6f, c.y - r * 0.35f), ImVec2(x, c.y + r * 0.35f), col);
            d->AddTriangleFilled(ImVec2(x - r * 0.2f, c.y), ImVec2(x + r * 0.5f, c.y - r), ImVec2(x + r * 0.5f, c.y + r), col);
            if (icon == Icon::SoundOn) {
                d->PathArcTo(ImVec2(x + r * 0.5f, c.y), r * 0.6f, -0.3f * kPi, 0.3f * kPi, 8);
                d->PathStroke(col, 0, 1.5f);
                d->PathArcTo(ImVec2(x + r * 0.5f, c.y), r * 1.05f, -0.3f * kPi, 0.3f * kPi, 10);
                d->PathStroke(col, 0, 1.5f);
            } else {
                const ImU32 red = IM_COL32(230, 80, 70, 255);
                d->AddLine(ImVec2(x + r * 0.8f, c.y - r * 0.5f), ImVec2(x + r * 1.7f, c.y + r * 0.5f), red, 2.0f);
                d->AddLine(ImVec2(x + r * 0.8f, c.y + r * 0.5f), ImVec2(x + r * 1.7f, c.y - r * 0.5f), red, 2.0f);
            }
            break;
        }
        case Icon::Follow:
            // The playhead with an arrow running along it.
            d->AddLine(ImVec2(c.x, c.y - r * 1.1f), ImVec2(c.x, c.y + r * 1.1f), col, 2.0f);
            d->AddTriangleFilled(ImVec2(c.x - r * 1.1f, c.y - r * 0.5f), ImVec2(c.x - r * 1.1f, c.y + r * 0.5f),
                                 ImVec2(c.x - r * 0.3f, c.y), col);
            d->AddTriangleFilled(ImVec2(c.x + r * 0.3f, c.y - r * 0.5f), ImVec2(c.x + r * 0.3f, c.y + r * 0.5f),
                                 ImVec2(c.x + r * 1.1f, c.y), col);
            break;
    }
    return pressed;
}

}  // namespace

int ScoreWindow::staffStep(int note, bool* sharp) {
    // The letter each semitone is written on (C D E F G A B = 0-6), raised by a sharp
    // on the black keys.
    static const int kLetter[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    static const bool kSharp[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
    note = std::clamp(note, 0, 127);
    if (sharp) *sharp = kSharp[note % 12];
    return (note / 12 - 1) * 7 + kLetter[note % 12];
}

void ScoreWindow::updateRows(const ScoreMusic& music) {
    const RolandSound::Score& notes = *music.notes;
    const uint64_t last = notes.empty() ? 0 : notes.back().start;
    if (rowsFor_ == music.notes && rowsNotes_ == notes.size() && rowsLast_ == last) return;
    rowsFor_ = music.notes;
    rowsNotes_ = notes.size();
    rowsLast_ = last;
    // An instrument per track, in the order the channels go, and each kept in the colour
    // it had.
    struct Heard {
        uint32_t voices = 0;
        std::array<int, 128> count{};
        int total = 0;
    };
    std::map<uint32_t, Heard> heard;
    for (const RolandSound::ScoreNote& n : notes) {
        Heard& h = heard[n.fingerprint];
        h.voices |= 1u << n.voice;
        ++h.count[static_cast<size_t>(n.note)];
        ++h.total;
    }
    std::map<uint32_t, int> colors;
    for (const Row& r : rows_) colors[r.fingerprint] = r.index;
    int next = 0;
    for (const auto& [fp, index] : colors) next = std::max(next, index + 1);
    rows_.clear();
    std::map<uint32_t, size_t> rowOf;
    for (const auto& [fp, h] : heard) {
        Row r;
        r.fingerprint = fp;
        r.voices = h.voices;
        // The clef where most of its notes are: treble from A3 up.
        int seen = 0, median = 60;
        for (int k = 0; k < 128; ++k) {
            seen += h.count[static_cast<size_t>(k)];
            if (seen * 2 >= h.total) {
                median = k;
                break;
            }
        }
        r.treble = median >= 57;
        auto c = colors.find(fp);
        r.index = c != colors.end() ? c->second : next++;
        rowOf[fp] = rows_.size();
        rows_.push_back(r);
    }
    const double rate = music.clockRate;
    for (size_t i = 0; i < notes.size(); ++i) {
        const RolandSound::ScoreNote& n = notes[i];
        notation::Played p;
        p.start = static_cast<double>(n.start) / rate;
        p.end = n.end ? static_cast<double>(n.end) / rate : -1;
        p.note = n.note;
        p.id = static_cast<int>(i);
        rows_[rowOf[n.fingerprint]].played.push_back(p);
    }
    std::sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) {
        const uint32_t la = a.voices & (~a.voices + 1), lb = b.voices & (~b.voices + 1);  // lowest channel
        return la != lb ? la < lb : a.index < b.index;
    });
}

void ScoreWindow::drawToolbar(const ScoreMusic& music, ScoreRequests& out) {
    if (iconButton("##tocar", music.playing ? Icon::Pause : Icon::Play)) out.togglePlay = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", music.playing ? tr("Pausa") : tr("Tocar"));
    if (music.track) {
        ImGui::SameLine();
        if (iconButton("##parar", Icon::Stop)) {
            out.stop = true;
            follow_ = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Parar y volver al principio"));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(!music.canSpeed);
    if (!music.canSpeed) speed_ = 1.0;
    static const double kSpeeds[] = {0.5, 1.0, 2.0};
    static const char* kSpeedNames[] = {"x0.5", "x1", "x2"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, 2);
        const bool on = speed_ == kSpeeds[i];
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(kSpeedNames[i])) speed_ = kSpeeds[i];
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("La velocidad de la música. Las notas del sintetizador no cambian de tono;\n"
                                      "el sonido original sí, como en cámara lenta."));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    const double at = dragSeek_ >= 0 ? dragSeek_ : music.now;
    if (music.track) ImGui::Text("%s / %s", minutes(at).c_str(), minutes(music.length).c_str());
    else ImGui::TextUnformatted(music.playing ? tr("En directo") : tr("En pausa"));
    ImGui::SameLine();
    if (iconButton("##seguir", Icon::Follow, follow_)) follow_ = !follow_;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Seguir la música. Arrastra los pentagramas para moverte por ellos; la rueda con\n"
                                   "Mayús los mueve también, y con Ctrl acerca o aleja."));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("##zoom", &seconds_, 1.5f, 30.0f, tr("%.0f s a la vista"), ImGuiSliderFlags_AlwaysClamp);
    // The tempo: guessed from the music, or by hand.
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Tempo automático"), &tempoAuto_) && !tempoAuto_) bpm_ = guessed_.bpm();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("El tempo y dónde empiezan los compases (4/4) se adivinan por cuándo empiezan las notas.\n"
                                   "Si las figuras salen el doble o la mitad de largas, quítalo y pon el tempo a mano."));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    int bpm = tempoAuto_ ? meter_.bpm() : bpm_;
    ImGui::BeginDisabled(tempoAuto_);
    if (ImGui::DragInt("##bpm", &bpm, 0.5f, 30, 300, "%d BPM", ImGuiSliderFlags_AlwaysClamp)) bpm_ = bpm;
    ImGui::EndDisabled();
}

void ScoreWindow::updateMeter(const ScoreMusic& music) {
    // A track's tempo is guessed once, from all of it; a game's every few seconds, from
    // what it has played lately, and kept while it stays near.
    const bool again = music.track ? (meterFor_ != music.notes || meterNotes_ != music.notes->size())
                                   : (meterAt_ < 0 || std::fabs(music.now - meterAt_) > 4.0 || meterFor_ != music.notes);
    if (again && !rows_.empty()) {
        const bool first = meterAt_ < 0 || meterFor_ != music.notes;
        meterFor_ = music.notes;
        meterNotes_ = music.notes->size();
        meterAt_ = music.now;
        std::vector<double> onsets, weights;
        for (const Row& r : rows_)
            for (const notation::Played& p : r.played) {
                if (!music.track && p.start < music.now - 30) continue;
                onsets.push_back(p.start);
                const double held = (p.end < 0 ? music.now : p.end) - p.start;
                weights.push_back(std::clamp(held, 0.05, 1.0));
            }
        const notation::Meter g = notation::guessMeter(onsets, weights);
        if (music.track || first || std::fabs(g.quarter - guessed_.quarter) > guessed_.quarter * 0.04) guessed_ = g;
    }
    meter_ = guessed_;
    if (!tempoAuto_) meter_.quarter = 60.0 / std::max(1, bpm_);
}

void ScoreWindow::draw(const ScoreMusic* music, ScoreRequests& out) {
    const ImGuiViewport* screen = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(screen->Pos.x + 20, screen->Pos.y + screen->Size.y * 0.45f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::min(900.0f, screen->Size.x - 40), screen->Size.y * 0.5f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Partitura"), &open_)) {
        ImGui::End();
        return;
    }
    if (!music || !music->notes) {
        ImGui::TextWrapped("%s", tr("Aquí se escriben las notas de la música mientras suena: abre un juego, o una pista en el "
                                    "Editor de instrumentos. La música de los discos de Mega CD son pistas de audio y no tiene "
                                    "notas que escribir."));
        ImGui::End();
        return;
    }
    updateRows(*music);
    updateMeter(*music);
    drawToolbar(*music, out);

    const ImGuiIO& io = ImGui::GetIO();
    const float scrollbar = ImGui::GetStyle().ScrollbarSize;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float left = origin.x + kHeaderWidth, right = origin.x + width - scrollbar - 4;
    if (right - left < 60) {
        ImGui::End();
        return;
    }
    const double perPixel = seconds_ / static_cast<double>(right - left);
    // Where the view is: following, the playhead stays a quarter of the way in on a track
    // (what comes is in sight) and near the end on a game (nothing comes yet).
    const double now = dragSeek_ >= 0 ? dragSeek_ : music->now;
    if (follow_) {
        viewStart_ = now - seconds_ * (music->track ? 0.25 : 0.85);
        if (music->track) viewStart_ = std::max(-0.5, viewStart_);
    }
    auto xAt = [&](double t) { return left + static_cast<float>((t - viewStart_) / perPixel); };
    auto timeAt = [&](float x) { return viewStart_ + static_cast<double>(x - left) * perPixel; };
    const notation::Meter& meter = meter_;

    // The ruler: the beats, and each bar's number; on a track it seeks.
    const float rulerHeight = ImGui::GetTextLineHeight() + 6;
    ImDrawList* wd = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(ImVec2(left, origin.y));
    ImGui::InvisibleButton("regla", ImVec2(right - left, rulerHeight));
    if (music->track && ImGui::IsItemActive()) dragSeek_ = std::clamp(timeAt(io.MousePos.x), 0.0, std::max(0.0, music->length));
    if (music->track && ImGui::IsItemDeactivated() && dragSeek_ >= 0) {
        out.seek = dragSeek_;
        dragSeek_ = -1;
    }
    if (music->track && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Pulsa o arrastra para ir a otro punto."));
    wd->AddRectFilled(ImVec2(left, origin.y), ImVec2(right, origin.y + rulerHeight), ImGui::GetColorU32(ImGuiCol_FrameBg));
    {
        const long long firstBeat = static_cast<long long>(std::floor((viewStart_ - meter.origin) / meter.quarter));
        const float beatPixels = static_cast<float>(meter.quarter / perPixel);
        for (long long b = firstBeat, n = 0; n < 4000; ++b, ++n) {
            const float x = xAt(meter.origin + static_cast<double>(b) * meter.quarter);
            if (x > right) break;
            if (x < left) continue;
            const bool bar = ((b % 4) + 4) % 4 == 0;
            if (!bar && beatPixels < 6) continue;
            wd->AddLine(ImVec2(x, origin.y + rulerHeight * (bar ? 0.2f : 0.65f)), ImVec2(x, origin.y + rulerHeight),
                        ImGui::GetColorU32(bar ? ImGuiCol_Text : ImGuiCol_TextDisabled));
            const long long number = (b >= 0 ? b / 4 : -((-b + 3) / 4)) + 1;
            if (bar && (beatPixels * 4 > 28 || number % 4 == 1))
                wd->AddText(ImVec2(x + 3, origin.y + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), std::to_string(number).c_str());
        }
    }
    if (music->track && music->length > 0) {
        const float end = xAt(music->length);
        if (end < right) wd->AddRectFilled(ImVec2(std::max(left, end), origin.y), ImVec2(right, origin.y + rulerHeight), kFaint);
    }

    // The tracks, scrolling up and down.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rulerHeight + 2));
    ImGui::BeginChild("pistas", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (rows_.empty())
        ImGui::TextDisabled("%s", tr("Esperando notas... (la batería no se escribe: no tiene notas)"));
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float s = 9.0f;  // the space between two lines
    const float rowHeight = s * 13;
    const RolandSound::Score& notes = *music->notes;
    const RolandSound::ScoreNote* tip = nullptr;
    const float top = ImGui::GetCursorScreenPos().y;
    for (size_t ri = 0; ri < rows_.size(); ++ri) {
        const Row& row = rows_[ri];
        ImGui::PushID(static_cast<int>(row.fingerprint));
        const float y0 = top + static_cast<float>(ri) * rowHeight;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y0));
        const bool off = music->silenced && music->silenced->count(row.fingerprint);
        const ImU32 color = trackColor(row.index, off ? 0.35f : 1.0f);
        const ImU32 ink = off ? IM_COL32(40, 36, 32, 90) : kInk;
        // The header: its colour, channels, what it sounds as and its switch.
        d->AddRectFilled(ImVec2(origin.x, y0 + 1), ImVec2(left - 4, y0 + rowHeight - 1), ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
        d->AddRectFilled(ImVec2(origin.x, y0 + 1), ImVec2(origin.x + 5, y0 + rowHeight - 1), color, 2.0f);
        std::string channels;
        for (int v = 0; v < RolandSound::kVoices; ++v)
            if ((row.voices >> v) & 1) channels += (channels.empty() ? "" : ", ") + std::string(tr(kVoiceNames[v]));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 12, y0 + 6));
        ImGui::PushClipRect(ImVec2(origin.x, y0), ImVec2(left - 8, y0 + rowHeight), true);
        ImGui::TextUnformatted(channels.c_str());
        if (music->sound) {
            const RolandSound::Assignment a = music->sound->assignmentFor(row.fingerprint);
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 12, ImGui::GetCursorScreenPos().y));
            ImGui::TextDisabled("%s", music->sound->followOnly() || a.original || a.program < 0 ? tr("Sonido original")
                                                                                              : RolandSound::programName(a.program));
        }
        ImGui::PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 12, y0 + rowHeight - ImGui::GetFrameHeight() - 6));
        if (iconButton("##sonido", off ? Icon::SoundOff : Icon::SoundOn)) out.toggleSound = row.fingerprint;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", off ? tr("Volver a oír este instrumento") : tr("Silenciar este instrumento"));

        // The staff, which can be dragged to move along the music (on a track, a click
        // goes there).
        ImGui::SetCursorScreenPos(ImVec2(left, y0));
        ImGui::InvisibleButton("pentagrama", ImVec2(right - left, rowHeight));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
            viewStart_ -= static_cast<double>(io.MouseDelta.x) * perPixel;
            follow_ = false;
            panned_ = true;
        }
        if (ImGui::IsItemDeactivated()) {
            if (!panned_ && music->track) out.seek = std::clamp(timeAt(io.MousePos.x), 0.0, music->length);
            panned_ = false;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && io.MouseWheel != 0 && (io.KeyShift || io.KeyCtrl)) {
            if (io.KeyCtrl) {
                const double at = timeAt(io.MousePos.x);
                seconds_ = std::clamp(seconds_ * (io.MouseWheel > 0 ? 0.8f : 1.25f), 1.5f, 30.0f);
                viewStart_ = at - static_cast<double>(io.MousePos.x - left) * seconds_ / static_cast<double>(right - left);
            } else {
                viewStart_ -= io.MouseWheel * seconds_ * 0.1;
            }
            follow_ = false;
        }
        if (hovered && io.MouseWheelH != 0) {
            viewStart_ += io.MouseWheelH * seconds_ * 0.1;
            follow_ = false;
        }
        if (!ImGui::IsItemVisible()) {
            ImGui::PopID();
            continue;
        }

        d->AddRectFilled(ImVec2(left, y0 + 1), ImVec2(right, y0 + rowHeight - 1), kPaper, 3.0f);
        d->PushClipRect(ImVec2(left, y0 + 1), ImVec2(right, y0 + rowHeight - 1), true);
        const Staff st{y0 + rowHeight / 2, s / 2, row.treble ? 34 : 22};
        const int lowLine = st.center - 4, highLine = st.center + 4;
        for (int l = lowLine; l <= highLine; l += 2) d->AddLine(ImVec2(left, st.y(l)), ImVec2(right, st.y(l)), kInk, 1.0f);
        if (row.treble) drawTrebleClef(d, left + s * 1.4f, st, s, kInk);
        else drawBassClef(d, left + s * 1.2f, st, s, kInk);
        // 4/4.
        {
            ImFont* font = ImGui::GetFont();
            const float size = s * 2.3f;
            const float tx = left + s * 3.6f;
            d->AddText(font, size, ImVec2(tx, st.y(highLine) - size * 0.08f), kInk, "4");
            d->AddText(font, size, ImVec2(tx, st.y(st.center) - size * 0.08f), kInk, "4");
        }
        const float notesLeft = left + s * 5.6f;
        d->PushClipRect(ImVec2(notesLeft - s * 2, y0 + 1), ImVec2(right, y0 + rowHeight - 1), true);

        // The figures in view.
        const long long from = meter.at(timeAt(notesLeft)) - 2;
        long long to = meter.at(timeAt(right)) + 2;
        to = std::min(to, meter.at(music->track ? music->length : music->now));
        std::vector<notation::Played> inView;
        const double fromTime = meter.time(from - notation::kBar), toTime = meter.time(to + notation::kBar);
        for (const notation::Played& p : row.played) {
            const double end = p.end < 0 ? music->now : p.end;
            if (end < fromTime || p.start > toTime) continue;
            notation::Played q = p;
            q.end = end;
            inView.push_back(q);
        }
        const std::vector<notation::Figure> figs =
            to > from ? notation::transcribe(inView, meter, from, to) : std::vector<notation::Figure>{};

        // The bar lines.
        for (long long k = (from >= 0 ? from / 16 : -((-from + 15) / 16)) * 16; k <= to + 16; k += 16) {
            const float x = xAt(meter.time(k)) - s * 1.6f;
            if (x >= notesLeft - s && x <= right) d->AddLine(ImVec2(x, st.y(highLine)), ImVec2(x, st.y(lowLine)), kInk, 1.2f);
        }

        // Beams: the eighths and sixteenths of a beat that follow one another.
        std::vector<int> group(figs.size(), -1);
        std::vector<std::vector<size_t>> groups;
        auto flagged = [&](const notation::Figure& f) { return !f.rest && f.length <= 3; };
        auto beatOf = [](long long at) { return at >= 0 ? at / 4 : -((-at + 3) / 4); };
        for (size_t i = 0; i < figs.size(); ++i) {
            if (!flagged(figs[i]) || group[i] >= 0) continue;
            std::vector<size_t> g{i};
            for (size_t j = i + 1; j < figs.size(); ++j) {
                const notation::Figure& a = figs[g.back()];
                if (!flagged(figs[j]) || figs[j].at != a.at + a.length || beatOf(figs[j].at) != beatOf(figs[i].at)) break;
                g.push_back(j);
            }
            if (g.size() < 2) continue;
            for (size_t j : g) group[j] = static_cast<int>(groups.size());
            groups.push_back(g);
        }
        auto stepsOf = [&](const notation::Figure& f, std::vector<int>& steps, std::vector<bool>& sharps) {
            steps.clear();
            sharps.clear();
            for (int n : f.notes) {
                bool sharp = false;
                steps.push_back(std::clamp(staffStep(n, &sharp), lowLine - 8, highLine + 8));
                sharps.push_back(sharp);
            }
        };
        // Each figure's stem: up when its notes lie low, and the beamed ones all the same
        // way, to one line.
        std::vector<bool> up(figs.size(), true);
        std::vector<float> stemTip(figs.size(), 0);
        std::vector<int> steps;
        std::vector<bool> sharps;
        for (size_t i = 0; i < figs.size(); ++i) {
            if (figs[i].rest || group[i] >= 0) continue;
            stepsOf(figs[i], steps, sharps);
            int sum = 0;
            for (int k : steps) sum += k;
            up[i] = sum < st.center * static_cast<int>(steps.size());
            const int hi = *std::max_element(steps.begin(), steps.end()), lo = *std::min_element(steps.begin(), steps.end());
            stemTip[i] = up[i] ? std::min(st.y(hi) - s * 3.5f, st.y(st.center)) : std::max(st.y(lo) + s * 3.5f, st.y(st.center));
        }
        for (const std::vector<size_t>& g : groups) {
            int sum = 0, count = 0;
            for (size_t i : g) {
                stepsOf(figs[i], steps, sharps);
                for (int k : steps) sum += k, ++count;
            }
            const bool u = sum < st.center * count;
            float tipY = u ? 1e9f : -1e9f;
            for (size_t i : g) {
                stepsOf(figs[i], steps, sharps);
                const int hi = *std::max_element(steps.begin(), steps.end()), lo = *std::min_element(steps.begin(), steps.end());
                tipY = u ? std::min({tipY, st.y(hi) - s * 3.2f, st.y(st.center)}) : std::max({tipY, st.y(lo) + s * 3.2f, st.y(st.center)});
            }
            for (size_t i : g) {
                up[i] = u;
                stemTip[i] = tipY;
            }
        }

        // Accidentals last until the bar line; a sharp needs a natural to go.
        std::map<int, bool> accidental;
        long long accidentalBar = LLONG_MIN;
        for (size_t i = 0; i < figs.size(); ++i) {
            const notation::Figure& f = figs[i];
            const float x = xAt(meter.time(f.at));  // heads sit on the time they start
            const long long bar = f.at >= 0 ? f.at / 16 : -((-f.at + 15) / 16);
            if (bar != accidentalBar) {
                accidental.clear();
                accidentalBar = bar;
            }
            if (f.rest) {
                const float rx = f.length == 16 ? xAt(meter.time(f.at + 8)) - s * 1.6f : x;
                if (rx > notesLeft - s * 2 && rx < right + s) drawRest(d, rx, st, f.length, s, ink);
                continue;
            }
            stepsOf(f, steps, sharps);
            const bool continued = i > 0 && !figs[i - 1].rest && figs[i - 1].tied && figs[i - 1].notes == f.notes;
            if (x < notesLeft - s * 3 || x > right + s * 3) {
                for (size_t k = 0; k < steps.size(); ++k) accidental[steps[k]] = sharps[k];
                continue;
            }
            const int hi = *std::max_element(steps.begin(), steps.end()), lo = *std::min_element(steps.begin(), steps.end());
            const float lw = s * 0.95f;
            for (int l = highLine + 2; l <= hi; l += 2) d->AddLine(ImVec2(x - lw, st.y(l)), ImVec2(x + lw, st.y(l)), kInk, 1.0f);
            for (int l = lowLine - 2; l >= lo; l -= 2) d->AddLine(ImVec2(x - lw, st.y(l)), ImVec2(x + lw, st.y(l)), kInk, 1.0f);
            int shift = 0;
            for (size_t k = 0; k < steps.size(); ++k) {
                const float ny = st.y(steps[k]);
                auto was = accidental.find(steps[k]);
                const bool before = was != accidental.end() && was->second;
                if (sharps[k] != before && !continued) {
                    const ImVec2 at(x - s * (1.25f + 0.8f * static_cast<float>(shift++ % 2)), ny);
                    if (sharps[k]) drawSharp(d, at, s, ink);
                    else drawNatural(d, at, s, ink);
                }
                accidental[steps[k]] = sharps[k];
                if (f.length >= 8) drawHollowHead(d, ImVec2(x, ny), s, color, f.length == 16);
                else drawHead(d, ImVec2(x, ny), s, color);
                if (f.length == 3 || f.length == 6 || f.length == 12) {
                    const int dotStep = (steps[k] - st.center) % 2 == 0 ? steps[k] + 1 : steps[k];
                    d->AddCircleFilled(ImVec2(x + s * 1.0f, st.y(dotStep)), s * 0.17f, ink);
                }
                if (hovered && std::fabs(io.MousePos.x - x) < s * 0.7f && std::fabs(io.MousePos.y - ny) < s * 0.6f && f.id >= 0 &&
                    static_cast<size_t>(f.id) < notes.size())
                    tip = &notes[static_cast<size_t>(f.id)];
            }
            // The stem, and the flags of an eighth or sixteenth on its own.
            if (f.length < 16) {
                const float sx = up[i] ? x + s * 0.55f : x - s * 0.55f;
                const float base = up[i] ? st.y(lo) : st.y(hi);
                d->AddLine(ImVec2(sx, base), ImVec2(sx, stemTip[i]), ink, std::max(1.0f, s * 0.12f));
                if (group[i] < 0 && f.length <= 3) drawFlags(d, ImVec2(sx, stemTip[i]), up[i], f.length == 1 ? 2 : 1, s, ink);
            }
            // A tie to the next figure.
            if (f.tied) {
                size_t j = i + 1;
                while (j < figs.size() && figs[j].rest) ++j;
                const float x2 = j < figs.size() ? xAt(meter.time(figs[j].at)) : x + s * 3;
                const float dir = up[i] ? 1.0f : -1.0f;
                // From one head to the other, flatter the closer they are.
                const float xa = x + s * 0.3f, xb = std::max(xa + s * 0.4f, x2 - s * 0.3f);
                const float depth = std::min(s * 0.8f, (xb - xa) * 0.3f);
                for (int k : steps) {
                    const float ny = st.y(k) + dir * s * 0.6f;
                    d->AddBezierCubic(ImVec2(xa, ny), ImVec2(xa + (xb - xa) * 0.25f, ny + dir * depth),
                                      ImVec2(xb - (xb - xa) * 0.25f, ny + dir * depth), ImVec2(xb, ny), ink,
                                      std::max(1.0f, s * 0.14f));
                }
            }
        }
        // The beams over the stems' ends: one for eighths, a second for sixteenths.
        const float thick = s * 0.45f;
        for (const std::vector<size_t>& g : groups) {
            const bool u = up[g.front()];
            const float tipY = stemTip[g.front()];
            auto stemX = [&](size_t i) { return xAt(meter.time(figs[i].at)) + (u ? s * 0.55f : -s * 0.55f); };
            const float dir = u ? 1.0f : -1.0f;  // the beams go inwards, towards the heads
            auto beam = [&](float xa, float xb, int level) {
                const float ya = tipY + dir * static_cast<float>(level) * s * 0.75f;
                d->AddRectFilled(ImVec2(std::min(xa, xb), std::min(ya, ya + dir * thick)),
                                 ImVec2(std::max(xa, xb) + 1, std::max(ya, ya + dir * thick)), ink);
            };
            beam(stemX(g.front()), stemX(g.back()), 0);
            for (size_t k = 0; k < g.size(); ++k) {
                if (figs[g[k]].length != 1) continue;
                const bool prev = k > 0 && figs[g[k - 1]].length == 1, next = k + 1 < g.size() && figs[g[k + 1]].length == 1;
                if (next) beam(stemX(g[k]), stemX(g[k + 1]), 1);
                else if (!prev) beam(stemX(g[k]), stemX(g[k]) + (k + 1 < g.size() ? s * 0.9f : -s * 0.9f), 1);
            }
        }
        d->PopClipRect();
        d->PopClipRect();
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, top + static_cast<float>(rows_.size()) * rowHeight));
    ImGui::Dummy(ImVec2(1, 1));
    // The playhead, across every track.
    const float px = xAt(now);
    if (px >= left && px <= right) {
        const ImVec2 clipMin = ImGui::GetWindowPos();
        const ImVec2 clipMax(clipMin.x + ImGui::GetWindowSize().x, clipMin.y + ImGui::GetWindowSize().y);
        d->PushClipRect(clipMin, clipMax, false);
        d->AddLine(ImVec2(px, clipMin.y), ImVec2(px, clipMax.y), kPlayhead, 2.0f);
        d->PopClipRect();
    }
    ImGui::EndChild();
    if (px >= left && px <= right) {
        wd->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + rulerHeight), kPlayhead, 2.0f);
        wd->AddTriangleFilled(ImVec2(px - 6, origin.y), ImVec2(px + 6, origin.y), ImVec2(px, origin.y + 8), kPlayhead);
    }
    if (tip) {
        const double held = static_cast<double>((tip->end ? tip->end : tip->start) - tip->start) / music->clockRate;
        ImGui::SetTooltip("%s  %s  (%.2f s)", noteName(tip->note).c_str(), tr(kVoiceNames[tip->voice]), held);
    }
    ImGui::End();
}

}  // namespace plumsg
