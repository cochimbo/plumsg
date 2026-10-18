#include "instrument_editor.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "imgui.h"

namespace plumsg {

namespace {

namespace fs = std::filesystem;

bool readBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string readText(const std::string& path) {
    std::vector<uint8_t> data;
    if (!readBytes(path, data)) return {};
    return std::string(data.begin(), data.end());
}

bool writeBytes(const std::string& path, const void* data, size_t size) {
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
    std::ofstream out(fs::u8path(path), std::ios::binary);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

std::string fileName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string withoutExtension(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    const size_t slash = name.find_last_of("/\\");
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? name : name.substr(0, dot);
}

std::string noteName(int note) {
    static const char* kNames[12] = {"Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"};
    note = std::clamp(note, 0, 127);
    return std::string(kNames[note % 12]) + std::to_string(note / 12 - 1);
}

std::string minutes(double seconds) {
    const int s = static_cast<int>(std::max(0.0, seconds));
    char text[16];
    std::snprintf(text, sizeof text, "%d:%02d", s / 60, s % 60);
    return text;
}

// The console's sound channels as RolandSound counts them, and the DAC.
constexpr int kDac = 10;
const char* const kVoiceNames[11] = {"FM 1", "FM 2", "FM 3", "FM 4", "FM 5", "FM 6", "PSG 1", "PSG 2", "PSG 3", "Ruido", "DAC"};
const int kVoiceOrder[11] = {0, 1, 2, 3, 4, 5, kDac, 6, 7, 8, 9};

const char* const kFamilies[16] = {"Pianos", "Percusión afinada", "Órganos", "Guitarras", "Bajos", "Cuerdas",
                                   "Conjuntos", "Metales", "Lengüetas", "Flautas", "Solistas sintetizados",
                                   "Fondos sintetizados", "Efectos sintetizados", "Étnicos", "Percusión",
                                   "Efectos de sonido"};

std::string describe(const RolandSound::Assignment& a) {
    if (a.original) return "Sonido original";
    if (a.drum >= 0) return std::string("Batería: ") + RolandSound::drumName(a.drum);
    std::string s = RolandSound::programName(a.program);
    if (a.octave) s += a.octave > 0 ? " (+" + std::to_string(a.octave) + " oct.)" : " (" + std::to_string(a.octave) + " oct.)";
    return s;
}

}  // namespace

InstrumentEditor::InstrumentEditor(std::string soundFolder) : folder_(std::move(soundFolder)) {
    player_.setRoland(&synth_);
}

RolandSound::Assignments InstrumentEditor::assignmentsFor(const std::string& soundFolder, const std::string& game) {
    RolandSound::Assignments all = RolandSound::parseAssignments(readText(soundFolder + "cache.ini"));
    if (!game.empty())
        for (const auto& [fp, a] : RolandSound::parseAssignments(
                 readText(soundFolder + withoutExtension(fileName(game)) + ".ini")))
            all[fp] = a;
    return all;
}

std::string InstrumentEditor::gamePath() const { return folder_ + gameName_ + ".ini"; }

void InstrumentEditor::open(const std::string& game) {
    open_ = true;
    if (game == game_ && !tracks_.empty()) return;
    stop();
    game_ = game;
    gameName_ = withoutExtension(fileName(game));
    message_.clear();
    selected_ = soloInstrument_ = 0;
    mutedVoices_ = soloVoices_ = 0;
    loadAssignments();
    listTracks();
    track_.clear();
    player_.load(VgmFile{});
    trackSeen_.clear();
    if (!tracks_.empty()) openTrack(tracks_.back());  // the newest recording
}

void InstrumentEditor::loadAssignments() {
    cache_ = RolandSound::parseAssignments(readText(cachePath()));
    own_ = game_.empty() ? RolandSound::Assignments{} : RolandSound::parseAssignments(readText(gamePath()));
    RolandSound::Assignments all = cache_;
    for (const auto& [fp, a] : own_) all[fp] = a;
    synth_.setAssignments(std::move(all));
}

void InstrumentEditor::listTracks() {
    tracks_.clear();
    if (gameName_.empty()) return;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(tracksFolder()), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        const std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : std::string();
        if (name.compare(0, gameName_.size() + 1, gameName_ + " ") == 0 && (ext == ".vgz" || ext == ".vgm"))
            tracks_.push_back(it->path().u8string());
    }
    std::sort(tracks_.begin(), tracks_.end());  // dated names: oldest first
}

void InstrumentEditor::useSoundfont(const RolandSound& from, int reverb, int chorus) {
    // Shared again each time: the emulator's may have changed.
    soundfontReady_ = synth_.shareSoundfont(from) || synth_.ready();
    synth_.setEffects(reverb, chorus);
    if (!soundfontReady_) rolandOutput_ = false;
    player_.setRolandOutput(rolandOutput_);
}

bool InstrumentEditor::openTrack(const std::string& path) {
    stop();
    std::vector<uint8_t> bytes;
    VgmFile file;
    std::string error;
    if (!readBytes(path, bytes) || !file.parse(std::move(bytes), &error)) {
        message_ = "No se pudo abrir " + fileName(path) + ": " + (error.empty() ? "no se puede leer." : error);
        return false;
    }
    if (file.ymClock == 0) {
        message_ = fileName(path) + " no tiene música de YM2612: no es de Mega Drive.";
        return false;
    }
    message_.clear();
    track_ = path;
    if (std::find(tracks_.begin(), tracks_.end(), path) == tracks_.end()) tracks_.push_back(path);
    synth_.clearSeen();
    player_.load(std::move(file));
    player_.setLoop(loop_);
    player_.analyse();  // every instrument in it, before playing
    trackSeen_ = synth_.seen();
    soloInstrument_ = 0;
    applyListening();
    return true;
}

void InstrumentEditor::saveRecording(const std::vector<uint8_t>& file, const std::string& game) {
    if (game != game_) open(game);
    const std::time_t now = std::time(nullptr);
    char when[32];
    std::strftime(when, sizeof when, "%Y-%m-%d %H%M%S", std::localtime(&now));
    const std::string path = tracksFolder() + (gameName_.empty() ? std::string("Juego") : gameName_) + " " + when + ".vgz";
    if (!writeBytes(path, file.data(), file.size())) {
        message_ = "No se pudo guardar la grabación en " + path + ".";
        return;
    }
    openTrack(path);
}

void InstrumentEditor::stop() {
    playing_ = false;
    synth_.allNotesOff();
}

void InstrumentEditor::play(double seconds, double rate, std::vector<int16_t>& out) {
    if (!playing()) return;
    player_.play(seconds, rate, out);
    if (player_.finished()) {
        playing_ = false;
        player_.restart();
    }
}

void InstrumentEditor::applyListening() {
    uint32_t muted = mutedVoices_;
    if (soloVoices_) muted |= 0x7FF & ~soloVoices_;
    std::set<uint32_t> silenced;
    if (soloInstrument_)
        for (const auto& [fp, seen] : trackSeen_)
            if (fp != soloInstrument_) silenced.insert(fp);
    player_.setListening(muted, std::move(silenced));
}

void InstrumentEditor::assign(uint32_t fingerprint, const RolandSound::Assignment* a, EditorRequests& out) {
    if (a) {
        if (!game_.empty()) own_[fingerprint] = *a;
        if (game_.empty() || share_) cache_[fingerprint] = *a;
    } else {
        own_.erase(fingerprint);
        if (game_.empty() || share_) cache_.erase(fingerprint);
    }
    const std::string ownText = RolandSound::formatAssignments(own_), cacheText = RolandSound::formatAssignments(cache_);
    bool ok = writeBytes(cachePath(), cacheText.data(), cacheText.size());
    if (!game_.empty()) ok = writeBytes(gamePath(), ownText.data(), ownText.size()) && ok;
    message_ = ok ? std::string() : "No se pudieron guardar las asignaciones en " + folder_ + ".";
    RolandSound::Assignments all = cache_;
    for (const auto& [fp, x] : own_) all[fp] = x;
    synth_.setAssignments(std::move(all));
    applyListening();
    out.assignmentsChanged = true;
}

// The instruments in the track, and those the game's file assigns that the track does
// not play: FM first, then the PSG; each kind in the order of the channel it plays on.
std::vector<InstrumentEditor::Instrument> InstrumentEditor::instruments() const {
    std::vector<Instrument> list;
    for (const auto& [fp, seen] : trackSeen_) list.push_back({fp, seen, true, {}});
    for (const auto& [fp, a] : own_) {
        if (trackSeen_.count(fp)) continue;
        Instrument i;
        i.fingerprint = fp;
        i.seen.kind = RolandSound::kindOf(fp);
        list.push_back(i);
    }
    auto lowest = [](const Instrument& i) {
        for (int v = 0; v < RolandSound::kVoices; ++v)
            if ((i.seen.channels >> v) & 1) return v;
        return 99;
    };
    std::sort(list.begin(), list.end(), [&](const Instrument& a, const Instrument& b) {
        if (a.seen.kind != b.seen.kind) return a.seen.kind < b.seen.kind;
        if (a.heard != b.heard) return a.heard;
        if (lowest(a) != lowest(b)) return lowest(a) < lowest(b);
        return a.fingerprint < b.fingerprint;
    });
    int fm = 0;
    for (Instrument& i : list) {
        switch (i.seen.kind) {
            case RolandSound::Kind::Fm: i.label = "FM " + std::to_string(++fm); break;
            case RolandSound::Kind::PsgTone: i.label = "PSG " + std::to_string((i.fingerprint & 0xFF) + 1); break;
            case RolandSound::Kind::PsgNoise: {
                const int control = static_cast<int>(i.fingerprint & 7);
                static const char* kRates[4] = {"agudo", "medio", "grave", "de PSG 3"};
                i.label = std::string((control & 4) ? "Ruido blanco " : "Ruido periódico ") + kRates[control & 3];
                break;
            }
        }
    }
    return list;
}

void InstrumentEditor::draw(const EditorView& view, EditorRequests& out) {
    const ImGuiViewport* screen = ImGui::GetMainViewport();
    const float top = ImGui::GetFrameHeight() + 10;  // below the menu bar
    ImGui::SetNextWindowPos(ImVec2(screen->Pos.x + 10, screen->Pos.y + top), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::min(960.0f, screen->Size.x - 20), std::min(680.0f, screen->Size.y - top - 10)),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Editor de instrumentos", &open_)) {
        ImGui::End();
        if (!open_) stop();
        return;
    }
    if (!open_) stop();
    if (!view.soundfontReady && !soundfontReady_) rolandOutput_ = false;
    ImGui::Text("Juego: %s", gameName_.empty() ? "ninguno (lo que elijas vale para todos los juegos)" : gameName_.c_str());
    drawTrack(view, out);
    if (!message_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "%s", message_.c_str());
    ImGui::Separator();

