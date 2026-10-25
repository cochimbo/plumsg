#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../core/i18n.h"
#include "../core/megadrive.h"
#include "../core/netplay.h"
#include "../core/recording.h"
#include "../core/rewind.h"
#include "../core/system.h"
#include "../render/gl_renderers.h"
#include "../render/profile3d.h"
#include "../net/udp.h"
#include "achievements.h"
#include "audio.h"
#include "crash.h"
#include "fps_counter.h"
#include "game_options.h"
#include "http.h"
#include "languages.h"
#include "input.h"
#include "miniz.h"
#include "settings.h"
#include "ui.h"

using namespace plumsg;

namespace {

constexpr int kSlots = 10;
constexpr size_t kRewindBytes = size_t{256} << 20;  // memory the rewind snapshots may take

std::string withoutExtension(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path;
    return path.substr(0, dot);
}

std::string fileName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    SDL_RWops* f = SDL_RWFromFile(path.c_str(), "rb");
    if (!f) return false;
    const Sint64 size = SDL_RWsize(f);
    out.resize(size > 0 ? static_cast<size_t>(size) : 0);
    const bool ok = size > 0 && SDL_RWread(f, out.data(), 1, out.size()) == out.size();
    SDL_RWclose(f);
    return ok;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    SDL_RWops* f = SDL_RWFromFile(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = SDL_RWwrite(f, data.data(), 1, data.size()) == data.size();
    SDL_RWclose(f);
    return ok;
}

// Portable use (a plumsg.ini next to the executable) keeps everything there; otherwise
// the per-user settings folder, since the install folder may not be writable.
struct Paths {
    std::string ini;
    std::string states;    // folder for savestates, ending in a separator
    std::string profiles;  // folder for the games' 3D profiles, ending in a separator
    std::string cheats;    // folder for the games' cheat lists
    std::string covers;    // folder for the library's downloaded covers
    std::string info;      // folder for the library's game data, descriptions and screenshots
    std::string soundfont; // the SoundFont that comes with plumsg, next to the executable
    std::string sound;     // the instrument editor's folder: assignments and recordings
    std::string videos;    // folder for the games' video and GIF recordings
};

const char* const kSoundfont = "soundfonts/GeneralUser-GS.sf2";

Paths findPaths() {
    std::string base;
    if (char* b = SDL_GetBasePath()) {
        base = b;
        SDL_free(b);
        const std::string portable = base + "plumsg.ini";
        if (SDL_RWops* f = SDL_RWFromFile(portable.c_str(), "rb")) {
            SDL_RWclose(f);
            return {portable, base + "states/", base + "profiles/", base + "cheats/", base + "covers/", base + "game-info/",
                    base + kSoundfont, base + "sound/", base + "recordings/"};
        }
    }
    if (char* pref = SDL_GetPrefPath("plumsg", "plumsg")) {
        const std::string dir = pref;
        SDL_free(pref);
        return {dir + "plumsg.ini", dir + "states/", dir + "profiles/", dir + "cheats/", dir + "covers/", dir + "game-info/",
                base + kSoundfont, dir + "sound/", dir + "recordings/"};
    }
    return {base + "plumsg.ini", base + "states/", base + "profiles/", base + "cheats/", base + "covers/", base + "game-info/",
            base + kSoundfont, base + "sound/", base + "recordings/"};
}

// Up to 0.0.31 the folders and some files next to plumsg.ini had Spanish names. Renames
// them once to the English ones; a folder whose new name already exists is left alone.
void migrateSpanishNames(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::u8path(dir);
    static const std::pair<const char*, const char*> kFolders[] = {
        {"perfiles", "profiles"}, {"trucos", "cheats"}, {"caratulas", "covers"}, {"fichas", "game-info"},
        {"sonido", "sound"}, {"grabaciones", "recordings"}, {"fallos", "crash-reports"}, {"sound/pistas", "sound/tracks"}};
    for (const auto& [es, en] : kFolders) {
        const fs::path from = base / fs::u8path(es), to = base / fs::u8path(en);
        if (fs::is_directory(from, ec) && !fs::exists(to, ec)) fs::rename(from, to, ec);
    }
    // Files that only change their extension.
    static const std::array<std::array<const char*, 3>, 3> kFiles = {{{"states", ".logros", ".achievements"},
                                                                       {"sound", ".efectos", ".effects"},
                                                                       {"crash-reports", "ultimo-fallo.txt", "last-crash.txt"}}};
    for (const auto& [folder, es, en] : kFiles) {
        for (fs::directory_iterator it(base / folder, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().u8string();
            const std::string old = es;
            if (name.size() < old.size() || name.compare(name.size() - old.size(), old.size(), old) != 0) continue;
            const fs::path to = it->path().parent_path() / fs::u8path(name.substr(0, name.size() - old.size()) + en);
            std::error_code e;
            if (!fs::exists(to, e)) fs::rename(it->path(), to, e);
        }
        ec.clear();
    }
}

struct App {
    MegaDrive console;
    bool romLoaded = false;
    std::string romPath;
    std::string statesDir;
    std::string profilesDir;
    std::string cheatsDir;
    std::vector<Cheat> cheats;  // the loaded game's
    std::vector<SlotInfo> slots = std::vector<SlotInfo>(kSlots);
    std::string lastError;  // why the last load failed
    // Joined a netplay game: the console runs the host's game, cartridge save
    // included, which must not overwrite ours. Until another game is loaded.
    bool hostsBattery = false;
    std::string achievementsHash;  // the game as RetroAchievements knows it
    bool noCheats = false;         // RetroAchievements' hardcore mode: the cheats stay off

    std::string savePath() const { return withoutExtension(romPath) + ".sav"; }
    std::string statePath(int slot) const {
        return statesDir + withoutExtension(fileName(romPath)) + ".s" + std::to_string(slot);
    }
    // A game's own options (3D profile included) and its cheats, by the game's path.
    std::string profilePath(const std::string& game) const { return profilesDir + withoutExtension(fileName(game)) + ".ini"; }
    std::string cheatsPath(const std::string& game) const { return cheatsDir + withoutExtension(fileName(game)) + ".txt"; }
    std::string cheatsPath() const { return cheatsPath(romPath); }

    // Hands the console the codes that are on and valid.
    void applyCheats() {
        std::vector<CheatPatch> patches;
        if (noCheats) {
            console.setCheats(patches);
            return;
        }
        for (const Cheat& c : cheats) {
            std::vector<CheatPatch> p;
            if (c.enabled && decodeCheatCodes(c.code, &p, nullptr, console.isMasterSystem())) patches.insert(patches.end(), p.begin(), p.end());
        }
        console.setCheats(patches);
    }

    void loadCheats() {
        cheats = readCheats(romPath);
        applyCheats();
    }

    void saveCheats() { writeCheats(romPath, cheats); }

    std::vector<Cheat> readCheats(const std::string& game) const {
        std::vector<uint8_t> data;
        if (!readFile(cheatsPath(game), data)) return {};
        return parseCheats(std::string(data.begin(), data.end()));
    }

    void writeCheats(const std::string& game, const std::vector<Cheat>& list) const {
        std::error_code ec;
        if (list.empty()) {
            std::filesystem::remove(std::filesystem::u8path(cheatsPath(game)), ec);
            return;
        }
        std::filesystem::create_directories(std::filesystem::u8path(cheatsDir), ec);
        const std::string text = formatCheats(list);
        writeFile(cheatsPath(game), std::vector<uint8_t>(text.begin(), text.end()));
    }

    // The options `game` keeps of its own, over `general`.
    GameOverrides readOverrides(const std::string& game, const Settings& general, const InputConfig& generalInput) const {
        std::vector<uint8_t> data;
        if (game.empty() || !readFile(profilePath(game), data)) return {{}, general, generalInput};
        return parseGameOverrides(std::string(data.begin(), data.end()), general, generalInput);
    }

    void writeOverrides(const std::string& game, const GameOverrides& o) const {
        std::error_code ec;
        const std::string text = formatGameOverrides(o, fileName(game));
        if (text.empty()) {
            std::filesystem::remove(std::filesystem::u8path(profilePath(game)), ec);
            return;
        }
        std::filesystem::create_directories(std::filesystem::u8path(profilesDir), ec);
        writeFile(profilePath(game), std::vector<uint8_t>(text.begin(), text.end()));
    }

    // Cartridge SRAM, or the Mega CD's backup RAM, next to the game.
    void saveBattery() {
        if (!romLoaded || hostsBattery) return;
        if (MegaCd* cd = console.megaCd(); cd && !console.hasMsuMd()) {
            if (cd->backupRamChanged() &&
                writeFile(savePath(), std::vector<uint8_t>(cd->backupRam().begin(), cd->backupRam().end())))
                cd->clearBackupRamChanged();
            return;
        }
        if (console.cartridge().hasSaveData()) console.cartridge().writeSave(savePath());
    }

    void loadBattery() {
        MegaCd* cd = console.megaCd();
        if (!cd || console.hasMsuMd()) {  // an MSU-MD game saves on its cartridge
            console.cartridge().loadSave(savePath());
            return;
        }
        std::vector<uint8_t> data;
        if (readFile(savePath(), data) && data.size() == cd->backupRam().size())
            std::copy(data.begin(), data.end(), cd->backupRam().begin());
        else
            cd->formatBackupRam();
        cd->clearBackupRamChanged();
    }

    bool load(const std::string& path, SDL_Window* window) {
        saveBattery();
        std::string error;
        if (!console.loadRom(path, &error)) {
            lastError = error;
            // With a window the caller shows it inside the window: a system message box
            // can open behind the game in full screen and never be seen.
            if (!window) std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        romPath = path;
        setCrashContext(path);
        hostsBattery = false;
        lastError.clear();
        achievementsHash = Achievements::gameHash(console);  // before cheats change the ROM
        loadBattery();
        romLoaded = true;
        loadCheats();
        if (window) {
            std::string title = "plumsg - " + fileName(path);
            const std::string& name = console.cartridge().header().title;
            if (!name.empty()) title += " (" + name + ")";
            SDL_SetWindowTitle(window, title.c_str());
        }
        refreshSlots();
        return true;
    }

    // Closes the game: its cartridge save is written and nothing runs until another is loaded.
    void unload(SDL_Window* window) {
        if (!romLoaded) return;
        saveBattery();
        romLoaded = false;
        romPath.clear();
        setCrashContext("");
        hostsBattery = false;
        cheats.clear();
        console.setCheats({});
        if (window) SDL_SetWindowTitle(window, "plumsg");
        refreshSlots();
    }

    // Reads every slot's date and thumbnail for the Estado menu.
    void refreshSlots() {
        for (int i = 0; i < kSlots; ++i) refreshSlot(i);
    }

    void refreshSlot(int i) {
        SlotInfo& s = slots[static_cast<size_t>(i)];
        if (s.texture) {
            gl::DeleteTextures(1, &s.texture);
            s.texture = 0;
        }
        s.used = false;
        s.when.clear();
        if (!romLoaded) return;
        std::error_code ec;
        const auto path = std::filesystem::u8path(statePath(i));
        if (!std::filesystem::exists(path, ec)) return;
        s.used = true;
        // The file's modification time, as local date and time.
        const auto written = std::filesystem::last_write_time(path, ec);
        if (!ec) {
            const auto age = std::filesystem::file_time_type::clock::now() - written;
            const std::time_t t = std::time(nullptr) - std::chrono::duration_cast<std::chrono::seconds>(age).count();
            char text[32];
            if (std::strftime(text, sizeof text, "%d/%m/%Y %H:%M", std::localtime(&t))) s.when = text;
        }
        std::vector<uint8_t> data;
        std::vector<uint32_t> pixels;
        if (!readFile(statePath(i), data) || !MegaDrive::stateThumbnail(data, pixels, &s.width, &s.height)) return;
        gl::GenTextures(1, &s.texture);
        gl::BindTexture(gl::TEXTURE_2D, s.texture);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
        gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, s.width, s.height, 0, gl::BGRA, gl::UNSIGNED_INT_8_8_8_8_REV,
                       pixels.data());
    }

    std::string saveState(int slot) {
        std::vector<uint8_t> data;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(statesDir), ec);
        if (!console.saveState(data) || !writeFile(statePath(slot), data))
            return trf("No se pudo guardar el estado en %s: ¿falta espacio en el disco?", statePath(slot).c_str());
        refreshSlot(slot);
        return trf("Estado guardado en la ranura %d", slot);
    }

    std::string loadState(int slot, bool* loaded = nullptr) {
        if (loaded) *loaded = false;
        std::vector<uint8_t> data;
        if (!readFile(statePath(slot), data)) return trf("La ranura %d está vacía", slot);
        std::string error;
        if (!console.loadState(data, &error))
            return trf("No se pudo cargar el estado: %s. La partida sigue como estaba.", error.c_str());
        if (loaded) *loaded = true;
        return trf("Estado cargado de la ranura %d", slot);
    }
    // The achievements' progress when the state in `slot` was saved, next to it.
    std::string progressPath(int slot) const { return statePath(slot) + ".achievements"; }
};

// Saves what is on screen (the whole window) as a PNG, for screenshots taken from the
// command line.
bool captureWindow(SDL_Window* window, const std::string& path) {
    int w, h;
    SDL_GL_GetDrawableSize(window, &w, &h);
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4), flipped(pixels.size());
    gl::ReadPixels(0, 0, w, h, gl::RGBA, gl::UNSIGNED_BYTE, pixels.data());
    const size_t row = static_cast<size_t>(w) * 4;
    for (int y = 0; y < h; ++y)
        std::copy_n(&pixels[static_cast<size_t>(h - 1 - y) * row], row, &flipped[static_cast<size_t>(y) * row]);
    size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(flipped.data(), w, h, 4, &size);
    if (!png) return false;
    std::vector<uint8_t> bytes(static_cast<uint8_t*>(png), static_cast<uint8_t*>(png) + size);
    mz_free(png);
    return writeFile(path, bytes);
}

