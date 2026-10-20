#include "instrument_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include <SDL.h>

#include "imgui.h"
#include "../core/i18n.h"
#include "miniz.h"

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

// The console's sound channels as RolandSound counts them, and the DAC: a Mega Drive's,
// and a Master System's with its YM2413 (nine FM channels or six and the drums).
constexpr int kDac = 10;
const char* const kVoiceNames[RolandSound::kVoices] = {
    "FM 1",   "FM 2",   "FM 3",       "FM 4",     "FM 5",         "FM 6",          "PSG 1",
    "PSG 2",  "PSG 3",  N_("Ruido"),  "DAC",      "FM 7",         "FM 8",          "FM 9",
    N_("Bombo"), N_("Caja"), N_("Tom"), N_("Platillo"), N_("Charles")};
const std::vector<int> kVoiceOrder = {0, 1, 2, 3, 4, 5, kDac, 6, 7, 8, 9};
const std::vector<int> kVoiceOrderOpll = {0, 1, 2, 3, 4, 5, 11, 12, 13, 14, 15, 16, 17, 18, 6, 7, 8, 9};

const char* const kFamilies[16] = {N_("Pianos"), N_("Percusión afinada"), N_("Órganos"), N_("Guitarras"), N_("Bajos"), N_("Cuerdas"),
                                   N_("Conjuntos"), N_("Metales"), N_("Lengüetas"), N_("Flautas"), N_("Solistas sintetizados"),
                                   N_("Fondos sintetizados"), N_("Efectos sintetizados"), N_("Étnicos"), N_("Percusión"),
                                   N_("Efectos de sonido")};

// A, B ... Z, then AA, AB...
std::string instrumentLetter(int n) {
    std::string s;
    for (++n; n > 0; n = (n - 1) / 26) s.insert(s.begin(), static_cast<char>('A' + (n - 1) % 26));
    return s;
}

std::string describe(const RolandSound::Assignment& a) {
    if (a.original) return tr("Sonido original");
    if (a.drum >= 0) return trf("Batería: %s", RolandSound::drumName(a.drum));
    std::string s = RolandSound::programName(a.program);
    if (a.octave) s += trf(" (%+d oct.)", a.octave);
    return s;
}

}  // namespace

InstrumentEditor::InstrumentEditor(std::string soundFolder) : folder_(std::move(soundFolder)) {
    player_.setRoland(&synth_);
}

InstrumentEditor::~InstrumentEditor() { endExtraction(); }

RolandSound::Assignments InstrumentEditor::assignmentsFor(const std::string& soundFolder, const std::string& game) {
    RolandSound::Assignments all = RolandSound::parseAssignments(readText(soundFolder + "cache.ini"));
    if (!game.empty())
        for (const auto& [fp, a] : RolandSound::parseAssignments(
                 readText(soundFolder + withoutExtension(fileName(game)) + ".ini")))
            all[fp] = a;
    return all;
}

std::set<uint32_t> InstrumentEditor::effectsFor(const std::string& soundFolder, const std::string& game) {
    std::set<uint32_t> out;
    if (game.empty()) return out;
    const std::string text = readText(soundFolder + withoutExtension(fileName(game)) + ".efectos");
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        char* stop = nullptr;
        const unsigned long fp = std::strtoul(line.c_str(), &stop, 16);
        if (stop != line.c_str() && fp) out.insert(static_cast<uint32_t>(fp));
    }
    return out;
}

std::string InstrumentEditor::gamePath() const { return folder_ + gameName_ + ".ini"; }

void InstrumentEditor::open(const std::string& game) {
    open_ = true;
    if (game == game_ && !tracks_.empty()) return;
    stop();
    if (game != game_) endExtraction();
    game_ = game;
    gameName_ = withoutExtension(fileName(game));
    message_.clear();
    selected_ = soloInstrument_ = 0;
    mutedVoices_ = soloVoices_ = 0;
    loadAssignments();
    listTracks();
    track_ = Track{};
    player_.load(VgmFile{});
    trackSeen_.clear();
    if (!tracks_.empty()) loadTrack(tracks_.front());
}