    const std::vector<Instrument> list = instruments();
    if (list.empty() && player_.loaded()) {
        ImGui::TextWrapped("En esta pista no suena música del YM2612 ni del PSG.");
        ImGui::End();
        return;
    }
    if (list.empty()) {
        ImGui::TextWrapped(
            "Graba un trozo del juego (abre el juego, pulsa Grabar y juega hasta que suene la música que quieres "
            "probar) o abre un .vgm con su música, de vgmrips.net por ejemplo. Aquí saldrán sus instrumentos.");
        ImGui::End();
        return;
    }
    // The lists take what the assignment below leaves.
    const float bottom = ImGui::GetFrameHeightWithSpacing() * 4 + ImGui::GetTextLineHeightWithSpacing() * 2;
    const float height = std::max(120.0f, ImGui::GetContentRegionAvail().y - bottom);
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("instrumentos", ImVec2(width * 0.64f, height), ImGuiChildFlags_ResizeX);
    drawInstruments(list);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("canales", ImVec2(0, height));
    drawChannels();
    ImGui::EndChild();
    ImGui::Separator();
    drawAssignment(list, out);
    ImGui::End();
}

void InstrumentEditor::drawTrack(const EditorView& view, EditorRequests& out) {
    // Which track: the game's recordings, or a .vgm.
    ImGui::SetNextItemWidth(360);
    const std::string current = track_.empty() ? std::string("(ninguna)") : fileName(track_);
    if (ImGui::BeginCombo("Pista", current.c_str())) {
        for (auto it = tracks_.rbegin(); it != tracks_.rend(); ++it)
            if (ImGui::Selectable(fileName(*it).c_str(), *it == track_)) openTrack(*it);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Abrir .vgm...")) {
        stop();
        out.chooseTrack = true;
    }
    const bool canRecord = !view.loadedGame.empty() && view.loadedGame == game_;
    if (view.recording || !game_.empty()) ImGui::SameLine();
    if (view.recording) {
        if (ImGui::Button("Parar la grabación")) out.stopRecording = true;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Grabando %s", minutes(view.recordedSeconds).c_str());
        ImGui::TextWrapped("Juega hasta que suene lo que quieres probar y pulsa Parar. Para usar el teclado en el juego, haz clic fuera de esta ventana.");
    } else {
        if (canRecord) {
            if (ImGui::Button("Grabar del juego")) {
                stop();
                out.startRecording = true;
            }
        } else if (!game_.empty()) {
            if (ImGui::Button("Jugar para grabar")) {
                stop();
                out.load = game_;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Abre el juego; luego pulsa Grabar del juego aquí.");
        }
    }

    // Transport.
    ImGui::BeginDisabled(!player_.loaded() || view.recording);
    if (ImGui::Button(playing_ ? "Pausa" : "Tocar", ImVec2(80, 0))) {
        playing_ = !playing_;
        if (!playing_) synth_.allNotesOff();
    }
    ImGui::SameLine();
    if (ImGui::Button("Al principio")) {
        player_.restart();
        applyListening();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Repetir", &loop_)) player_.setLoop(loop_);
    ImGui::SameLine();
    const float length = static_cast<float>(std::max(1.0, player_.file().seconds()));
    float pos = seekTo_ >= 0 ? seekTo_ : static_cast<float>(std::fmod(player_.position(), static_cast<double>(length)));
    ImGui::SetNextItemWidth(260);
    const std::string label = minutes(pos) + " / " + minutes(length);
    if (ImGui::SliderFloat("##posicion", &pos, 0.0f, length, label.c_str())) seekTo_ = pos;
    if (ImGui::IsItemDeactivatedAfterEdit() && seekTo_ >= 0) {
        player_.seek(seekTo_);
        applyListening();
        seekTo_ = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::RadioButton("Original", !rolandOutput_)) {
        rolandOutput_ = false;
        player_.setRolandOutput(false);
        applyListening();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!soundfontReady_);
    if (ImGui::RadioButton("Roland", rolandOutput_)) {
        rolandOutput_ = true;
        player_.setRolandOutput(true);
        applyListening();
    }
    ImGui::EndDisabled();
    if (!soundfontReady_ && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("No se ha podido abrir el SoundFont (Sonido > Elegir SoundFont).");
    ImGui::EndDisabled();
    if (player_.loaded() && !player_.file().game.empty())
        ImGui::TextDisabled("%s%s%s", player_.file().game.c_str(), player_.file().title.empty() ? "" : " - ",
                            player_.file().title.c_str());
}

void InstrumentEditor::drawInstruments(const std::vector<Instrument>& list) {
    ImGui::TextDisabled("Instrumentos (elige uno para cambiarlo)");
    ImGui::BeginChild("lista", ImVec2(0, 0), ImGuiChildFlags_Borders);
    std::set<uint32_t> now;  // the instruments playing a note now
    const uint32_t active = synth_.activeVoices();
    for (int v = 0; v < RolandSound::kVoices; ++v)
        if ((active >> v) & 1) now.insert(synth_.voiceInstrument(v));
    if (!selected_ || std::none_of(list.begin(), list.end(), [&](const Instrument& i) { return i.fingerprint == selected_; }))
        selected_ = list.front().fingerprint;
    for (const Instrument& i : list) {
        ImGui::PushID(static_cast<int>(i.fingerprint));
        const RolandSound::Assignment a = synth_.assignmentFor(i.fingerprint);
        const bool byHand = synth_.assignments().count(i.fingerprint) != 0;
        std::string text = i.label + "   " + describe(a) + (byHand ? "" : "  (sugerido)");
        if (i.heard) {
            std::string channels;
            for (int v = 0; v < RolandSound::kVoices; ++v)
                if ((i.seen.channels >> v) & 1) channels += (channels.empty() ? "" : ", ") + std::string(kVoiceNames[v]);
            text += "   [" + channels + "; " + std::to_string(i.seen.notes) + (i.seen.notes == 1 ? " nota]" : " notas]");
        } else {
            text += "   [no suena en esta pista]";
        }
        const bool on = playing_ && now.count(i.fingerprint);
        if (on) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
        if (ImGui::Selectable(text.c_str(), selected_ == i.fingerprint)) selected_ = i.fingerprint;
        if (on) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void InstrumentEditor::drawChannels() {
    ImGui::TextDisabled("Canales");
    const uint32_t active = synth_.activeVoices();
    bool changed = false;
    for (int v : kVoiceOrder) {
        ImGui::PushID(v);
        const bool on = playing_ && v < RolandSound::kVoices && ((active >> v) & 1);
        ImGui::TextColored(on ? ImVec4(0.45f, 0.95f, 0.55f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%-6s", kVoiceNames[v]);
        ImGui::SameLine(80);
        bool solo = (soloVoices_ >> v) & 1, mute = (mutedVoices_ >> v) & 1;
        if (ImGui::Checkbox("Solo", &solo)) {
            soloVoices_ ^= 1u << v;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Silencio", &mute)) {
            mutedVoices_ ^= 1u << v;
            changed = true;
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Oír todos")) {
        soloVoices_ = mutedVoices_ = 0;
        changed = true;
    }
    if (changed) applyListening();
}

void InstrumentEditor::drawAssignment(const std::vector<Instrument>& list, EditorRequests& out) {
    auto it = std::find_if(list.begin(), list.end(), [&](const Instrument& i) { return i.fingerprint == selected_; });
    if (it == list.end()) return;
    const Instrument& inst = *it;
    const bool noise = inst.seen.kind == RolandSound::Kind::PsgNoise;
    RolandSound::Assignment a = synth_.assignmentFor(inst.fingerprint);
    const RolandSound::Assignment before = a;
    if (inst.heard)
        ImGui::Text("%s: %d %s, de %s a %s. Sugerencia: %s.", inst.label.c_str(), inst.seen.notes, inst.seen.notes == 1 ? "nota" : "notas",
                    noteName(inst.seen.low).c_str(), noteName(inst.seen.high).c_str(), describe(inst.seen.rules).c_str());
    else
        ImGui::Text("%s", inst.label.c_str());

    int kind = a.original ? 2 : a.drum >= 0 ? 1 : 0;
    if (!noise) {
        if (ImGui::RadioButton("Instrumento", kind == 0)) kind = 0;
        ImGui::SameLine();
    }
    if (ImGui::RadioButton("Batería", kind == 1)) kind = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("Sonido original", kind == 2)) kind = 2;
    if (kind == 0 && (a.original || a.drum >= 0)) {
        a.original = false;
        a.drum = -1;
        if (a.program < 0) a.program = inst.seen.rules.program >= 0 ? inst.seen.rules.program : 0;
    } else if (kind == 1 && a.drum < 0) {
        a.original = false;
        a.drum = inst.seen.rules.drum >= 0 ? inst.seen.rules.drum : 38;
        a.program = -1;
    } else if (kind == 2) {
        a.original = true;
    }

    ImGui::BeginDisabled(kind == 2);
    ImGui::SetNextItemWidth(300);
    if (kind == 0) {
        const std::string preview = std::to_string(a.program + 1) + " " + RolandSound::programName(a.program);
        if (ImGui::BeginCombo("Instrumento GM", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int p = 0; p < 128; ++p) {
                if (p % 8 == 0) ImGui::SeparatorText(kFamilies[p / 8]);
                const std::string name = std::to_string(p + 1) + " " + RolandSound::programName(p);
                if (ImGui::Selectable(name.c_str(), p == a.program)) a.program = p;
                if (p == a.program && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderInt("Octava", &a.octave, -3, 3, "%+d");
    } else {
        const char* drum = RolandSound::drumName(a.drum);
        if (ImGui::BeginCombo("Pieza del kit", drum ? drum : "", ImGuiComboFlags_HeightLarge)) {
            for (int n = 35; n <= 81; ++n)
                if (ImGui::Selectable(RolandSound::drumName(n), n == a.drum)) a.drum = n;
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderInt("Volumen", &a.volume, 0, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    // Sliders save when let go, not on every step.
    const bool sliding = ImGui::IsItemActive();

    bool solo = soloInstrument_ == inst.fingerprint;
    if (ImGui::Checkbox("Escuchar solo este", &solo)) {
        soloInstrument_ = solo ? inst.fingerprint : 0;
        applyListening();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!synth_.assignments().count(inst.fingerprint));
    if (ImGui::Button("Volver a la sugerencia")) {
        assign(inst.fingerprint, nullptr, out);
        ImGui::EndDisabled();
        return;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(game_.empty());
    ImGui::Checkbox("Usar también en otros juegos", &share_);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Lo que elijas se guarda también en la lista común: el mismo sonido en otro juego\n"
                          "saldrá ya asignado. Lo de cada juego manda sobre la lista común.");

    if (a != before || (!sliding && pendingSlide_)) {
        if (sliding) {
            // Heard at once, saved when the slider is let go.
            RolandSound::Assignments all = synth_.assignments();
            all[inst.fingerprint] = a;
            synth_.setAssignments(std::move(all));
            pendingSlide_ = true;
        } else {
            pendingSlide_ = false;
            assign(inst.fingerprint, &a, out);
        }
    }
}

}  // namespace plumsg