// Width of the picture at 224 lines: 4:3, plus the widescreen columns.
int pictureWidth(const Settings& s) { return 299 * (320 + s.wideLeft + s.wideRight) / 320; }

// Window size for a scale factor: the picture at 224 lines per scale step, plus the menu bar.
void applyWindowScale(SDL_Window* window, int scale, int menuHeight, const Settings& s) {
    SDL_SetWindowSize(window, pictureWidth(s) * scale, 224 * scale + menuHeight);
}

struct CommandLine {
    std::string rom;
    std::string capture;  // --screenshot file.png: take a screenshot and quit
    int frames = 120;     // --frames N: frames to run before the screenshot
    int pseudo3d = -1;    // --3d / --2d override the setting
    std::string view;     // --view name
    float tilt = -1000, yaw = -1000;
    bool stateMenu = false;  // --states-menu: start with the states menu open
    std::vector<std::string> library;  // --library folder (repeatable): start with the library of those folders open
    int filter = -1;         // --filter none|scanlines|crt
    int upscale = -1;        // --upscaling none|xbr|anime4k
    bool fps = false;        // --fps: with the FPS counter on
    bool downloadCovers = false;  // --download-covers: fetch the library's missing covers
    bool libraryList = false;     // --list: with --library, show it as a list
    bool bios = false;            // --bios: start with the Mega CD BIOS window open
    bool coverSearch = false;     // --find-cover: with --library, look for the first game's cover
    bool cheats = false;          // --cheats: start with the game's Cheats window open
    bool achievements = false;    // --achievements: start with the RetroAchievements window open
    int gameOptions = 0;          // --game-options [controls]: start with its Options for this game window open (2 on Controls)
    bool editor = false;          // --instruments: start with the instrument editor open
    std::string track;            // --track f.vgm: and this track in it
    double seek = -1;             // --seek s: and at that second of it, playing
    bool score = false;           // --score: start with the Partitura open
    // For launchers (LaunchBox, Playnite, frontends of arcade cabinets). These last only
    // for this session: the settings file keeps what it had.
    int fullscreen = -1;          // --fullscreen / --windowed
    int region = -1;              // --region auto|jp|us|eu (a Region)
    int slot = -1;                // --load-state N: load the game's state N once it is open
    bool netHost = false;         // --host: host a netplay game with the open game
    std::string netJoin;          // --join address: join that host
    std::string record;           // --record file.avi|file.gif: record the game from the start
    bool help = false;            // --help
    std::vector<std::string> unknown;  // options plumsg does not know
};

const char* const kHelp = N_(
    "plumsg [juego] [opciones]\n\n"
    "  juego                  cartucho (.md .gen .bin .smd .32x .sms .gg .zip) o disco (.cue .chd .iso)\n"
    "  --fullscreen           abrir a pantalla completa\n"
    "  --windowed             abrir en ventana\n"
    "  --3d / --2d            con o sin la vista pseudo-3D\n"
    "  --filter f             none, scanlines o crt\n"
    "  --upscaling u          none, xbr o anime4k (experimental)\n"
    "  --fps                  mostrar el contador de FPS con su gráfica\n"
    "  --region r             auto, jp, us o eu\n"
    "  --load-state N         cargar el estado guardado N (0-9) del juego\n"
    "  --host                 alojar una partida en red con el juego (puerto del menú Red)\n"
    "  --join direccion       unirse a la partida en red de ese anfitrión (IP:puerto)\n"
    "  --library carpeta      abrir la biblioteca de esa carpeta (se puede repetir)\n"
    "  --bios                 abrir la ventana de las BIOS del Mega CD\n"
    "  --find-cover           con --library: buscar la carátula del primer juego\n"
    "  --list                 con --library: verla como lista en vez de cuadrícula\n"
    "  --cheats               abrir los trucos del juego, con sus listas de internet\n"
    "  --achievements         abrir la ventana de RetroAchievements\n"
    "  --game-options         abrir las opciones propias del juego\n"
    "  --game-options controls   ...en la pestaña de sus controles\n"
    "  --instruments          abrir el editor de instrumentos (del juego, si se da)\n"
    "  --track f.vgm          con --instruments: abrir esa música (.vgm o .vgz)\n"
    "  --seek s               con --track: tocarla desde ese segundo\n"
    "  --score                abrir la Partitura\n"
    "  --record f.avi         grabar la partida desde el principio, en vídeo (.avi) o GIF (.gif)\n"
    "  --screenshot f.png     guardar una captura tras --frames N fotogramas y salir\n\n"
    "La pantalla completa y la región dadas aquí valen solo para esta vez. Las opciones\n"
    "en español de las versiones anteriores (--pantalla-completa, --biblioteca...) siguen valiendo.");