void InstrumentEditor::loadAssignments() {
    cache_ = RolandSound::parseAssignments(readText(cachePath()));
    own_ = game_.empty() ? RolandSound::Assignments{} : RolandSound::parseAssignments(readText(gamePath()));
    RolandSound::Assignments all = cache_;
    for (const auto& [fp, a] : own_) all[fp] = a;
    synth_.setAssignments(std::move(all));
}

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isTrack(const std::string& name) {
    const std::string l = lower(name);
    return l.size() > 4 && (l.compare(l.size() - 4, 4, ".vgm") == 0 || l.compare(l.size() - 4, 4, ".vgz") == 0);
}

bool isZip(const std::string& name) {
    const std::string l = lower(name);
    return l.size() > 4 && l.compare(l.size() - 4, 4, ".zip") == 0;
}

// The songs in a .zip, by name, with their contents.
std::vector<std::pair<std::string, std::vector<uint8_t>>> zipTracks(const std::vector<uint8_t>& zip) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zip.data(), zip.size(), 0)) return out;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&archive, i, &st) || st.m_is_directory || !isTrack(st.m_filename)) continue;
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&archive, i, &size, 0);
        if (!data) continue;
        const uint8_t* b = static_cast<const uint8_t*>(data);
        out.emplace_back(st.m_filename, std::vector<uint8_t>(b, b + size));
        mz_free(data);
    }
    mz_zip_reader_end(&archive);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

// What a song is called: its tag's title, else its file name.
std::string trackTitle(std::vector<uint8_t> bytes, const std::string& name) {
    VgmFile file;
    if (file.parse(std::move(bytes), nullptr) && !file.title.empty()) return file.title;
    return withoutExtension(fileName(name));
}

}  // namespace

// The game's songs (sonido/pistas/<game>/: .vgm, .vgz and vgmrips .zip packs), then its
// recordings (sonido/pistas/<game> <date>.vgz), newest first.
void InstrumentEditor::listTracks() {
    tracks_.clear();
    if (gameName_.empty()) return;
    std::error_code ec;
    std::vector<std::string> songs, recordings;
    for (fs::directory_iterator it(fs::u8path(gameTracksFolder()), ec), end; !ec && it != end; it.increment(ec))
        if (isTrack(it->path().u8string()) || isZip(it->path().u8string())) songs.push_back(it->path().u8string());
    ec.clear();
    for (fs::directory_iterator it(fs::u8path(tracksFolder()), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        if (name.compare(0, gameName_.size() + 1, gameName_ + " ") == 0 && isTrack(name))
            recordings.push_back(it->path().u8string());
    }
    std::sort(songs.begin(), songs.end());
    std::sort(recordings.rbegin(), recordings.rend());  // dated names
    for (const std::string& p : songs) addTracks(p);
    for (const std::string& p : recordings) {
        const std::string name = withoutExtension(fileName(p));
        tracks_.push_back({p, {}, trf("Grabación %s", name.substr(std::min(name.size(), gameName_.size() + 1)).c_str())});
    }
}

void InstrumentEditor::addTracks(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!readBytes(path, bytes)) return;
    if (isZip(path)) {
        for (auto& [name, data] : zipTracks(bytes)) {
            Track t{path, name, trackTitle(std::move(data), name)};
            if (std::find(tracks_.begin(), tracks_.end(), t) == tracks_.end()) tracks_.push_back(t);
        }
        return;
    }
    Track t{path, {}, trackTitle(std::move(bytes), path)};
    if (std::find(tracks_.begin(), tracks_.end(), t) == tracks_.end()) tracks_.push_back(t);
}

void InstrumentEditor::useSoundfont(const RolandSound& from, int reverb, int chorus,
                                    const std::array<int, RolandSound::kEqBands>& equalizer) {
    // Shared again each time: the emulator's may have changed.
    soundfontReady_ = synth_.shareSoundfont(from) || synth_.ready();
    synth_.setEffects(reverb, chorus);
    synth_.setEqualizer(equalizer);
    if (!soundfontReady_) rolandOutput_ = false;
    player_.setRolandOutput(rolandOutput_);
}

