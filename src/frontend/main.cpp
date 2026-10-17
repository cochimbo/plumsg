#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../core/megadrive.h"
#include "../core/netplay.h"
#include "../core/rewind.h"
#include "../core/system.h"
#include "../render/gl_renderers.h"
#include "../render/profile3d.h"
#include "../net/udp.h"
#include "audio.h"
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
};

Paths findPaths() {
    std::string base;
    if (char* b = SDL_GetBasePath()) {
        base = b;
        SDL_free(b);
        const std::string portable = base + "plumsg.ini";
        if (SDL_RWops* f = SDL_RWFromFile(portable.c_str(), "rb")) {
            SDL_RWclose(f);
            return {portable, base + "states/", base + "perfiles/", base + "trucos/", base + "caratulas/"};
        }
    }
    if (char* pref = SDL_GetPrefPath("plumsg", "plumsg")) {
        const std::string dir = pref;
        SDL_free(pref);
        return {dir + "plumsg.ini", dir + "states/", dir + "perfiles/", dir + "trucos/", dir + "caratulas/"};
    }
    return {base + "plumsg.ini", base + "states/", base + "perfiles/", base + "trucos/", base + "caratulas/"};
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

    std::string savePath() const { return withoutExtension(romPath) + ".sav"; }
    std::string statePath(int slot) const {
        return statesDir + withoutExtension(fileName(romPath)) + ".s" + std::to_string(slot);
    }
    std::string profilePath() const { return profilesDir + withoutExtension(fileName(romPath)) + ".ini"; }
    std::string cheatsPath() const { return cheatsDir + withoutExtension(fileName(romPath)) + ".txt"; }

    // Hands the console the codes that are on and valid.
    void applyCheats() {
        std::vector<CheatPatch> patches;
        for (const Cheat& c : cheats) {
            CheatPatch p;
            if (c.enabled && decodeCheat(c.code, &p, nullptr)) patches.push_back(p);
        }
        console.setCheats(patches);
    }

    void loadCheats() {
        cheats.clear();
        std::vector<uint8_t> data;
        if (readFile(cheatsPath(), data)) cheats = parseCheats(std::string(data.begin(), data.end()));
        applyCheats();
    }

    void saveCheats() {
        std::error_code ec;
        if (cheats.empty()) {
            std::filesystem::remove(std::filesystem::u8path(cheatsPath()), ec);
            return;
        }
        std::filesystem::create_directories(std::filesystem::u8path(cheatsDir), ec);
        const std::string text = formatCheats(cheats);
        writeFile(cheatsPath(), std::vector<uint8_t>(text.begin(), text.end()));
    }

    // Cartridge SRAM, or the Mega CD's backup RAM, next to the game.
    void saveBattery() {
        if (!romLoaded || hostsBattery) return;
        if (MegaCd* cd = console.megaCd()) {
            if (cd->backupRamChanged() &&
                writeFile(savePath(), std::vector<uint8_t>(cd->backupRam().begin(), cd->backupRam().end())))
                cd->clearBackupRamChanged();
            return;
        }
        if (console.cartridge().hasSaveData()) console.cartridge().writeSave(savePath());
    }

    void loadBattery() {
        MegaCd* cd = console.megaCd();
        if (!cd) {
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
        hostsBattery = false;
        lastError.clear();
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
            return "No se pudo guardar el estado en " + statePath(slot);
        refreshSlot(slot);
        return "Estado guardado en la ranura " + std::to_string(slot);
    }

    std::string loadState(int slot) {
        std::vector<uint8_t> data;
        if (!readFile(statePath(slot), data)) return "La ranura " + std::to_string(slot) + " está vacía";
        std::string error;
        if (!console.loadState(data, &error)) return "No se pudo cargar el estado: " + error;
        return "Estado cargado de la ranura " + std::to_string(slot);
    }
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

// Window size for a scale factor: 4:3 at 224 lines per scale step, plus the menu bar.
void applyWindowScale(SDL_Window* window, int scale, int menuHeight) {
    SDL_SetWindowSize(window, 299 * scale, 224 * scale + menuHeight);
}

struct CommandLine {
    std::string rom;
    std::string capture;  // --captura file.png: take a screenshot and quit
    int frames = 120;     // --cuadros N: frames to run before the screenshot
    int pseudo3d = -1;    // --3d / --2d override the setting
    std::string view;     // --vista name
    float tilt = -1000, yaw = -1000;
    bool stateMenu = false;  // --menu-estados: start with the states menu open
    std::vector<std::string> library;  // --biblioteca folder (repeatable): start with the library of those folders open
    int filter = -1;         // --filtro ninguno|scanlines|crt
    bool downloadCovers = false;  // --descargar-caratulas: fetch the library's missing covers
    bool bios = false;            // --bios: start with the Mega CD BIOS window open
    // For launchers (LaunchBox, Playnite, frontends of arcade cabinets). These last only
    // for this session: the settings file keeps what it had.
    int fullscreen = -1;          // --pantalla-completa / --ventana
    int region = -1;              // --region auto|jp|us|eu (a Region)
    int slot = -1;                // --estado N: load the game's state N once it is open
    bool netHost = false;         // --alojar: host a netplay game with the open game
    std::string netJoin;          // --unirse address: join that host
    bool help = false;            // --ayuda
    std::vector<std::string> unknown;  // options plumsg does not know
};

const char* kHelp =
    "plumsg [juego] [opciones]\n\n"
    "  juego                  cartucho (.md .gen .bin .smd .32x .zip) o disco (.cue .chd .iso)\n"
    "  --pantalla-completa    abrir a pantalla completa (o --fullscreen)\n"
    "  --ventana              abrir en ventana (o --windowed)\n"
    "  --3d / --2d            con o sin la vista pseudo-3D\n"
    "  --filtro f             ninguno, scanlines o crt\n"
    "  --region r             auto, jp, us o eu\n"
    "  --estado N             cargar el estado guardado N (0-9) del juego\n"
    "  --alojar               alojar una partida en red con el juego (puerto del menú Red)\n"
    "  --unirse direccion     unirse a la partida en red de ese anfitrión (IP:puerto)\n"
    "  --biblioteca carpeta   abrir la biblioteca de esa carpeta (se puede repetir)\n"
    "  --bios                 abrir la ventana de las BIOS del Mega CD\n"
    "  --captura f.png        guardar una captura tras --cuadros N fotogramas y salir\n\n"
    "La pantalla completa y la región dadas aquí valen solo para esta vez.";

CommandLine parse(int argc, char** argv) {
    CommandLine c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--captura") c.capture = next();
        else if (a == "--cuadros") c.frames = std::max(1, std::atoi(next().c_str()));
        else if (a == "--3d") c.pseudo3d = 1;
        else if (a == "--2d") c.pseudo3d = 0;
        else if (a == "--vista") c.view = next();
        else if (a == "--inclinacion") c.tilt = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--giro") c.yaw = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--menu-estados") c.stateMenu = true;
        else if (a == "--biblioteca") c.library.push_back(next());
        else if (a == "--descargar-caratulas") c.downloadCovers = true;
        else if (a == "--bios") c.bios = true;
        else if (a == "--filtro") {
            const std::string f = next();
            c.filter = f == "crt" ? 2 : f == "scanlines" ? 1 : 0;
        }
        else if (a == "--pantalla-completa" || a == "--fullscreen") c.fullscreen = 1;
        else if (a == "--ventana" || a == "--windowed") c.fullscreen = 0;
        else if (a == "--region") {
            const std::string r = next();
            if (r == "auto") c.region = static_cast<int>(Region::Auto);
            else if (r == "jp" || r == "japon") c.region = static_cast<int>(Region::JapanNtsc);
            else if (r == "us" || r == "usa") c.region = static_cast<int>(Region::UsaNtsc);
            else if (r == "eu" || r == "europa" || r == "pal") c.region = static_cast<int>(Region::EuropePal);
            else c.unknown.push_back(a + " " + r);
        } else if (a == "--estado" || a == "--load-state") {
            const std::string n = next();
            char* end = nullptr;
            const long slot = std::strtol(n.c_str(), &end, 10);
            if (!n.empty() && *end == 0 && slot >= 0 && slot < kSlots) c.slot = static_cast<int>(slot);
            else c.unknown.push_back(a + " " + n);
        } else if (a == "--alojar" || a == "--host") c.netHost = true;
        else if (a == "--unirse" || a == "--join") {
            c.netJoin = next();
            if (c.netJoin.empty()) c.unknown.push_back(a);
        } else if (a == "--ayuda" || a == "--help" || a == "-h" || a == "/?") c.help = true;
        else if (a.size() > 1 && a[0] == '-') c.unknown.push_back(a);
        else c.rom = a;
    }
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    const CommandLine cmd = parse(argc, argv);
    if (cmd.help || !cmd.unknown.empty()) {
        // A window build on Windows has no console, so this goes to a message box too.
        std::string text = kHelp;
        if (!cmd.unknown.empty()) {
            text = "Opción desconocida: " + cmd.unknown.front() + "\n\n" + text;
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
    const Paths paths = findPaths();
    const bool capturing = !cmd.capture.empty();
    auto input = std::make_unique<Input>(InputConfig::loadOrCreate(paths.ini));
    Settings settings = Settings::load(paths.ini);
    // A screenshot run never touches the user's settings.
    auto applyCommandLine = [&] {
        if (cmd.pseudo3d >= 0) settings.render.pseudo3d = cmd.pseudo3d == 1;
        if (!cmd.view.empty()) settings.view = cmd.view;
        if (cmd.tilt > -1000) settings.render.tilt = cmd.tilt;
        if (cmd.yaw > -1000) settings.render.yaw = cmd.yaw;
        if (cmd.filter >= 0) settings.render.filter = static_cast<Filter2D>(cmd.filter);
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
    // The pseudo-3D options of [video]. While a game with its own profile is loaded,
    // settings.render holds the profile's and these wait here.
    RenderOptions global3D = settings.render;
    bool hasProfile = false;

    // OpenGL 3.3 core everywhere; macOS only hands out core contexts marked forward compatible.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_Window* window = SDL_CreateWindow("plumsg", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          299 * settings.windowScale, 224 * settings.windowScale,
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
            "plumsg necesita una tarjeta gráfica con OpenGL 3.3 y sus drivers actualizados.\n\n" +
            std::string(context ? "Falta la función " + std::string(missing) : SDL_GetError());
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
    auto app = std::make_unique<App>();
    app->statesDir = paths.states;
    app->profilesDir = paths.profiles;
    app->cheatsDir = paths.cheats;
    ui->library().setCoversFolder(paths.covers);
    auto audio = std::make_unique<Audio>();

    auto applyConsole = [&] {
        app->console.setRegion(settings.region);
        app->console.setPadType(0, settings.padType[0]);
        app->console.setPadType(1, settings.padType[1]);
    };
    applyConsole();
    auto save = [&] {
        if (capturing) return;
        if (!hasProfile) {
            copy3D(settings.render, global3D);
            toSave(settings).save(paths.ini, input->config());
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(app->profilesDir), ec);
        const std::string text = formatProfile(settings.render, fileName(app->romPath));
        writeFile(app->profilePath(), std::vector<uint8_t>(text.begin(), text.end()));
        Settings general = toSave(settings);
        copy3D(global3D, general.render);
        general.save(paths.ini, input->config());
    };
    RewindBuffer rewind;
    FramePacer pacer;
    auto setRewindLimits = [&] {
        rewind.setLimits(static_cast<size_t>(settings.rewindSeconds * app->console.refreshRate()), kRewindBytes);
    };
    setRewindLimits();
    // A newly loaded game takes its own 3D profile if it has one, else the general options.
    auto loadProfile = [&] {
        copy3D(global3D, settings.render);
        hasProfile = false;
        std::vector<uint8_t> data;
        if (!readFile(app->profilePath(), data)) return;
        RenderOptions r = settings.render;
        if (!parseProfile(std::string(data.begin(), data.end()), r)) return;
        copy3D(r, settings.render);
        hasProfile = true;
        ui->notify("Perfil 3D de " + fileName(app->romPath));
    };
    auto load = [&](const std::string& path) {
        const ConsoleSystem system = detectSystem(path);
        if (system == ConsoleSystem::MegaCD) {
            // Discs boot with the BIOS of their region, from the files the user chose.
            static const Region regions[3] = {Region::JapanNtsc, Region::UsaNtsc, Region::EuropePal};
            for (size_t i = 0; i < 3; ++i) {
                std::vector<uint8_t> bios;
                if (settings.cdBios[i].empty() || !readFile(settings.cdBios[i], bios) || bios.size() != 0x20000)
                    bios.clear();
                app->console.setCdBios(regions[i], std::move(bios));
            }
        }
        if (app->load(path, capturing ? nullptr : window)) {
            settings.addRecent(path);
            loadProfile();
            rewind.clear();
            setRewindLimits();
        } else {
            if (!capturing) ui->showError(app->lastError);
            if (app->lastError.find("BIOS") != std::string::npos) ui->openBios();  // the game is fine: it waits for its BIOS
            else settings.removeRecent(path);
        }
        save();
    };
    auto setFullscreen = [&] { SDL_SetWindowFullscreen(window, settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); };
    if (settings.fullscreen) setFullscreen();
    if (!cmd.rom.empty()) {
        load(cmd.rom);
        applyCommandLine();  // over the game's profile too
        if (cmd.slot >= 0 && app->romLoaded) ui->notify(app->loadState(cmd.slot));
    }
    bool openStateMenu = cmd.stateMenu;
    if (!cmd.library.empty()) settings.libraryFolders = cmd.library;
    if (!cmd.library.empty() || (cmd.rom.empty() && !capturing && settings.libraryAtStart && !settings.libraryFolders.empty()))
        ui->toggleLibrary();
    if (cmd.bios) ui->openBios();
    int loops = 0;
    bool downloadCovers = cmd.downloadCovers;
    bool sizedForMenu = false;

    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    Uint64 next = SDL_GetPerformanceCounter();
    bool running = true;
    bool paused = false;
    bool slowMotion = false;
    bool orbiting = false;  // dragging the pseudo-3D camera with the mouse
    int framesRun = 0;
    RenderOptions& cam = settings.render;
    auto centreCamera = [&] {
        const RenderOptions defaults;
        cam.tilt = defaults.tilt;
        cam.yaw = defaults.yaw;
        cam.zoom = defaults.zoom;
        save();
    };
    auto saveState = [&](int slot) {
        if (app->romLoaded) ui->notify(app->saveState(slot));
    };
    auto loadState = [&](int slot) {
        if (app->romLoaded) ui->notify(app->loadState(slot));
    };
    auto toggleSlowMotion = [&] {
        slowMotion = !slowMotion;
        ui->notify(slowMotion ? "Cámara lenta (" + std::to_string(settings.slowMotion) + "%)" : "Velocidad normal");
    };
    auto setProfile = [&](bool on) {
        if (!app->romLoaded || on == hasProfile) return;
        if (on) {
            hasProfile = true;
            ui->notify("El 3D se guarda ahora solo para " + fileName(app->romPath));
        } else {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(app->profilePath()), ec);
            hasProfile = false;
            copy3D(global3D, settings.render);
            ui->notify("El juego vuelve a usar el 3D general");
        }
        save();
    };
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
        rewind.clear();
        if (error) ui->showError(why);
        else if (!why.empty()) ui->notify(why);
    };
    auto startNet = [&](bool host) {
        stopNet("", false);
        if (!app->romLoaded) {
            ui->showError("Abre primero el juego: los dos tenéis que tener abierto el mismo.");
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
        paused = slowMotion = false;
        ui->notify(host ? "Esperando al otro jugador en el puerto " + std::to_string(settings.netPort)
                        : "Conectando con " + settings.netAddress);
    };
    auto selectSlot = [&](int delta) {
        settings.stateSlot = (settings.stateSlot + delta + kSlots) % kSlots;
        const SlotInfo& s = app->slots[static_cast<size_t>(settings.stateSlot)];
        ui->notify("Ranura " + std::to_string(settings.stateSlot) + (s.used ? " (" + s.when + ")" : " (vacía)"));
        save();
    };
    if (app->romLoaded && (cmd.netHost || !cmd.netJoin.empty())) {
        if (!cmd.netJoin.empty()) settings.netAddress = cmd.netJoin;
        startNet(cmd.netHost);
        ui->openNetplay();  // shows how the connection goes
    }
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input->handleEvent(e);  // first, so it sees every trigger pull and release
            if (ui->handleEvent(e)) continue;
            switch (e.type) {
                case SDL_QUIT: running = false; break;
                case SDL_DROPFILE:
                    stopNet("Has salido de la partida en red", false);
                    load(e.drop.file);
                    SDL_free(e.drop.file);
                    break;
                default: break;
            }
            if (ui->keyboardBusy() && e.type == SDL_KEYDOWN) continue;

            switch (input->hotkeyFor(e)) {
                case Hotkey::Toggle3D:
                    settings.render.pseudo3d = !settings.render.pseudo3d;
                    ui->notify(settings.render.pseudo3d ? "Efecto 3D activado" : "Efecto 3D desactivado");
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
                case Hotkey::SlowMotion:
                    if (app->romLoaded && !netPlaying()) toggleSlowMotion();
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
            const bool alt = (e.key.keysym.mod & KMOD_ALT) != 0;
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
                case SDL_SCANCODE_F1:
                    if (!netPlaying()) app->console.reset();
                    break;
                case SDL_SCANCODE_F2:
                    settings.view = renderers[(viewIndex() + 1) % renderers.size()]->name();
                    ui->notify(std::string("Vista: ") + settings.view);
                    save();
                    break;
                case SDL_SCANCODE_P:
                    if (!netPlaying()) paused = !paused;
                    break;
                case SDL_SCANCODE_M:
                    settings.mute = !settings.mute;
                    ui->notify(settings.mute ? "Sonido silenciado" : "Sonido activado");
                    save();
                    break;
                case SDL_SCANCODE_RETURN:
                    if (!alt) break;
                    [[fallthrough]];
                case SDL_SCANCODE_F11:
                    settings.fullscreen = !settings.fullscreen;
                    setFullscreen();
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
        uiState.romLoaded = app->romLoaded;
        uiState.paused = paused;
        uiState.slowMotion = slowMotion;
        uiState.hasProfile = hasProfile;
        uiState.gameName = app->romLoaded ? fileName(app->romPath) : std::string();
        uiState.slots = app->slots;
        uiState.cheats = &app->cheats;
        uiState.netSession = net != nullptr;
        uiState.netPlaying = netPlaying();
        uiState.netProgress = -1;
        if (net) {
            const bool host = net->role() == NetplaySession::Role::Host;
            switch (net->phase()) {
                case NetplaySession::Phase::Waiting:
                    uiState.netStatus = "Esperando al otro jugador en el puerto " + std::to_string(settings.netPort) +
                                        ". Mientras, puedes seguir jugando: la partida empezará donde estés.";
                    break;
                case NetplaySession::Phase::Connecting:
                    uiState.netStatus = "Conectando con " + settings.netAddress + "...";
                    break;
                case NetplaySession::Phase::Syncing:
                    uiState.netStatus = host ? "Enviando la partida al otro jugador..." : "Recibiendo la partida del anfitrión...";
                    uiState.netProgress = net->syncProgress();
                    break;
                default:
                    uiState.netStatus = std::string("Jugando en red como jugador ") + (host ? "1" : "2") + ".\nPing: " +
                                        std::to_string(net->pingMs()) + " ms. Retardo de los mandos: " +
                                        std::to_string(net->config().inputDelay) + " fotogramas. Vuelta atrás: " +
                                        std::to_string(net->lastRollback()) + " fotogramas.";
                    if (net->desyncFrame() >= 0)
                        uiState.netStatus += "\nLas dos consolas se han descompasado (fotograma " +
                                             std::to_string(net->desyncFrame()) +
                                             "): lo que veis ya no es lo mismo. Salid y volved a empezar.";
                    break;
            }
        }
        const UiActions act = ui->frame(uiState);
        if (!act.load.empty()) {
            stopNet("Has salido de la partida en red", false);
            load(act.load);
        }
        if (act.netHost) startNet(true);
        if (act.netJoin) startNet(false);
        if (act.netLeave) stopNet("Has salido de la partida en red", false);
        const bool netBusy = netPlaying();
        if (act.reset && !netBusy) app->console.reset();
        if (act.togglePause && !netBusy) paused = !paused;
        if (act.quit) running = false;
        if (act.fullscreenChanged) setFullscreen();
        if (act.windowScale) applyWindowScale(window, act.windowScale, ui->menuHeight());
        if (act.saveState >= 0) saveState(act.saveState);
        if (act.loadState >= 0 && !netBusy) loadState(act.loadState);
        if (act.consoleChanged && !netBusy) {
            // A new region needs a fresh start, like switching a real console's region.
            applyConsole();
            if (app->romLoaded) app->console.reset();
            rewind.clear();
            setRewindLimits();
        }
        if (act.toggleSlowMotion) toggleSlowMotion();
        if (act.rewindChanged) setRewindLimits();
        if (act.profile >= 0) setProfile(act.profile == 1);
        if (act.cheatsChanged && app->romLoaded) {
            if (!net) app->applyCheats();
            app->saveCheats();
        }
        if (act.settingsChanged) save();
        // The first frame tells us how tall the menu bar is; grow the window to fit it.
        if (!sizedForMenu && !settings.fullscreen) {
            applyWindowScale(window, settings.windowScale, ui->menuHeight());
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
                    ui->notify(std::string("¡Conectados! Eres el jugador ") + (host ? "1" : "2"));
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
                    if (net->advance(app->console, input->poll(0, keyboard))) {
                        ++framesRun;
                        audio->push(app->console.audio().samples(), settings.volume, settings.mute || capturing);
                        app->console.audio().clearSamples();
                    }
                }
                uiState.speed = net->desyncFrame() >= 0 ? "Red: ¡descompasados!"
                                                        : "Red " + std::to_string(net->pingMs()) + " ms";
            } else {
                uiState.speed = net->phase() == NetplaySession::Phase::Syncing ? "Red: preparando la partida"
                                                                               : "Red: conectando";
            }
            pacer.reset();
        }
        // The game waits while the states menu, the library or the file browser is open.
        else if (app->romLoaded && !paused && !ui->stateMenuOpen() && !ui->libraryOpen() && !ui->browserOpen()) {
            const bool keyboard = !ui->keyboardBusy();
            const bool fast = input->held(Hotkey::FastForward, keyboard);
            if (settings.rewindSeconds > 0 && input->held(Hotkey::Rewind, keyboard)) {
                // Back one frame per tick: load the newest snapshot and run that frame
                // again (silently, without the pads) to have its picture on screen.
                std::vector<uint8_t> snapshot;
                std::string error;
                if (rewind.pop(snapshot) && app->console.loadState(snapshot, &error)) {
                    app->console.setPads(PadState{}, PadState{});
                    app->console.runFrame();
                    app->console.audio().clearSamples();
                    uiState.speed = "<< Rebobinando";
                } else {
                    uiState.speed = "<< No se puede rebobinar más";
                }
                pacer.reset();
            } else {
                // Fast forward and slow motion run more or fewer frames per tick; the
                // sound follows, higher or lower, because each frame's samples are made
                // for 1/speed of a tick.
                const double speed = fast ? settings.fastForward : slowMotion ? settings.slowMotion / 100.0 : 1.0;
                if (fast) uiState.speed = ">> x" + std::to_string(settings.fastForward);
                else if (slowMotion) uiState.speed = "Cámara lenta " + std::to_string(settings.slowMotion) + "%";
                for (int n = pacer.framesForTick(speed); n > 0; --n) {
                    if (settings.rewindSeconds > 0) {
                        std::vector<uint8_t> snapshot;
                        if (app->console.saveState(snapshot, true)) rewind.push(std::move(snapshot));
                    }
                    app->console.setPads(input->poll(0, keyboard), input->poll(1, keyboard));
                    app->console.setSampleRate(audio->rate() / speed);
                    app->console.runFrame();
                    ++framesRun;
                    audio->push(app->console.audio().samples(), settings.volume, settings.mute || capturing);
                    app->console.audio().clearSamples();
                }
            }
        }

        int w, h;
        SDL_GL_GetDrawableSize(window, &w, &h);
        gl::Viewport(0, 0, w, h);
        gl::ClearColor(0, 0, 0, 1);
        gl::Clear(gl::COLOR_BUFFER_BIT);
        // The game fills the window below the menu bar; OpenGL counts rows from the bottom.
        const Viewport game{0, 0, w, std::max(1, h - ui->menuHeight())};
        Renderer& view = settings.render.pseudo3d ? *pseudo3d : *renderers[viewIndex()];
        view.setOptions(settings.render);
        view.draw(app->console.frame(), game);
        ui->render();
        ++loops;
        if (downloadCovers && ui->library().pending() == 0 && loops > 5) {
            ui->library().downloadMissing();
            downloadCovers = false;
            loops = 0;
        }
        // A screenshot waits a few frames for the window and the popups to settle, and a
        // library one for its covers.
        const bool libraryReady =
            loops > 3 && (!ui->libraryOpen() || (!downloadCovers && ui->library().pending() == 0 && loops > 20));
        if (capturing && libraryReady &&
            (framesRun >= cmd.frames || !app->romLoaded || ui->libraryOpen() ||
             (ui->stateMenuOpen() && framesRun >= cmd.frames - 1))) {
            const bool ok = captureWindow(window, cmd.capture);
            std::printf("%s %s\n", ok ? "captura guardada en" : "no se pudo guardar", cmd.capture.c_str());
            running = false;
        }
        SDL_GL_SwapWindow(window);
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