// The Spanish names options had up to 0.0.31, still accepted so launchers set up with them
// keep working.
std::string optionName(const std::string& a) {
    static const std::pair<const char*, const char*> kSpanish[] = {
        {"--captura", "--screenshot"}, {"--grabar", "--record"}, {"--cuadros", "--frames"}, {"--vista", "--view"},
        {"--inclinacion", "--tilt"}, {"--giro", "--yaw"}, {"--menu-estados", "--states-menu"},
        {"--biblioteca", "--library"}, {"--descargar-caratulas", "--download-covers"}, {"--lista", "--list"},
        {"--buscar-caratula", "--find-cover"}, {"--trucos", "--cheats"}, {"--logros", "--achievements"},
        {"--opciones-juego", "--game-options"}, {"--instrumentos", "--instruments"}, {"--pista", "--track"}, {"--partitura", "--score"},
        {"--filtro", "--filter"}, {"--reescalado", "--upscaling"}, {"--pantalla-completa", "--fullscreen"},
        {"--ventana", "--windowed"}, {"--estado", "--load-state"}, {"--alojar", "--host"}, {"--unirse", "--join"},
        {"--ayuda", "--help"}};
    for (const auto& [es, en] : kSpanish)
        if (a == es) return en;
    return a;
}

CommandLine parse(int argc, char** argv) {
    CommandLine c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = optionName(argv[i]);
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--screenshot") c.capture = next();
        else if (a == "--record") c.record = next();
        else if (a == "--frames") c.frames = std::max(1, std::atoi(next().c_str()));
        else if (a == "--3d") c.pseudo3d = 1;
        else if (a == "--2d") c.pseudo3d = 0;
        else if (a == "--view") c.view = next();
        else if (a == "--tilt") c.tilt = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--yaw") c.yaw = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--states-menu") c.stateMenu = true;
        else if (a == "--fps") c.fps = true;
        else if (a == "--library") c.library.push_back(next());
        else if (a == "--download-covers") c.downloadCovers = true;
        else if (a == "--list") c.libraryList = true;
        else if (a == "--bios") c.bios = true;
        else if (a == "--find-cover") c.coverSearch = true;
        else if (a == "--cheats") c.cheats = true;
        else if (a == "--achievements") c.achievements = true;
        else if (a == "--game-options") {
            c.gameOptions = 1;
            if (i + 1 < argc && (std::string(argv[i + 1]) == "controls" || std::string(argv[i + 1]) == "controles")) {
                c.gameOptions = 2;
                ++i;
            }
        }
        else if (a == "--instruments") c.editor = true;
        else if (a == "--track") c.track = next();
        else if (a == "--seek") c.seek = std::atof(next().c_str());
        else if (a == "--score") c.score = true;
        else if (a == "--filter") {
            const std::string f = next();
            c.filter = f == "crt" ? 2 : f == "scanlines" ? 1 : 0;
        }
        else if (a == "--upscaling") {
            const std::string r = next();
            c.upscale = r == "anime4k" ? 2 : r == "xbr" ? 1 : 0;
        }
        else if (a == "--fullscreen") c.fullscreen = 1;
        else if (a == "--windowed") c.fullscreen = 0;
        else if (a == "--region") {
            const std::string r = next();
            if (r == "auto") c.region = static_cast<int>(Region::Auto);
            else if (r == "jp" || r == "japan" || r == "japon") c.region = static_cast<int>(Region::JapanNtsc);
            else if (r == "us" || r == "usa") c.region = static_cast<int>(Region::UsaNtsc);
            else if (r == "eu" || r == "europe" || r == "europa" || r == "pal") c.region = static_cast<int>(Region::EuropePal);
            else c.unknown.push_back(a + " " + r);
        } else if (a == "--load-state") {
            const std::string n = next();
            char* end = nullptr;
            const long slot = std::strtol(n.c_str(), &end, 10);
            if (!n.empty() && *end == 0 && slot >= 0 && slot < kSlots) c.slot = static_cast<int>(slot);
            else c.unknown.push_back(a + " " + n);
        } else if (a == "--host") c.netHost = true;
        else if (a == "--join") {
            c.netJoin = next();
            if (c.netJoin.empty()) c.unknown.push_back(a);
        } else if (a == "--help" || a == "-h" || a == "/?") c.help = true;
        else if (a.size() > 1 && a[0] == '-') c.unknown.push_back(a);
        else c.rom = a;
    }
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    const CommandLine cmd = parse(argc, argv);
    const Paths paths = findPaths();
    const std::string settingsDir = paths.ini.substr(0, paths.ini.find_last_of("/\\") + 1);
    migrateSpanishNames(settingsDir);
    // Next to plumsg.ini: the reports of the runs that ended in a crash.
    const std::string crashDir = settingsDir + "crash-reports/";
    installCrashHandler(crashDir);
    Settings settings = Settings::load(paths.ini);
    // Until a language is picked in Opciones > Idioma, the system's.
    applyLanguage(settings.language.empty() ? systemLanguage() : settings.language);
    if (cmd.help || !cmd.unknown.empty()) {
        // A window build on Windows has no console, so this goes to a message box too.
        std::string text = tr(kHelp);
        if (!cmd.unknown.empty()) {
            text = trf("Opción desconocida: %s", cmd.unknown.front().c_str()) + "\n\n" + text;
        }
        std::fprintf(cmd.help ? stdout : stderr, "%s\n", text.c_str());
        SDL_ShowSimpleMessageBox(cmd.help ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR, "plumsg", text.c_str(),
                                 nullptr);
        return cmd.help ? 0 : 2;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const bool capturing = !cmd.capture.empty();
    auto input = std::make_unique<Input>(InputConfig::loadOrCreate(paths.ini));
    // A screenshot run never touches the user's settings.
    auto applyCommandLine = [&] {
        if (cmd.pseudo3d >= 0) settings.render.pseudo3d = cmd.pseudo3d == 1;
        if (!cmd.view.empty()) settings.view = cmd.view;
        if (cmd.tilt > -1000) settings.render.tilt = cmd.tilt;
        if (cmd.yaw > -1000) settings.render.yaw = cmd.yaw;
        if (cmd.filter >= 0) settings.render.filter = static_cast<Filter2D>(cmd.filter);
        if (cmd.upscale >= 0) settings.render.upscale = static_cast<Upscale2D>(cmd.upscale);
        if (cmd.fps) settings.fpsCounter = true;
    };
    applyCommandLine();
    // Fullscreen and region from the command line are for this session only.
    const bool fileFullscreen = settings.fullscreen;
    const Region fileRegion = settings.region;
    if (cmd.fullscreen >= 0) settings.fullscreen = cmd.fullscreen == 1;
    if (cmd.region >= 0) settings.region = static_cast<Region>(cmd.region);
    auto toSave = [&](Settings s) {
        if (cmd.fullscreen >= 0) s.fullscreen = fileFullscreen;
        if (cmd.region >= 0) s.region = fileRegion;
        return s;
    };
    if (capturing) settings.fullscreen = false;
    // The general values of the options a game can have its own of. While a game with
    // some of its own is loaded, settings holds the game's and the general ones wait here.
    Settings general = toSave(settings);
    GameOverrides game{{}, general, input->config()};  // the loaded game's own options

    // OpenGL 3.3 core everywhere; macOS only hands out core contexts marked forward compatible.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_Window* window = SDL_CreateWindow("plumsg", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          pictureWidth(settings) * settings.windowScale, 224 * settings.windowScale,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 320, 224);
    SDL_GLContext context = SDL_GL_CreateContext(window);
    const char* missing = context ? gl::load() : nullptr;
    if (!context || missing) {
        const std::string message =
            std::string(tr("plumsg necesita una tarjeta gráfica con OpenGL 3.3 y sus drivers actualizados.")) + "\n\n" +
            std::string(context ? trf("Falta la función %s", missing) : SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "plumsg", message.c_str(), window);
        return 1;
    }
    SDL_GL_SetSwapInterval(0);  // the loop below paces itself to the console's refresh rate

    std::vector<std::unique_ptr<Renderer>> renderers;
    renderers.push_back(std::make_unique<FlatRenderer>());
    renderers.push_back(std::make_unique<LayerRenderer>());
    auto pseudo3d = std::make_unique<Pseudo3DRenderer>();
    UiState uiState;
    for (const auto& r : renderers) uiState.views.push_back(r->name());
    auto viewIndex = [&] {
        for (size_t i = 0; i < renderers.size(); ++i)
            if (settings.view == renderers[i]->name()) return i;
        return size_t{0};
    };
    // An unknown view in the settings file (an old name, a typo) falls back to 2D.
    settings.view = renderers[viewIndex()]->name();

    input->openAll();
    auto ui = std::make_unique<Ui>(window, context, settings, *input);
    // The last run crashed: say where its report is, so that it can be sent.
    if (const std::string report = takeCrashReport(crashDir); !report.empty() && !capturing)
        ui->showError(trf("plumsg se cerró por un fallo la última vez.\nEl informe está en %s", report.c_str()));
    auto roland = std::make_unique<RolandSound>();  // before the console, which points at it
    auto app = std::make_unique<App>();
    app->statesDir = paths.states;
    app->profilesDir = paths.profiles;
    app->cheatsDir = paths.cheats;
    ui->library().setCoversFolder(paths.covers);
    ui->gameInfo().setFolder(paths.info);
    auto audio = std::make_unique<Audio>();
    // RetroAchievements: a screenshot run shows its windows but never logs in.
    Achievements achievements([](const std::string& url, const std::string& post, const std::string& agent,
                                 std::string& body) {
        std::vector<uint8_t> data;
        const int status = httpRequest(url, post, agent, data);
        body.assign(data.begin(), data.end());
        return status;
    });
    ui->setAchievements(&achievements);
    auto hardcore = [&] { return !capturing && settings.achievements && settings.raHardcore; };
    app->noCheats = hardcore();
    // Logs in (with the token kept from the last time) and identifies the loaded game,
    // or logs out when switched off.
    auto applyAchievements = [&] {
        if (capturing) return;
        achievements.setHardcore(hardcore());
        if (!settings.achievements) {
            if (achievements.loggedIn() || achievements.loggingIn()) achievements.logout();
            return;
        }
        if (!achievements.loggedIn() && !achievements.loggingIn() && !settings.raToken.empty())
            achievements.loginWithToken(settings.raUser, settings.raToken);
        if (app->romLoaded && !achievements.gameLoaded() && !achievements.loadingGame())
            achievements.loadGame(app->console, app->achievementsHash);
    };
    applyAchievements();

    auto applyConsole = [&] {
        app->console.setRegion(settings.region);
        app->console.setPadType(0, settings.padType[0]);
        app->console.setPadType(1, settings.padType[1]);
        app->console.setMasterSystemFm(settings.smsFm);
    };
    applyConsole();
    // The Roland sound, when chosen and its SoundFont opens; else the chips' own.
    std::string rolandFont;  // the SoundFont loaded (or tried)
    bool rolandFontOk = false;
    InstrumentEditor editor(paths.sound);
    ui->setEditor(&editor);
    ui->setSoundfonts(paths.soundfont, paths.sound + "soundfonts/");
    auto loadSoundfont = [&] {
        const std::string path = settings.soundfont.empty() ? paths.soundfont : settings.soundfont;
        if (path == rolandFont) return rolandFontOk;
        rolandFont = path;
        std::vector<uint8_t> data;
        rolandFontOk = readFile(path, data) && roland->loadSoundfont(data);
        if (!rolandFontOk && !capturing)
            ui->showError(trf("No se pudo abrir el SoundFont %s.\nSe oye el sonido original.", path.c_str()));
        return rolandFontOk;
    };
    // The game's instruments: those chosen by hand, and its sound effects.
    auto applyInstruments = [&](const std::string& path) {
        roland->setAssignments(InstrumentEditor::assignmentsFor(paths.sound, path));
        roland->setSoundEffects(InstrumentEditor::effectsFor(paths.sound, path), settings.effectsOriginal);
    };
    // The game's instruments switched off in the Partitura, until another game loads.
    std::set<uint32_t> gameSilenced;
    auto applySound = [&] {
        // A Mega CD disc's music is CD audio, already better than a SoundFont: there the
        // FM and PSG only play sound effects, which stay as they are. So for an MSU-MD
        // cartridge, which plays its music from CD tracks too.
        const bool on = settings.roland && !app->console.megaCd() && loadSoundfont();
        // With the chips' own sound, the notes are still followed for the Partitura and
        // to switch instruments off.
        const bool follow = !on && !app->console.megaCd() && (ui->scoreOpen() || !gameSilenced.empty());
        roland->setFollowOnly(follow);
        roland->setEffects(settings.reverb, settings.chorus);
        roland->setEqualizer(settings.equalizer);
        roland->setTimbreMatching(settings.compareSound && on);
        app->console.setRoland(on || follow ? roland.get() : nullptr, follow ? 0 : settings.chipMix);
    };
    applySound();
    // Each option goes to the game's file if the game has its own value of it, else to plumsg.ini.
    auto save = [&] {
        if (capturing) return;
        Settings out = toSave(settings);
        for (const GameOption& o : gameOptions()) {
            if (game.has(o.key)) o.copy(general, out);
            else o.copy(settings, general);
        }
        out.save(paths.ini, input->config());
        if (!app->romLoaded) return;
        game.values = settings;
        app->writeOverrides(app->romPath, game);
    };
    RewindBuffer rewind;
    bool runAheadFailed = false;  // the console could not go back once: no more for this game
    FramePacer pacer;
    auto setRewindLimits = [&] {
        rewind.setLimits(static_cast<size_t>(settings.rewindSeconds * app->console.refreshRate()), kRewindBytes);
    };
    setRewindLimits();
    // A game takes its own options where it has them, and the general ones elsewhere.
    // Its own buttons go over the general ones in Input, which keeps those for plumsg.ini.
    auto applyControls = [&] { input->setGameButtons(game.has(kGameControls) ? &game.controls : nullptr); };
    auto useGameOptions = [&](const std::string& path) {
        game.restore(general, settings);
        game = app->readOverrides(path, general, input->config());
        game.applyTo(settings);
        applyControls();
        applyConsole();
        gameSilenced.clear();
        roland->setListening(0, {});
        applySound();
        applyInstruments(path);
    };
    // The instrument editor records the game's music while it is played.
    VgmWriter recorder;
    bool recording = false;
    auto stopRecording = [&] {
        if (!recording) return;
        recording = false;
        app->console.setVgmWriter(nullptr);
        editor.saveRecording(recorder.finish(app->console.masterClocks(), withoutExtension(fileName(app->romPath))),
                             app->romPath);
    };
    // Grabar vídeo / Grabar GIF: the game as it is played, in the recordings folder.
    GameRecording video;
    std::vector<uint32_t> screen;  // the game's part of the window, read back for it
    // The 2D view records the console's own picture, sharp and without the filter; the
    // other views what is on screen, the only place their picture exists.
    auto recordsConsole = [&] { return !settings.render.pseudo3d && viewIndex() == 0; };
    auto stopVideo = [&] {
        if (!video.active()) return;
        const bool gif = video.format() == GameRecording::Format::Gif;
        std::string error;
        if (!video.finish(&error)) ui->showError(error);
        else if (gif) ui->notify(trf("GIF guardado en %s", video.path().c_str()));
        else ui->notify(trf("Vídeo guardado en %s", video.path().c_str()));
    };
    // Into `path`, or a new file in the recordings folder.
    auto startVideo = [&](GameRecording::Format format, std::string path = {}) {
        if (!app->romLoaded) return;
        const bool gif = format == GameRecording::Format::Gif;
        int width, height;
        if (recordsConsole()) {
            // As wide as H40 (H32 is stretched to it, as on a TV); a video three times that.
            const VideoFrameMd& f = app->console.frame();
            const int scale = gif ? 1 : 3;
            width = VideoFrameMd::kMaxWidth * scale;
            height = (f.interlaced ? f.height / 2 : f.height) * scale;
        } else {
            // The screen, made smaller by a whole number: a GIF to about 480x360, a video
            // to 720 lines at most.
            int w, h;
            SDL_GL_GetDrawableSize(window, &w, &h);
            width = w;
            height = std::max(1, h - ui->menuHeight());
            const int k = gif ? std::max((width + 479) / 480, (height + 359) / 360) : (height + 719) / 720;
            width = std::max(2, width / k / 2 * 2);
            height = std::max(2, height / k / 2 * 2);
        }
        if (path.empty()) {
            char stamp[32] = "";
            const std::time_t now = std::time(nullptr);
            std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
            path = paths.videos + withoutExtension(fileName(app->romPath)) + "_" + stamp + (gif ? ".gif" : ".avi");
        }
        std::string error;
        if (!video.start(path, format, width, height, app->console.refreshRate(), audio->freq(), &error)) {
            ui->showError(error);
            return;
        }
        ui->notify(gif ? tr("Grabando un GIF (Ctrl+G para pararlo)") : tr("Grabando vídeo (Ctrl+R para pararlo)"));
    };
    // The same kind again stops it; the other kind takes its place.
    auto toggleVideo = [&](GameRecording::Format format) {
        const bool same = video.active() && video.format() == format;
        stopVideo();
        if (!same) startVideo(format);
    };
    auto load = [&](const std::string& path) {
        stopRecording();
        stopVideo();
        const ConsoleSystem system = detectSystem(path);
        if (system == ConsoleSystem::MegaCD || !msuMdCue(path).empty()) {
            // Discs boot with the BIOS of their region, from the files the user chose, and
            // MSU-MD cartridges with any of them.
            static const Region regions[3] = {Region::JapanNtsc, Region::UsaNtsc, Region::EuropePal};
            for (size_t i = 0; i < 3; ++i) {
                std::vector<uint8_t> bios;
                if (settings.cdBios[i].empty() || !readFile(settings.cdBios[i], bios) || bios.size() != 0x20000)
                    bios.clear();
                app->console.setCdBios(regions[i], std::move(bios));
            }
        }
        useGameOptions(path);  // before loading: the region may be the game's own
        if (app->load(path, capturing ? nullptr : window)) {
            applySound();  // now it is known whether it is a disc
            if (settings.achievements && !capturing) achievements.loadGame(app->console, app->achievementsHash);
            settings.addRecent(path);
            if (!game.own.empty()) ui->notify(trf("Opciones propias de %s", fileName(path).c_str()));
            if (!app->console.loadNotice().empty()) ui->notify(app->console.loadNotice());
            else if (app->console.hasMsuMd()) ui->notify(tr("Música en CD (MSU-MD)"));
            rewind.clear();
            setRewindLimits();
            runAheadFailed = false;
        } else {
            useGameOptions(app->romLoaded ? app->romPath : std::string());
            // A missing BIOS already says what to do; anything else is the file's fault.
            const bool bios = app->lastError.find("BIOS") != std::string::npos;
            if (!capturing)
                ui->showError(bios ? app->lastError
                                   : trf("No se pudo abrir %s:\n%s\n\nplumsg abre cartuchos de Mega Drive, 32X, Master System y "
                                         "Game Gear y discos de Mega CD. Si el archivo es uno de ellos, puede estar "
                                         "incompleto o dañado: prueba a copiarlo otra vez.",
                                         fileName(path).c_str(), app->lastError.c_str()));
            if (bios) ui->openBios();  // the game is fine: it waits for its BIOS
            else settings.removeRecent(path);
        }
        save();
    };
    auto setFullscreen = [&] { SDL_SetWindowFullscreen(window, settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); };
    if (settings.fullscreen) setFullscreen();
    if (!cmd.rom.empty()) {
        load(cmd.rom);
        applyCommandLine();  // over the game's profile too
        if (cmd.slot >= 0 && app->romLoaded && !hardcore()) ui->notify(app->loadState(cmd.slot));
        roland->allNotesOff();
        if (!cmd.record.empty()) {
            const bool gif = cmd.record.size() > 4 && cmd.record.compare(cmd.record.size() - 4, 4, ".gif") == 0;
            startVideo(gif ? GameRecording::Format::Gif : GameRecording::Format::Avi, cmd.record);
        }
    }
    bool openStateMenu = cmd.stateMenu;
    if (!cmd.library.empty()) settings.libraryFolders = cmd.library;
    if (cmd.libraryList) settings.libraryList = true;
    if (!cmd.library.empty() || (cmd.rom.empty() && !capturing && settings.libraryAtStart && !settings.libraryFolders.empty()))
        ui->toggleLibrary();
    if (cmd.bios) ui->openBios();
    if (cmd.achievements) ui->openAchievements();
    if (cmd.editor) {
        loadSoundfont();
        editor.useSoundfont(*roland, settings.reverb, settings.chorus, settings.equalizer);
        editor.open(app->romLoaded ? app->romPath : std::string());
        if (!cmd.track.empty()) editor.openTrack(cmd.track);
        if (cmd.seek >= 0 && editor.trackLoaded()) {
            editor.seekTrack(cmd.seek);
            editor.togglePlay();
        }
    }
    if (cmd.score) {
        ui->openScore();
        applySound();
    }
    int loops = 0;
    bool downloadCovers = cmd.downloadCovers;
    bool coverSearch = cmd.coverSearch;
    bool sizedForMenu = false;

    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    Uint64 next = SDL_GetPerformanceCounter();
    bool running = true;
    bool paused = false;
    bool slowMotion = false;
    double musicSpeed = 1.0;  // the Partitura's speed
    bool orbiting = false;  // dragging the pseudo-3D camera with the mouse
    bool padMenuWasOpen = false;  // the states menu, the library or the file browser, last frame
    int framesRun = 0;
    // The FPS counter's measures: the frames run in this turn of the loop and the time
    // spent running them, and when the last turn ended.
    FpsCounter fpsCounter;
    int turnFrames = 0;
    Uint64 turnEmu = 0;
    double turnTarget = 0;
    Uint64 lastTurnEnd = next;
    auto timed = [&](auto&& run) {
        const Uint64 start = SDL_GetPerformanceCounter();
        run();
        turnEmu += SDL_GetPerformanceCounter() - start;
    };
    RenderOptions& cam = settings.render;
    auto centreCamera = [&] {
        const RenderOptions defaults;
        cam.tilt = defaults.tilt;
        cam.yaw = defaults.yaw;
        cam.zoom = defaults.zoom;
        save();
    };
    auto saveState = [&](int slot) {
        if (!app->romLoaded) return;
        ui->notify(app->saveState(slot));
        std::error_code ec;
        if (achievements.gameLoaded()) writeFile(app->progressPath(slot), achievements.saveProgress());
        else std::filesystem::remove(std::filesystem::u8path(app->progressPath(slot)), ec);
    };
    auto loadState = [&](int slot) {
        if (!app->romLoaded) return;
        if (hardcore()) {
            ui->notify(tr("En modo hardcore no se pueden cargar estados"));
            return;
        }
        bool loaded = false;
        ui->notify(app->loadState(slot, &loaded));
        roland->allNotesOff();  // what was playing is not what the state plays
        std::vector<uint8_t> progress;
        if (loaded && !readFile(app->progressPath(slot), progress)) progress.clear();
        if (loaded) achievements.loadProgress(progress);
    };
    // The console's reset button; the achievements start over with it.
    auto resetConsole = [&] {
        app->console.reset();
        achievements.reset();
    };
    auto toggleSlowMotion = [&] {
        slowMotion = !slowMotion;
        ui->notify(slowMotion ? trf("Cámara lenta (%d%%)", settings.slowMotion) : std::string(tr("Velocidad normal")));
    };
    auto setProfile = [&](bool on) {
        if (!app->romLoaded || on == game.has("profile_3d")) return;
        if (on) {
            game.own.insert("profile_3d");
            ui->notify(trf("El 3D se guarda ahora solo para %s", fileName(app->romPath).c_str()));
        } else {
            game.own.erase("profile_3d");
            copy3D(general.render, settings.render);
            ui->notify(tr("El juego vuelve a usar el 3D general"));
        }
        save();
    };
    auto setWideProfile = [&](bool on) {
        if (!app->romLoaded || on == game.has("widescreen")) return;
        if (on) {
            game.own.insert("widescreen");
            ui->notify(trf("La pantalla ancha se guarda ahora solo para %s", fileName(app->romPath).c_str()));
        } else {
            game.own.erase("widescreen");
            findGameOption("widescreen")->copy(general, settings);
            ui->notify(tr("El juego vuelve a usar la pantalla ancha general"));
        }
        save();
    };
    // The Opciones de este juego and Trucos windows can be for a game that is not loaded
    // (opened from the library): then they work on these copies, saved to its files.
    std::string optionsGame, cheatsGame;
    std::string windowsRom;  // the game loaded when they were opened
    GameOverrides otherOptions;
    std::vector<Cheat> otherCheats;
    bool cheatsSms = false;  // the cheats' game is a Master System or Game Gear one
    auto openGameOptions = [&](const std::string& path) {
        optionsGame = path;
        if (path != app->romPath || !app->romLoaded) otherOptions = app->readOverrides(path, general, input->config());
    };
    auto openCheats = [&](const std::string& path) {
        cheatsGame = path;
        const ConsoleSystem system = detectSystem(path);
        cheatsSms = system == ConsoleSystem::MasterSystem || system == ConsoleSystem::GameGear;
        if (path != app->romPath || !app->romLoaded) otherCheats = app->readCheats(path);
    };
    if (cmd.cheats && app->romLoaded) {
        openCheats(app->romPath);
        ui->openCheats();
    }
    if (cmd.gameOptions && app->romLoaded) {
        openGameOptions(app->romPath);
        ui->openGameOptions(cmd.gameOptions == 2);
    }
    // Netplay. The session talks through the socket, so it goes first on the way out.
    std::unique_ptr<UdpTransport> netSocket;
    std::unique_ptr<NetplaySession> net;
    NetplaySession::Phase netPhase = NetplaySession::Phase::Ended;
    // Connected (or connecting): what would make the two consoles differ is off.
    auto netPlaying = [&] { return net && net->phase() != NetplaySession::Phase::Waiting; };
    auto stopNet = [&](std::string why, bool error) {  // a copy: it may be the session's, gone below
        if (!net) return;
        net->end();
        net.reset();
        netSocket.reset();
        if (app->romLoaded) app->applyCheats();
        app->console.setPadType(1, settings.padType[1]);
        rewind.clear();
        if (error) ui->showError(why);
        else if (!why.empty()) ui->notify(why);
    };
    auto startNet = [&](bool host) {
        stopNet("", false);
        if (!app->romLoaded) {
            ui->showError(tr("Abre primero el juego: los dos tenéis que tener abierto el mismo."));
            return;
        }
        auto socket = std::make_unique<UdpTransport>();
        if (!socket->open(host ? settings.netPort : 0) ||
            (!host && !socket->setPeer(settings.netAddress, settings.netPort))) {
            ui->showError(socket->error());
            return;
        }
        NetplaySession::Config config;
        config.inputDelay = settings.netInputDelay;
        net = std::make_unique<NetplaySession>(*socket, host ? NetplaySession::Role::Host : NetplaySession::Role::Guest,
                                               config);
        netSocket = std::move(socket);
        netPhase = net->phase();
        // ROM cheats change the cartridge, which the savestate the guest gets does not carry.
        app->console.setCheats({});
        // The guest plays on pad 2: it has to be plugged in (the guest gets the host's types).
        if (app->console.padType(1) == PadType::None) app->console.setPadType(1, PadType::SixButton);
        paused = slowMotion = false;
        ui->notify(host ? trf("Esperando al otro jugador en el puerto %d", settings.netPort)
                        : trf("Conectando con %s", settings.netAddress.c_str()));
    };
    // Parar emulación: back to the start screen (or the library) without closing plumsg.
    auto stop = [&] {
        if (!app->romLoaded) return;
        stopNet(tr("Has salido de la partida en red"), false);
        stopRecording();
        stopVideo();
        save();  // the game's own options, while they are still its
        app->unload(capturing ? nullptr : window);
        achievements.unloadGame();
        useGameOptions(std::string());
        roland->allNotesOff();
        rewind.clear();
        paused = slowMotion = false;
        save();
        ui->notify(tr("Emulación parada"));
        if (!settings.libraryFolders.empty() && !ui->libraryOpen()) ui->toggleLibrary();
    };
    auto selectSlot = [&](int delta) {
        settings.stateSlot = (settings.stateSlot + delta + kSlots) % kSlots;
        const SlotInfo& s = app->slots[static_cast<size_t>(settings.stateSlot)];
        ui->notify(s.used ? trf("Ranura %d (%s)", settings.stateSlot, s.when.c_str()) : trf("Ranura %d (vacía)", settings.stateSlot));
        save();
    };
    if (app->romLoaded && (cmd.netHost || !cmd.netJoin.empty())) {
        if (!cmd.netJoin.empty()) settings.netAddress = cmd.netJoin;
        startNet(cmd.netHost);
        ui->openNetplay();  // shows how the connection goes
    }
    while (running) {
        const Uint64 turnStart = SDL_GetPerformanceCounter();
        turnFrames = 0;
        turnEmu = 0;
        turnTarget = 0;
        // Only the picture changes with them, so they can follow the menus at any frame.
        app->console.vdp().setUnlimitedSprites(settings.unlimitedSprites);
        if (MasterSystem* sms = app->console.masterSystem()) sms->vdp().setUnlimitedSprites(settings.unlimitedSprites);
        app->console.vdp().setWidescreen(settings.wideLeft, settings.wideRight);
        // The overclock does change the game: in a netplay game the speed is the host's,
        // which came in the state both started from, and hardcore mode keeps the real one.
        if (!netPlaying()) app->console.setOverclock(hardcore() ? 100 : settings.overclock);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input->handleEvent(e);  // first, so it sees every trigger pull and release
            if (ui->handleEvent(e)) continue;
            switch (e.type) {
                case SDL_QUIT: running = false; break;
                case SDL_DROPFILE:
                    stopNet(tr("Has salido de la partida en red"), false);
                    load(e.drop.file);
                    SDL_free(e.drop.file);
                    break;
                default: break;
            }
            // Full screen works over the library and the windows too: no game key or text box wants these.
            if (e.type == SDL_KEYDOWN && !e.key.repeat &&
                (e.key.keysym.scancode == SDL_SCANCODE_F11 ||
                 (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT)))) {
                settings.fullscreen = !settings.fullscreen;
                setFullscreen();
                save();
                continue;
            }
            if (ui->keyboardBusy() && e.type == SDL_KEYDOWN) continue;

            switch (input->hotkeyFor(e)) {
                case Hotkey::Toggle3D:
                    settings.render.pseudo3d = !settings.render.pseudo3d;
                    ui->notify(settings.render.pseudo3d ? tr("Efecto 3D activado") : tr("Efecto 3D desactivado"));
                    save();
                    break;
                case Hotkey::SaveState: saveState(settings.stateSlot); break;
                case Hotkey::LoadState:
                    if (!netPlaying()) loadState(settings.stateSlot);
                    break;
                case Hotkey::PrevSlot: selectSlot(-1); break;
                case Hotkey::NextSlot: selectSlot(1); break;
                case Hotkey::StateMenu: ui->toggleStateMenu(); break;
                case Hotkey::Library: ui->toggleLibrary(); break;
                case Hotkey::Stop: stop(); break;
                case Hotkey::SlowMotion:
                    if (app->romLoaded && !netPlaying() && !hardcore()) toggleSlowMotion();
                    break;
                case Hotkey::FpsCounter:
                    settings.fpsCounter = !settings.fpsCounter;
                    ui->notify(settings.fpsCounter ? tr("Contador de FPS activado") : tr("Contador de FPS desactivado"));
                    save();
                    break;
                default: break;  // rewind and fast forward work while held, below
            }
            // The pseudo-3D camera: drag to turn it, wheel to zoom, double click to centre it.
            if (cam.pseudo3d) {
                // Without tilting the camera only the zoom changes.
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && !ui->mouseBusy() &&
                    cam.tiltCamera) {
                    if (e.button.clicks >= 2) centreCamera();
                    else orbiting = true;
                } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && orbiting) {
                    orbiting = false;
                    save();
                } else if (e.type == SDL_MOUSEMOTION && orbiting) {
                    cam.yaw = std::clamp(cam.yaw + 0.4f * static_cast<float>(e.motion.xrel), -kMaxYaw, kMaxYaw);
                    cam.tilt = std::clamp(cam.tilt + 0.4f * static_cast<float>(e.motion.yrel), kMinTilt, kMaxTilt);
                } else if (e.type == SDL_MOUSEWHEEL && !ui->mouseBusy()) {
                    cam.zoom = std::clamp(cam.zoom * std::pow(1.1f, static_cast<float>(e.wheel.y)), kMinZoom, kMaxZoom);
                    save();
                } else if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
                    centreCamera();
                }
            }
            if (e.type != SDL_KEYDOWN || e.key.repeat) continue;
            const bool ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;
            switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_ESCAPE:
                    settings.showMenu = !settings.showMenu;
                    save();
                    break;
                case SDL_SCANCODE_O:
                    if (ctrl) ui->openRom();
                    break;
                case SDL_SCANCODE_L:
                    if (ctrl) ui->toggleLibrary();
                    break;
                case SDL_SCANCODE_R:
                    if (ctrl) toggleVideo(GameRecording::Format::Avi);
                    break;
                case SDL_SCANCODE_G:
                    if (ctrl) toggleVideo(GameRecording::Format::Gif);
                    break;
                case SDL_SCANCODE_F1:
                    if (!netPlaying() && app->romLoaded) resetConsole();
                    break;
                case SDL_SCANCODE_F2:
                    settings.view = renderers[(viewIndex() + 1) % renderers.size()]->name();
                    ui->notify(trf("Vista: %s", settings.view.c_str()));
                    save();
                    break;
                case SDL_SCANCODE_P:
                    if (!netPlaying()) paused = !paused;
                    break;
                case SDL_SCANCODE_M:
                    settings.mute = !settings.mute;
                    ui->notify(settings.mute ? tr("Sonido silenciado") : tr("Sonido activado"));
                    save();
                    break;
                default: break;
            }
        }

        // For screenshots, open it once a few frames have run.
        if (openStateMenu && (framesRun >= cmd.frames - 1 || !capturing)) {
            ui->toggleStateMenu();
            openStateMenu = false;
        }
        // Another game loaded: the windows that were on the old one now edit its files.
        if (app->romPath != windowsRom) {
            windowsRom = app->romPath;
            if (!optionsGame.empty()) openGameOptions(optionsGame);
            if (!cheatsGame.empty()) openCheats(cheatsGame);
        }
        uiState.romLoaded = app->romLoaded;
        uiState.romPath = app->romPath;
        uiState.paused = paused;
        uiState.slowMotion = slowMotion;
        uiState.fps = &fpsCounter;
        uiState.hasProfile = game.has("profile_3d");
        uiState.hasWideProfile = game.has("widescreen");
        uiState.gameName = app->romLoaded ? fileName(app->romPath) : std::string();
        uiState.slots = app->slots;
        const bool cheatsLoaded = app->romLoaded && cheatsGame == app->romPath;
        uiState.cheats = cheatsGame.empty() ? nullptr : cheatsLoaded ? &app->cheats : &otherCheats;
        uiState.cheatsGame = cheatsGame.empty() ? std::string() : fileName(cheatsGame);
        uiState.cheatsPath = cheatsGame;
        uiState.cheatsSms = cheatsSms;
        const bool optionsLoaded = app->romLoaded && optionsGame == app->romPath;
        uiState.gameOptions.game = optionsGame.empty() ? std::string() : fileName(optionsGame);
        uiState.gameOptions.values = optionsLoaded ? &settings : &otherOptions.values;
        uiState.gameOptions.own = optionsLoaded ? &game.own : &otherOptions.own;
        uiState.gameOptions.general = &general;
        uiState.gameOptions.controls = optionsLoaded ? &game.controls : &otherOptions.controls;
        uiState.gameOptions.loaded = optionsLoaded;
        uiState.editor.loadedGame = app->romLoaded ? app->romPath : std::string();
        uiState.editor.recording = recording;
        uiState.editor.recordedSeconds = recorder.secondsAt(app->console.masterClocks());
        uiState.editor.soundfontReady = rolandFontOk;
        // The Partitura: the editor's track while it has one open, else the game's notes.
        ScoreMusic scoreMusic;
        const bool trackScore = editor.isOpen() && editor.trackLoaded();
        if (trackScore) {
            scoreMusic.notes = &editor.trackScore();
            scoreMusic.clockRate = editor.trackClock();
            scoreMusic.now = editor.trackPosition();
            scoreMusic.length = editor.trackLength();
            scoreMusic.playing = editor.playing();
            scoreMusic.track = true;
            scoreMusic.sound = &editor.synth();
            scoreMusic.silenced = &editor.silencedInstruments();
        } else if (app->romLoaded && app->console.rolandSound()) {
            scoreMusic.notes = &roland->score();
            scoreMusic.now = static_cast<double>(roland->scoreClock()) / RolandSound::kScoreClock;
            scoreMusic.playing = !paused;
            scoreMusic.canSpeed = !hardcore() && !net;
            scoreMusic.sound = roland.get();
            scoreMusic.silenced = &gameSilenced;
        }
        uiState.score = scoreMusic.notes ? &scoreMusic : nullptr;
        uiState.disc = app->romLoaded && app->console.megaCd();
        uiState.hardcore = hardcore();
        uiState.recording = !video.active() ? 0 : video.format() == GameRecording::Format::Gif ? 1 : 2;
        uiState.recordingSeconds = static_cast<int>(video.seconds());
        uiState.netSession = net != nullptr;
        uiState.netPlaying = netPlaying();
        uiState.netProgress = -1;
        if (net) {
            const bool host = net->role() == NetplaySession::Role::Host;
            switch (net->phase()) {
                case NetplaySession::Phase::Waiting:
                    uiState.netStatus = trf("Esperando al otro jugador en el puerto %d. Mientras, puedes seguir jugando: "
                                            "la partida empezará donde estés.", settings.netPort);
                    break;
                case NetplaySession::Phase::Connecting:
                    uiState.netStatus = trf("Conectando con %s...", settings.netAddress.c_str());
                    break;
                case NetplaySession::Phase::Syncing:
                    uiState.netStatus = host ? tr("Enviando la partida al otro jugador...") : tr("Recibiendo la partida del anfitrión...");
                    uiState.netProgress = net->syncProgress();
                    break;
                default:
                    uiState.netStatus = trf("Jugando en red como jugador %d.\nPing: %d ms. Retardo de los mandos: %d fotogramas. "
                                            "Vuelta atrás: %d fotogramas.", host ? 1 : 2, static_cast<int>(net->pingMs()),
                                            static_cast<int>(net->config().inputDelay), static_cast<int>(net->lastRollback()));
                    if (net->desyncFrame() >= 0)
                        uiState.netStatus += "\n" + trf("Las dos consolas se han descompasado (fotograma %d): lo que veis "
                                                        "ya no es lo mismo. Salid y volved a empezar.",
                                                        static_cast<int>(net->desyncFrame()));
                    break;
            }
        }
        const UiActions act = ui->frame(uiState);
        if (!act.load.empty()) {
            stopNet(tr("Has salido de la partida en red"), false);
            load(act.load);
        }
        if (act.netHost) startNet(true);
        if (act.netJoin) startNet(false);
        if (act.netLeave) stopNet(tr("Has salido de la partida en red"), false);
        const bool netBusy = netPlaying();
        if (act.reset && !netBusy && app->romLoaded) resetConsole();
        if (act.stop) stop();
        if (act.record == 1) toggleVideo(GameRecording::Format::Gif);
        if (act.record == 2) toggleVideo(GameRecording::Format::Avi);
        if (act.openRecordings) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(paths.videos), ec);
            SDL_OpenURL(("file:///" + paths.videos).c_str());
        }
        if (act.togglePause && !netBusy) paused = !paused;
        musicSpeed = act.musicSpeed;
        if (trackScore) {
            if (act.score.togglePlay) editor.togglePlay();
            if (act.score.stop) editor.stopTrack();
            if (act.score.seek >= 0) editor.seekTrack(act.score.seek);
            if (act.score.toggleSound) editor.toggleInstrument(act.score.toggleSound);
        } else if (act.score.toggleSound) {
            if (!gameSilenced.erase(act.score.toggleSound)) gameSilenced.insert(act.score.toggleSound);
            roland->setListening(0, gameSilenced);
            applySound();
        }
        if (act.quit) running = false;
        if (act.fullscreenChanged) setFullscreen();
        if (act.windowScale) applyWindowScale(window, act.windowScale, ui->menuHeight(), settings);
        if (act.saveState >= 0) saveState(act.saveState);
        if (act.loadState >= 0 && !netBusy) loadState(act.loadState);
        // A menu that owned the pad has closed (A loaded a state, Y saved one, B closed it):
        // that button is still down, and the game must not take it as a press of its own.
        const bool padMenuOpen = ui->stateMenuOpen() || ui->libraryOpen() || ui->browserOpen();
        if (padMenuWasOpen && !padMenuOpen) input->ignoreHeld();
        padMenuWasOpen = padMenuOpen;
        if (act.consoleChanged && !netBusy) {
            // A new region needs a fresh start, like switching a real console's region.
            applyConsole();
            if (app->romLoaded) resetConsole();
            rewind.clear();
            setRewindLimits();
        }
        if (act.toggleSlowMotion && !hardcore()) toggleSlowMotion();
        if (act.achievementsChanged) applyAchievements();
        // Hardcore switched on or off: the cheats, rewinding and slow motion follow.
        if (hardcore() != app->noCheats) {
            app->noCheats = hardcore();
            if (!net) app->applyCheats();
            if (app->noCheats) {
                slowMotion = false;
                rewind.clear();
            }
        }
        if (act.rewindChanged) setRewindLimits();
        if (act.profile >= 0) setProfile(act.profile == 1);
        if (act.wideProfile >= 0) setWideProfile(act.wideProfile == 1);
        if (!act.cheatsFor.empty()) openCheats(act.cheatsFor);
        if (act.cheatsChanged && cheatsLoaded) {
            if (!net) app->applyCheats();
            app->saveCheats();
        } else if (act.cheatsChanged && !cheatsGame.empty()) {
            app->writeCheats(cheatsGame, otherCheats);
        }
        if (!act.optionsFor.empty()) openGameOptions(act.optionsFor);
        if (act.gameOptionsChanged && optionsLoaded) {
            applyControls();
            save();
            if (act.rewindChanged) setRewindLimits();
        } else if (act.gameOptionsChanged && !optionsGame.empty()) {
            app->writeOverrides(optionsGame, otherOptions);
        }
        if (act.settingsChanged) save();
        if (act.equalizerChanged) {
            roland->setEqualizer(settings.equalizer);
            editor.useEqualizer(settings.equalizer);
        }
        if (act.soundChanged) applySound();
        if (act.soundChanged && app->romLoaded) applyInstruments(app->romPath);
        if (act.soundChanged && editor.isOpen() && loadSoundfont())
            editor.useSoundfont(*roland, settings.reverb, settings.chorus, settings.equalizer);
        if (act.openEditor) {
            loadSoundfont();
            editor.useSoundfont(*roland, settings.reverb, settings.chorus, settings.equalizer);
            editor.open(act.editorFor);
        }
        if (act.startRecording && app->romLoaded && !recording) {
            recording = true;
            app->console.setVgmWriter(&recorder);
            paused = false;
        }
        if (act.stopRecording || (recording && !editor.isOpen())) stopRecording();
        if (act.assignmentsChanged && app->romLoaded) applyInstruments(app->romPath);
        // The first frame tells us how tall the menu bar is; grow the window to fit it.
        if (!sizedForMenu && !settings.fullscreen) {
            applyWindowScale(window, settings.windowScale, ui->menuHeight(), settings);
            sizedForMenu = true;
        }

        const double frameSeconds = 1.0 / app->console.refreshRate();
        if (cam.pseudo3d && cam.tiltCamera) {
            // The right stick turns the camera at up to 90 degrees a second.
            float sx, sy;
            input->rightStick(&sx, &sy);
            const float step = 90.0f * static_cast<float>(frameSeconds);
            cam.yaw = std::clamp(cam.yaw + sx * step, -kMaxYaw, kMaxYaw);
            cam.tilt = std::clamp(cam.tilt + sy * step, kMinTilt, kMaxTilt);
        }

        uiState.speed.clear();
        bool ranFrame = false;
        if (net) {
            net->poll(app->console, SDL_GetTicks());
            const NetplaySession::Phase phase = net->phase();
            const bool host = net->role() == NetplaySession::Role::Host;
            if (phase != netPhase) {
                if (phase == NetplaySession::Phase::Syncing) {
                    rewind.clear();
                    if (!host) app->hostsBattery = true;  // the host's state is coming
                }
                if (phase == NetplaySession::Phase::Playing) {
                    ui->notify(trf("¡Conectados! Eres el jugador %d", host ? 1 : 2));
                    framesRun = 0;  // --cuadros counts the frames played together
                }
                netPhase = phase;
            }
            if (phase == NetplaySession::Phase::Ended) stopNet(net->endReason(), true);
        }
        if (app->romLoaded && netPlaying()) {
            // A netplay game never waits for menus: the other player is still playing.
            if (net->phase() == NetplaySession::Phase::Playing) {
                const bool keyboard = !ui->keyboardBusy();
                if (!net->shouldWait()) {
                    app->console.setSampleRate(audio->rate());
                    // The menus that own the pad do not stop a netplay game, but their presses are not the game's.
                    bool advanced = false;
                    timed([&] { advanced = net->advance(app->console, padMenuOpen ? PadState{} : input->poll(0, keyboard)); });
                    turnTarget = app->console.refreshRate();
                    if (advanced) {
                        achievements.doFrame();
                        ranFrame = true;
                        ++turnFrames;
                        ++framesRun;
                        audio->push(app->console.audio().samples(), settings.volume, settings.mute || capturing);
                        video.addAudio(app->console.audio().samples().data(), app->console.audio().samples().size());
                        app->console.audio().clearSamples();
                    }
                }
                uiState.speed = net->desyncFrame() >= 0 ? std::string(tr("Red: ¡descompasados!"))
                                                        : trf("Red %d ms", static_cast<int>(net->pingMs()));
            } else {
                uiState.speed = net->phase() == NetplaySession::Phase::Syncing ? tr("Red: preparando la partida")
                                                                               : tr("Red: conectando");
            }
            pacer.reset();
        }
        // The instrument editor is playing: the game waits, and its track is heard.
        else if (editor.playing()) {
            std::vector<int16_t> samples;
            // At another speed the track plays more or less of itself in the same time:
            // the synthesizer's notes keep their pitch, the chips' sound goes higher or lower.
            editor.play(frameSeconds * musicSpeed, audio->rate() / musicSpeed, samples);
            audio->push(samples, settings.volume, settings.mute || capturing);
            pacer.reset();
        }
        // The game waits while the states menu, the library or the file browser is open.
        else if (app->romLoaded && !paused && !ui->stateMenuOpen() && !ui->libraryOpen() && !ui->browserOpen()) {
            const bool keyboard = !ui->keyboardBusy();
            const bool fast = input->held(Hotkey::FastForward, keyboard);
            const bool canRewind = settings.rewindSeconds > 0 && !hardcore();
            if (canRewind && input->held(Hotkey::Rewind, keyboard)) {
                // Back one frame per tick: load the newest snapshot and run that frame
                // again (silently, without the pads) to have its picture on screen.
                std::vector<uint8_t> snapshot;
                std::string error;
                if (rewind.pop(snapshot) && app->console.loadState(snapshot, &error)) {
                    roland->allNotesOff();
                    app->console.setPads(PadState{}, PadState{});
                    app->console.runFrame();
                    app->console.audio().clearSamples();
                    uiState.speed = tr("<< Rebobinando");
                } else {
                    uiState.speed = tr("<< No se puede rebobinar más");
                }
                pacer.reset();
            } else {
                // Fast forward and slow motion run more or fewer frames per tick; the
                // sound follows, higher or lower, because each frame's samples are made
                // for 1/speed of a tick.
                const double scoreSpeed = hardcore() ? 1.0 : musicSpeed;  // the Partitura's x0.5 and x2
                const double speed = fast ? settings.fastForward : slowMotion ? settings.slowMotion / 100.0 : scoreSpeed;
                if (fast) uiState.speed = ">> x" + std::to_string(settings.fastForward);
                else if (slowMotion) uiState.speed = trf("Cámara lenta %d%%", settings.slowMotion);
                else if (scoreSpeed != 1.0) uiState.speed = scoreSpeed < 1 ? "x0.5" : "x2";
                // Run-ahead: the last frame of the tick is followed by a few more with the
                // same pads, whose picture is shown, and the console goes back. Not in fast
                // forward, where a frame or two less of lag is not felt.
                const int ahead = fast || runAheadFailed ? 0 : settings.runAhead;
                turnTarget = app->console.refreshRate() * speed;
                for (int n = pacer.framesForTick(speed); n > 0; --n) {
                    if (canRewind) {
                        std::vector<uint8_t> snapshot;
                        if (app->console.saveState(snapshot, true)) rewind.push(std::move(snapshot));
                    }
                    app->console.setPads(input->poll(0, keyboard), input->poll(1, keyboard));
                    app->console.setSampleRate(audio->rate() / speed);
                    // The picture shown is run-ahead's, so these need not be drawn.
                    app->console.setDrawing(ahead == 0);
                    timed([&] { app->console.runFrame(); });
                    achievements.doFrame();
                    ranFrame = true;
                    ++framesRun;
                    ++turnFrames;
                    audio->push(app->console.audio().samples(), settings.volume, settings.mute || capturing);
                    video.addAudio(app->console.audio().samples().data(), app->console.audio().samples().size());
                    app->console.audio().clearSamples();
                    bool aheadFailed = false;
                    if (ahead > 0 && n == 1) timed([&] { aheadFailed = !app->console.runAhead(ahead); });
                    if (aheadFailed) {
                        runAheadFailed = true;
                        ui->notify(tr("Run-ahead desactivado: no se pudo volver al fotograma actual."));
                    }
                }
                app->console.setDrawing(true);
            }
        }

        // RetroAchievements: its answers, and what it has to say.
        if (!ranFrame) achievements.idle();
        achievements.poll();
        for (const Achievements::Notice& n : achievements.takeNotices()) {
            if (n.kind == Achievements::Notice::Kind::Reset && app->romLoaded) {
                resetConsole();
                rewind.clear();
            }
            ui->showAchievement(n);
        }
        if (achievements.loggedIn() && achievements.token() != settings.raToken) {
            settings.raToken = achievements.token();
            save();
        }

        int w, h;
        SDL_GL_GetDrawableSize(window, &w, &h);
        gl::Viewport(0, 0, w, h);
        gl::ClearColor(0, 0, 0, 1);
        gl::Clear(gl::COLOR_BUFFER_BIT);
        // The game fills the window below the menu bar; OpenGL counts rows from the bottom.
        const Viewport gameArea{0, 0, w, std::max(1, h - ui->menuHeight())};
        Renderer& view = settings.render.pseudo3d ? *pseudo3d : *renderers[viewIndex()];
        view.setOptions(settings.render);
        if (app->romLoaded) view.draw(app->console.frame(), gameArea);  // stopped: the start screen
        // A recording takes the game's picture before the menus are drawn over it.
        if (video.active() && app->romLoaded) {
            if (recordsConsole()) {
                const VideoFrameMd& f = app->console.frame();
                video.addFrame(f.rgba.data(), f.width, f.height, VideoFrameMd::kMaxWidth);
            } else {
                screen.resize(static_cast<size_t>(gameArea.width) * gameArea.height);
                gl::ReadPixels(0, 0, gameArea.width, gameArea.height, gl::BGRA, gl::UNSIGNED_INT_8_8_8_8_REV,
                               screen.data());
                video.addFrame(screen.data() + static_cast<size_t>(gameArea.height - 1) * gameArea.width,
                               gameArea.width, gameArea.height,
                               -static_cast<ptrdiff_t>(gameArea.width));  // OpenGL's rows go bottom up
            }
            if (video.failed()) stopVideo();
        }
        ui->render();
        ++loops;
        if (downloadCovers && ui->library().pending() == 0 && loops > 5) {
            ui->library().downloadMissing();
            downloadCovers = false;
            loops = 0;
        }
        if (coverSearch && ui->libraryOpen() && ui->library().pending() == 0 && loops > 3) {
            ui->openCoverSearch(0);
            coverSearch = false;
            loops = 0;
        }
        // A screenshot waits a few frames for the window and the popups to settle, and a
        // library one for its covers.
        const bool libraryReady =
            loops > 3 && (!ui->libraryOpen() || (!downloadCovers && !coverSearch && ui->library().pending() == 0 && loops > 20)) &&
            !ui->coverSearchBusy() && !ui->cheatsBusy() && !ui->libraryInfoBusy();
        if (capturing && libraryReady &&
            (framesRun >= cmd.frames || !app->romLoaded || ui->libraryOpen() ||
             (ui->stateMenuOpen() && framesRun >= cmd.frames - 1))) {
            const bool ok = captureWindow(window, cmd.capture);
            std::printf("%s %s\n", ok ? "captura guardada en" : "no se pudo guardar", cmd.capture.c_str());
            running = false;
        }
        SDL_GL_SwapWindow(window);
        {
            const Uint64 end = SDL_GetPerformanceCounter();
            fpsCounter.tick(static_cast<double>(end - lastTurnEnd) / freq, turnFrames, static_cast<double>(turnEmu) / freq,
                            static_cast<double>(end - turnStart) / freq,
                            turnTarget > 0 ? turnTarget : app->console.refreshRate());
            lastTurnEnd = end;
        }
        if (capturing) continue;  // as fast as possible

        // Pace to the console's refresh rate; resync if we fell far behind.
        next += static_cast<Uint64>(frameSeconds * freq);
        Uint64 now = SDL_GetPerformanceCounter();
        if (now < next) {
            SDL_Delay(static_cast<Uint32>((next - now) * 1000 / freq));
        } else if (now - next > static_cast<Uint64>(freq / 4)) {
            next = now;
        }
    }

    if (net) net->end();
    net.reset();
    netSocket.reset();
    stopRecording();
    stopVideo();
    app->saveBattery();
    save();
    for (auto& s : app->slots)
        if (s.texture) gl::DeleteTextures(1, &s.texture);
    audio.reset();
    ui.reset();
    renderers.clear();
    pseudo3d.reset();
    input.reset();
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