bool InstrumentEditor::openTrack(const std::string& path) {
    stop();
    std::string from = path;
    // Kept with the game's songs, to be in its list next time.
    if (!gameName_.empty() && fs::u8path(path).parent_path() != fs::u8path(gameTracksFolder()).parent_path() &&
        fs::u8path(path).parent_path() != fs::u8path(tracksFolder()).parent_path()) {
        std::vector<uint8_t> bytes;
        const std::string copy = gameTracksFolder() + fileName(path);
        if (readBytes(path, bytes) && writeBytes(copy, bytes.data(), bytes.size())) from = copy;
    }
    const size_t before = tracks_.size();
    addTracks(from);
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& t) { return t.path == from; });
    if (it == tracks_.end()) {
        message_ = isZip(path) ? trf("No se pudo abrir %s: no tiene ningún .vgm ni .vgz.", fileName(path).c_str())
                                : trf("No se pudo abrir %s.", fileName(path).c_str());
        return false;
    }
    // A new pack opens at its first song; one already listed, at the first too.
    return loadTrack(before < tracks_.size() ? tracks_[before] : *it);
}

bool InstrumentEditor::loadTrack(const Track& track) {
    stop();
    std::vector<uint8_t> bytes;
    if (!readBytes(track.path, bytes)) {
        message_ = trf("No se pudo leer %s.", track.path.c_str());
        return false;
    }
    if (!track.member.empty()) {
        bytes.clear();
        std::vector<uint8_t> zip;
        readBytes(track.path, zip);
        for (auto& [name, data] : zipTracks(zip))
            if (name == track.member) bytes = std::move(data);
    }
    VgmFile file;
    std::string error;
    if (!file.parse(std::move(bytes), &error)) {
        message_ = trf("No se pudo abrir %s: %s", track.title.c_str(), error.c_str());
        return false;
    }
    message_.clear();
    track_ = track;
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
        message_ = trf("No se pudo guardar la grabación en %s.", path.c_str());
        return;
    }
    listTracks();
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& t) { return t.path == path; });
    if (it != tracks_.end()) loadTrack(*it);
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
    if (soloVoices_) muted |= ((1u << RolandSound::kVoices) - 1) & ~soloVoices_;
    std::set<uint32_t> silenced;
    if (soloInstrument_)
        for (const auto& [fp, seen] : trackSeen_)
            if (fp != soloInstrument_) silenced.insert(fp);
    player_.setListening(muted, std::move(silenced));
}

void InstrumentEditor::resetAll(const std::vector<Instrument>& list, EditorRequests& out) {
    // The game's file empties, and the shared list loses this list's instruments (not
    // other games'): what an older plumsg saved there would still be heard.
    own_.clear();
    for (const Instrument& i : list) {
        cache_.erase(i.fingerprint);
        for (uint32_t old : synth_.aliases(i.fingerprint)) cache_.erase(old);
    }
    save(out);
}

void InstrumentEditor::save(EditorRequests& out) {
    const std::string ownText = RolandSound::formatAssignments(own_), cacheText = RolandSound::formatAssignments(cache_);
    bool ok = writeBytes(cachePath(), cacheText.data(), cacheText.size());
    if (!game_.empty()) ok = writeBytes(gamePath(), ownText.data(), ownText.size()) && ok;
    message_ = ok ? std::string() : trf("No se pudieron guardar las asignaciones en %s.", folder_.c_str());
    RolandSound::Assignments all = cache_;
    for (const auto& [fp, x] : own_) all[fp] = x;
    synth_.setAssignments(std::move(all));
    applyListening();
    out.assignmentsChanged = true;
}

