#include "score_view.h"

#include <algorithm>
#include <array>
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
        rows_.push_back(r);
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
    ImGui::SliderFloat("##zoom", &seconds_, 2.0f, 30.0f, tr("%.0f s a la vista"), ImGuiSliderFlags_AlwaysClamp);
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
    drawToolbar(*music, out);
    updateRows(*music);

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
        viewStart_ = now - seconds_ * (music->track ? 0.25 : 0.9);
        if (music->track) viewStart_ = std::max(0.0, viewStart_);
    }
    auto xAt = [&](double t) { return left + static_cast<float>((t - viewStart_) / perPixel); };
    auto timeAt = [&](float x) { return viewStart_ + static_cast<double>(x - left) * perPixel; };

    // The ruler, a mark per second (or every few, zoomed out); on a track it seeks.
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
    const int step = seconds_ <= 10 ? 1 : seconds_ <= 20 ? 2 : 5;
    for (int t = static_cast<int>(std::floor(viewStart_ / step)) * step; xAt(t) <= right; t += step) {
        const float x = xAt(t);
        if (x < left) continue;
        wd->AddLine(ImVec2(x, origin.y + rulerHeight * 0.55f), ImVec2(x, origin.y + rulerHeight), ImGui::GetColorU32(ImGuiCol_Text));
        if (music->track) wd->AddText(ImVec2(x + 3, origin.y + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), minutes(t).c_str());
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
    const float rowHeight = s * 12;
    const RolandSound::Score& notes = *music->notes;
    const double rate = music->clockRate;
    const RolandSound::ScoreNote* tip = nullptr;
    const float top = ImGui::GetCursorScreenPos().y;
    for (size_t ri = 0; ri < rows_.size(); ++ri) {
        const Row& row = rows_[ri];
        ImGui::PushID(static_cast<int>(row.fingerprint));
        const float y0 = top + static_cast<float>(ri) * rowHeight;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y0));
        const bool off = music->silenced && music->silenced->count(row.fingerprint);
        const ImU32 color = trackColor(row.index, off ? 0.35f : 1.0f);
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
                seconds_ = std::clamp(seconds_ * (io.MouseWheel > 0 ? 0.8f : 1.25f), 2.0f, 30.0f);
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
        Staff st{y0 + rowHeight / 2, s / 2, row.treble ? 34 : 22};
        const int lowLine = st.center - 4, highLine = st.center + 4;
        for (int l = lowLine; l <= highLine; l += 2) d->AddLine(ImVec2(left, st.y(l)), ImVec2(right, st.y(l)), kInk, 1.0f);
        if (row.treble) drawTrebleClef(d, left + s * 1.4f, st, s, kInk);
        else drawBassClef(d, left + s * 1.2f, st, s, kInk);
        const float notesLeft = left + s * 3.6f;
        d->PushClipRect(ImVec2(notesLeft - s, y0 + 1), ImVec2(right, y0 + rowHeight - 1), true);
        // Bars first, then the heads over them.
        for (int pass = 0; pass < 2; ++pass) {
            for (const RolandSound::ScoreNote& n : notes) {
                if (n.fingerprint != row.fingerprint) continue;
                const double start = static_cast<double>(n.start) / rate;
                const double end = n.end ? static_cast<double>(n.end) / rate : std::max(start, music->track ? start : music->now);
                const float x0 = xAt(start), x1 = xAt(end);
                if (x0 > right + s) break;  // they come in the order they start
                if (x1 < notesLeft - s) continue;
                bool sharp = false;
                const int stepOf = std::clamp(staffStep(n.note, &sharp), lowLine - 7, highLine + 7);
                const float ny = st.y(stepOf);
                const bool past = !music->track || start <= now;
                if (pass == 0) {
                    d->AddRectFilled(ImVec2(x0, ny - s * 0.2f), ImVec2(std::max(x1, x0 + 1), ny + s * 0.2f),
                                     trackColor(row.index, off ? 0.15f : past ? 0.5f : 0.3f), s * 0.2f);
                    continue;
                }
                if (x0 < notesLeft - s) continue;  // only its bar is left in view
                const float lw = s * 0.95f;
                for (int l = highLine + 2; l <= stepOf; l += 2) d->AddLine(ImVec2(x0 - lw, st.y(l)), ImVec2(x0 + lw, st.y(l)), kInk, 1.0f);
                for (int l = lowLine - 2; l >= stepOf; l -= 2) d->AddLine(ImVec2(x0 - lw, st.y(l)), ImVec2(x0 + lw, st.y(l)), kInk, 1.0f);
                if (sharp) drawSharp(d, ImVec2(x0 - s * 1.15f, ny), s, kInk);
                drawHead(d, ImVec2(x0, ny), s, off ? trackColor(row.index, 0.35f) : past ? color : trackColor(row.index, 0.7f));
                if (hovered && std::fabs(io.MousePos.x - x0) < s * 0.7f && std::fabs(io.MousePos.y - ny) < s * 0.6f) tip = &n;
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
        const double held = static_cast<double>((tip->end ? tip->end : tip->start) - tip->start) / rate;
        ImGui::SetTooltip("%s  %s  (%.2f s)", noteName(tip->note).c_str(), tr(kVoiceNames[tip->voice]), held);
    }
    ImGui::End();
}

}  // namespace plumsg