void InstrumentEditor::assign(uint32_t fingerprint, const RolandSound::Assignment* a, EditorRequests& out) {
    // What is chosen goes to the game's file; with no game, to the shared list, which
    // every game reads (older plumsgs also put each game's choices there). Saved under its
    // fingerprint from now on: what an old file had under an old one goes.
    for (uint32_t old : synth_.aliases(fingerprint)) {
        own_.erase(old);
        if (game_.empty()) cache_.erase(old);
    }
    if (a) {
        if (game_.empty()) cache_[fingerprint] = *a;
        else own_[fingerprint] = *a;
    } else {
        // Back to the suggestion: also out of the shared list, or it would still be heard.
        own_.erase(fingerprint);
        cache_.erase(fingerprint);
        for (uint32_t old : synth_.aliases(fingerprint)) cache_.erase(old);
    }
    save(out);
}

// The instruments in the track, and those the game's file assigns that the track does
// not play: FM first, then the PSG; each kind in the order of the channel it plays on.
std::vector<InstrumentEditor::Instrument> InstrumentEditor::instruments() const {
    std::vector<Instrument> list;
    for (const auto& [fp, seen] : trackSeen_) list.push_back({fp, seen, true, {}});
    std::set<uint32_t> aliases;  // the old fingerprints of the track's instruments
    for (const auto& [fp, seen] : trackSeen_)
        for (uint32_t old : synth_.aliases(fp)) aliases.insert(old);
    for (const auto& [fp, a] : own_) {
        if (trackSeen_.count(fp) || aliases.count(fp)) continue;
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
            case RolandSound::Kind::Fm:
                if (const int n = RolandSound::opllInstrument(i.fingerprint)) i.label = std::string("FM: ") + RolandSound::opllInstrumentName(n);
                else i.label = "FM " + instrumentLetter(fm++);  // letters: FM 1-6 are the channels
                break;
            case RolandSound::Kind::Rhythm: i.label = std::string("FM: ") + RolandSound::rhythmName(static_cast<int>(i.fingerprint & 0xFF)); break;
            case RolandSound::Kind::PsgTone: i.label = "PSG " + std::to_string((i.fingerprint & 0xFF) + 1); break;
            case RolandSound::Kind::PsgNoise: {
                const int control = static_cast<int>(i.fingerprint & 7);
                static const char* kRates[4] = {N_("agudo"), N_("medio"), N_("grave"), N_("de PSG 3")};
                i.label = trf((control & 4) ? N_("Ruido blanco %s") : N_("Ruido periódico %s"), tr(kRates[control & 3]));
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
    if (!ImGui::Begin(tr("Editor de instrumentos"), &open_)) {
        ImGui::End();
        if (!open_) stop();
        return;
    }
    if (!open_) stop();
    if (!view.soundfontReady && !soundfontReady_) rolandOutput_ = false;
    ImGui::Text(tr("Juego: %s"), gameName_.empty() ? tr("ninguno (lo que elijas vale para todos los juegos)") : gameName_.c_str());
    drawTrack(view, out);
    if (!message_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "%s", message_.c_str());
    ImGui::Separator();

    const std::vector<Instrument> list = instruments();
    if (list.empty() && player_.loaded()) {
        ImGui::TextWrapped("%s", tr("En esta pista no suena música del YM2612 ni del PSG."));
        ImGui::End();
        return;
    }
    if (list.empty()) {
        ImGui::TextWrapped("%s", tr("Saca las canciones de la ROM del juego (Sacar canciones), graba un trozo del juego (abre el juego, pulsa "
            "Grabar y juega hasta que suene la música que quieres probar) o abre un .vgm con su música, de "
            "vgmrips.net por ejemplo. Aquí saldrán sus instrumentos."));
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
    drawChannels(list);
    ImGui::EndChild();
    ImGui::Separator();
    drawAssignment(list, out);
    ImGui::End();
}

void InstrumentEditor::drawTrack(const EditorView& view, EditorRequests& out) {
    // Which track: the game's recordings, or a .vgm.
    ImGui::SetNextItemWidth(360);
    const std::string current = track_.path.empty() ? std::string(tr("(ninguna)")) : track_.title;
    if (ImGui::BeginCombo(tr("Pista"), current.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (size_t i = 0; i < tracks_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(tracks_[i].title.c_str(), tracks_[i] == track_)) loadTrack(tracks_[i]);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Abrir .vgm..."))) {
        stop();
        out.chooseTrack = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Un .vgm o .vgz, o el .zip con todas las canciones del juego de vgmrips.net.\n"
                          "Se guarda con las pistas del juego, para tenerlo la próxima vez."));
    if (!gameName_.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Carpeta"))) {
            std::error_code ec;
            fs::create_directories(fs::u8path(gameTracksFolder()), ec);
            SDL_OpenURL(("file:///" + gameTracksFolder()).c_str());
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(tr("La carpeta de las canciones de este juego: lo que pongas en ella (.vgm, .vgz o\n"
                              "el .zip de vgmrips) sale en la lista.\n%s"), gameTracksFolder().c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(extraction_ != nullptr && !extraction_->done);
        if (ImGui::Button(tr("Sacar canciones"))) startExtraction();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr("Busca las canciones en la ROM del juego y las graba una a una, sin nombre\n"
                              "(Canción 1, Canción 2...), con las pistas del juego. Sirve para los juegos\n"
                              "con un driver de sonido como el de Sonic (SMPS), el de Streets of Rage 2,\n"
                              "el de EA (NHL 96) o el GEMS (Sonic Spinball, Earthworm Jim).\n"
                              "Tarda unos minutos."));
    }
    const bool canRecord = !view.loadedGame.empty() && view.loadedGame == game_;
    if (view.recording || !game_.empty()) ImGui::SameLine();
    if (view.recording) {
        if (ImGui::Button(tr("Parar la grabación"))) out.stopRecording = true;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), tr("Grabando %s"), minutes(view.recordedSeconds).c_str());
        ImGui::TextWrapped("%s", tr("Juega hasta que suene lo que quieres probar y pulsa Parar. Para usar el teclado en el juego, haz clic fuera de esta ventana."));
    } else {
        if (canRecord) {
            if (ImGui::Button(tr("Grabar del juego"))) {
                stop();
                out.startRecording = true;
            }
        } else if (!game_.empty()) {
            if (ImGui::Button(tr("Jugar para grabar"))) {
                stop();
                out.load = game_;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Abre el juego; luego pulsa Grabar del juego aquí."));
        }
    }

    drawExtraction(out);

    // Transport.
    ImGui::BeginDisabled(!player_.loaded() || view.recording);
    if (ImGui::Button(playing_ ? tr("Pausa") : tr("Tocar"), ImVec2(80, 0))) {
        playing_ = !playing_;
        if (!playing_) synth_.allNotesOff();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Al principio"))) {
        player_.restart();
        applyListening();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Repetir"), &loop_)) player_.setLoop(loop_);
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
    if (ImGui::RadioButton(tr("Original"), !rolandOutput_)) {
        rolandOutput_ = false;
        player_.setRolandOutput(false);
        applyListening();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!soundfontReady_);
    if (ImGui::RadioButton(tr("Sintetizador"), rolandOutput_)) {
        rolandOutput_ = true;
        player_.setRolandOutput(true);
        applyListening();
    }
    ImGui::EndDisabled();
    if (!soundfontReady_ && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("No se ha podido abrir el SoundFont: elige otro en la lista de al lado."));
    ImGui::EndDisabled();
    if (view.drawSoundfont) {  // which one the synthesizer plays, to compare them here
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("SoundFont"));
        ImGui::SameLine();
        view.drawSoundfont();
    }
    if (view.drawEqualizer) {
        if (view.drawSoundfont) ImGui::SameLine();
        if (ImGui::Button(tr("Ecualizador..."))) ImGui::OpenPopup("ecualizador");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Graves, medios y agudos del sintetizador, aquí y en los juegos."));
        if (ImGui::BeginPopup("ecualizador")) {
            view.drawEqualizer();
            ImGui::EndPopup();
        }
    }
    if (player_.loaded() && !player_.file().game.empty()) {
        if (view.drawSoundfont || view.drawEqualizer) ImGui::SameLine();
        ImGui::TextDisabled("%s%s%s", player_.file().game.c_str(), player_.file().title.empty() ? "" : " - ",
                            player_.file().title.c_str());
    }
}

void InstrumentEditor::startExtraction() {
    endExtraction();
    extraction_ = std::make_unique<Extraction>();
    extractionSeen_ = 0;
    message_.clear();
    Extraction* x = extraction_.get();
    const std::string rom = game_, game = gameName_, folder = gameTracksFolder();
    const std::string effectsPath = folder_ + gameName_ + ".efectos";
    x->thread = std::thread([x, rom, game, folder, effectsPath]() {
        SongExtractor extractor;
        std::string error;
        int saved = 0;
        const bool ok = extractor.run(
            rom, game,
            [x](const SongExtractor::Progress& p) {
                std::lock_guard<std::mutex> lock(x->mutex);
                x->progress = p;
                return !x->stop.load();
            },
            [&](const SongExtractor::Song& song) {
                char name[32];
                std::snprintf(name, sizeof name, "Canción %02d.vgz", saved + 1);
                if (!writeBytes(folder + name, song.vgz.data(), song.vgz.size())) return;
                std::lock_guard<std::mutex> lock(x->mutex);
                x->saved = ++saved;
            },
            &error);
        // The sound effects it heard, for Sonido > Efectos de sonido originales.
        if (!extractor.effectInstruments().empty()) {
            std::string text = "; Los efectos de sonido de " + game + " (huellas de instrumentos FM), de Sacar canciones.\n";
            char line[16];
            for (uint32_t fp : extractor.effectInstruments()) {
                std::snprintf(line, sizeof line, "%08x\n", fp);
                text += line;
            }
            if (writeBytes(effectsPath, text.data(), text.size())) x->effects = true;
        }
        std::lock_guard<std::mutex> lock(x->mutex);
        if (!ok) x->error = error;
        x->done = true;
    });
}

void InstrumentEditor::endExtraction() {
    if (!extraction_) return;
    extraction_->stop = true;
    if (extraction_->thread.joinable()) extraction_->thread.join();
    extraction_.reset();
}

void InstrumentEditor::drawExtraction(EditorRequests& out) {
    if (!extraction_) return;
    SongExtractor::Progress p;
    int saved = 0;
    std::string error;
    bool done = false;
    {
        // Done is read with the count: the songs all come at the end, quickly, and one
        // read before them would be taken for all there are.
        std::lock_guard<std::mutex> lock(extraction_->mutex);
        p = extraction_->progress;
        saved = extraction_->saved;
        error = extraction_->error;
        done = extraction_->done;
    }
    if (saved != extractionSeen_) {  // new songs: into the list, and the first one open
        extractionSeen_ = saved;
        listTracks();
        if (!player_.loaded() || track_.path.empty())
            if (!tracks_.empty()) loadTrack(tracks_.front());
    }
    if (done) {
        if (extraction_->effects) out.assignmentsChanged = true;  // the game's effects are known now
        if (!extraction_->stop) {
            if (!error.empty())
                message_ = error;
            else
                message_ = saved == 1 ? std::string(tr("He sacado 1 canción de la ROM: está en la lista de pistas."))
                                      : trf("He sacado %d canciones de la ROM: están en la lista de pistas.", saved);
        }
        extraction_->thread.join();
        extraction_.reset();
        return;
    }
    const float fraction = p.total > 0 ? static_cast<float>(p.tried) / static_cast<float>(p.total) : 0.0f;
    const std::string label =
        p.total > 0 ? trf(saved == 1 ? N_("Probando %d de %d: %d canción") : N_("Probando %d de %d: %d canciones"), p.tried,
                          p.total, saved)
                    : p.doing;
    ImGui::ProgressBar(p.total > 0 ? fraction : -1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(360, 0),
                       label.c_str());
    ImGui::SameLine();
    if (ImGui::Button(tr("Parar"))) {
        extraction_->stop = true;
        message_ = saved ? tr("Parado: las canciones sacadas hasta ahora se quedan en la lista.") : "";
    }
}

// The instruments and the channels are two tables side by side with rows of the same
// height, a header each and their first row level, so that a line across reads straight.
namespace {
constexpr ImGuiTableFlags kListFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
const ImVec4 kPlaying(0.45f, 0.95f, 0.55f, 1.0f);
}  // namespace

void InstrumentEditor::drawInstruments(const std::vector<Instrument>& list) {
    ImGui::TextDisabled("%s", tr("Instrumentos (elige uno para cambiarlo)"));
    std::set<uint32_t> now;  // the instruments playing a note now
    const uint32_t active = synth_.activeVoices();
    for (int v = 0; v < RolandSound::kVoices; ++v)
        if ((active >> v) & 1) now.insert(synth_.voiceInstrument(v));
    if (!selected_ || std::none_of(list.begin(), list.end(), [&](const Instrument& i) { return i.fingerprint == selected_; }))
        selected_ = list.front().fingerprint;
    if (!ImGui::BeginTable("lista", 4, kListFlags | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Instrumento"), ImGuiTableColumnFlags_WidthStretch, 1.8f);
    ImGui::TableSetupColumn(tr("Suena como"), ImGuiTableColumnFlags_WidthStretch, 2.2f);
    ImGui::TableSetupColumn(tr("Canales"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableSetupColumn(tr("Notas"), ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableHeadersRow();
    const float row = ImGui::GetFrameHeight();
    for (const Instrument& i : list) {
        ImGui::PushID(static_cast<int>(i.fingerprint));
        ImGui::TableNextRow(ImGuiTableRowFlags_None, row);
        const RolandSound::Assignment a = synth_.assignmentFor(i.fingerprint);
        const bool byHand = synth_.assignedAs(i.fingerprint) != 0;
        const bool on = playing_ && now.count(i.fingerprint);
        if (on) ImGui::PushStyleColor(ImGuiCol_Text, kPlaying);
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));  // level with the other columns
        if (ImGui::Selectable(i.label.c_str(), selected_ == i.fingerprint,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, row)))
            selected_ = i.fingerprint;
        ImGui::PopStyleVar();
        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(describe(a).c_str());
        if (!byHand) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr("(sugerido)"));
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::AlignTextToFramePadding();
        if (i.heard) {
            // "FM 1, 3, 4", not "FM 1, FM 3, FM 4": it fits.
            std::string channels, kind;
            for (int v = 0; v < RolandSound::kVoices; ++v) {
                if (!((i.seen.channels >> v) & 1)) continue;
                const std::string name = tr(kVoiceNames[v]);
                const size_t space = name.find(' ');
                const std::string prefix = space == std::string::npos ? std::string() : name.substr(0, space + 1);
                if (!channels.empty()) channels += ", ";
                channels += !prefix.empty() && prefix == kind ? name.substr(space + 1) : name;
                kind = prefix;
            }
            ImGui::TextUnformatted(channels.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d", i.seen.notes);
        } else {
            ImGui::TextDisabled("%s", tr("no suena en esta pista"));
        }
        if (on) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void InstrumentEditor::drawChannels(const std::vector<Instrument>& list) {
    ImGui::TextDisabled("%s", tr("Canales"));
    // The channels the chosen instrument plays on stand out.
    uint32_t chosen = 0;
    for (const Instrument& i : list)
        if (i.fingerprint == selected_) chosen = i.seen.channels;
    const uint32_t active = synth_.activeVoices();
    bool changed = false;
    // Room left below the table for Oír todos.
    const float height = std::max(ImGui::GetFrameHeight() * 3, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing());
    if (ImGui::BeginTable("canales", 3, kListFlags, ImVec2(0, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Canal"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("Solo"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(tr("Silencio"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        const float row = ImGui::GetFrameHeight();
        for (int v : player_.file().opllClock > 0 ? kVoiceOrderOpll : kVoiceOrder) {
            ImGui::PushID(v);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row);
            const bool on = playing_ && v < RolandSound::kVoices && ((active >> v) & 1);
            if ((chosen >> v) & 1) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(on ? kPlaying : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", tr(kVoiceNames[v]));
            bool solo = (soloVoices_ >> v) & 1, mute = (mutedVoices_ >> v) & 1;
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Checkbox("##solo", &solo)) {
                soloVoices_ ^= 1u << v;
                changed = true;
            }
            ImGui::TableSetColumnIndex(2);
            if (ImGui::Checkbox("##silencio", &mute)) {
                mutedVoices_ ^= 1u << v;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(tr("Oír todos"))) {
        soloVoices_ = mutedVoices_ = 0;
        changed = true;
    }
    if (changed) applyListening();
}

void InstrumentEditor::drawAssignment(const std::vector<Instrument>& list, EditorRequests& out) {
    auto it = std::find_if(list.begin(), list.end(), [&](const Instrument& i) { return i.fingerprint == selected_; });
    if (it == list.end()) return;
    const Instrument& inst = *it;
    const bool noise = inst.seen.kind == RolandSound::Kind::PsgNoise || inst.seen.kind == RolandSound::Kind::Rhythm;
    RolandSound::Assignment a = synth_.assignmentFor(inst.fingerprint);
    const RolandSound::Assignment before = a;
    if (inst.heard)
        ImGui::Text(tr("%s: %d %s, de %s a %s. Sugerencia: %s."), inst.label.c_str(), inst.seen.notes, inst.seen.notes == 1 ? tr("nota") : tr("notas"),
                    noteName(inst.seen.low).c_str(), noteName(inst.seen.high).c_str(), describe(inst.seen.rules).c_str());
    else
        ImGui::Text("%s", inst.label.c_str());

    int kind = a.original ? 2 : a.drum >= 0 ? 1 : 0;
    if (!noise) {
        if (ImGui::RadioButton(tr("Instrumento"), kind == 0)) kind = 0;
        ImGui::SameLine();
    }
    if (ImGui::RadioButton(tr("Batería"), kind == 1)) kind = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Sonido original"), kind == 2)) kind = 2;
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
        if (ImGui::BeginCombo(tr("Instrumento GM"), preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int p = 0; p < 128; ++p) {
                if (p % 8 == 0) ImGui::SeparatorText(tr(kFamilies[p / 8]));
                const std::string name = std::to_string(p + 1) + " " + RolandSound::programName(p);
                if (ImGui::Selectable(name.c_str(), p == a.program)) a.program = p;
                if (p == a.program && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderInt(tr("Octava"), &a.octave, -3, 3, "%+d");
    } else {
        const char* drum = RolandSound::drumName(a.drum);
        if (ImGui::BeginCombo(tr("Pieza del kit"), drum ? drum : "", ImGuiComboFlags_HeightLarge)) {
            for (int n = 35; n <= 81; ++n)
                if (ImGui::Selectable(RolandSound::drumName(n), n == a.drum)) a.drum = n;
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderInt(tr("Volumen"), &a.volume, 0, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    // Sliders save when let go, not on every step.
    const bool sliding = ImGui::IsItemActive();

    bool solo = soloInstrument_ == inst.fingerprint;
    if (ImGui::Checkbox(tr("Escuchar solo este"), &solo)) {
        soloInstrument_ = solo ? inst.fingerprint : 0;
        applyListening();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!synth_.assignedAs(inst.fingerprint));
    if (ImGui::Button(tr("Volver a la sugerencia"))) {
        assign(inst.fingerprint, nullptr, out);
        ImGui::EndDisabled();
        return;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool anyByHand = std::any_of(list.begin(), list.end(), [&](const Instrument& i) { return synth_.assignedAs(i.fingerprint) != 0; });
    ImGui::BeginDisabled(!anyByHand && own_.empty());
    if (ImGui::Button(tr("Todo a las sugerencias..."))) ImGui::OpenPopup("todo");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("Quita todo lo elegido a mano para este juego: cada instrumento vuelve a sonar\n"
                          "como sugieren las reglas."));
    if (ImGui::BeginPopup("todo")) {
        ImGui::TextUnformatted(game_.empty() ? tr("¿Quitar lo elegido a mano en todos los instrumentos de esta pista?")
                                             : trf("¿Quitar todo lo elegido a mano en %s?", gameName_.c_str()).c_str());
        if (ImGui::Button(tr("Sí, todo a las sugerencias"))) {
            resetAll(list, out);
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancelar"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

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
