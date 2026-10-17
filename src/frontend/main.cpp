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
#include "../render/gl_renderers.h"
#include "audio.h"
#include "input.h"
#include "miniz.h"
#include "settings.h"
#include "ui.h"

using namespace plumsg;

namespace {

constexpr int kSlots = 10;

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
    std::string states;  // folder for savestates, ending in a separator
};

Paths findPaths() {
    std::string base;
    if (char* b = SDL_GetBasePath()) {
        base = b;
        SDL_free(b);
        const std::string portable = base + "plumsg.ini";
        if (SDL_RWops* f = SDL_RWFromFile(portable.c_str(), "rb")) {
            SDL_RWclose(f);
            return {portable, base + "states/"};
        }
    }
    if (char* pref = SDL_GetPrefPath("plumsg", "plumsg")) {
        const std::string dir = pref;
        SDL_free(pref);
        return {dir + "plumsg.ini", dir + "states/"};
    }
    return {base + "plumsg.ini", base + "states/"};
}

struct App {
    MegaDrive console;
    bool romLoaded = false;
    std::string romPath;
    std::string statesDir;
    std::vector<SlotInfo> slots = std::vector<SlotInfo>(kSlots);

    std::string savePath() const { return withoutExtension(romPath) + ".sav"; }
    std::string statePath(int slot) const {
        return statesDir + withoutExtension(fileName(romPath)) + ".s" + std::to_string(slot);
    }

    void saveBattery() {
        if (romLoaded && console.cartridge().hasSaveData()) console.cartridge().writeSave(savePath());
    }

    bool load(const std::string& path, SDL_Window* window) {
        saveBattery();
        std::string error;
        if (!console.loadRom(path, &error)) {
            if (window) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "plumsg", error.c_str(), window);
            else std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        romPath = path;
        console.cartridge().loadSave(savePath());
        romLoaded = true;
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
};

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
        else c.rom = a;
    }
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    const CommandLine cmd = parse(argc, argv);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const Paths paths = findPaths();
    const bool capturing = !cmd.capture.empty();
    auto input = std::make_unique<Input>(InputConfig::loadOrCreate(paths.ini));
    Settings settings = Settings::load(paths.ini);
    // A screenshot run never touches the user's settings.
    if (cmd.pseudo3d >= 0) settings.render.pseudo3d = cmd.pseudo3d == 1;
    if (!cmd.view.empty()) settings.view = cmd.view;
    if (cmd.tilt > -1000) settings.render.tilt = cmd.tilt;
    if (cmd.yaw > -1000) settings.render.yaw = cmd.yaw;
    if (capturing) settings.fullscreen = false;

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
    auto audio = std::make_unique<Audio>();

    auto applyConsole = [&] {
        app->console.setRegion(settings.region);
        app->console.setPadType(0, settings.padType[0]);
        app->console.setPadType(1, settings.padType[1]);
    };
    applyConsole();
    auto save = [&] {
        if (!capturing) settings.save(paths.ini, input->config());
    };
    auto load = [&](const std::string& path) {
        if (app->load(path, capturing ? nullptr : window)) {
            settings.addRecent(path);
        } else {
            settings.removeRecent(path);
        }
        save();
    };
    auto setFullscreen = [&] { SDL_SetWindowFullscreen(window, settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); };
    if (settings.fullscreen) setFullscreen();
    if (!cmd.rom.empty()) load(cmd.rom);
    bool sizedForMenu = false;

    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    Uint64 next = SDL_GetPerformanceCounter();
    bool running = true;
    bool paused = false;
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
    auto selectSlot = [&](int delta) {
        settings.stateSlot = (settings.stateSlot + delta + kSlots) % kSlots;
        const SlotInfo& s = app->slots[static_cast<size_t>(settings.stateSlot)];
        ui->notify("Ranura " + std::to_string(settings.stateSlot) + (s.used ? " (" + s.when + ")" : " (vacía)"));
        save();
    };
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (ui->handleEvent(e)) continue;
            input->handleEvent(e);
            switch (e.type) {
                case SDL_QUIT: running = false; break;
                case SDL_DROPFILE:
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
                case Hotkey::LoadState: loadState(settings.stateSlot); break;
                case Hotkey::PrevSlot: selectSlot(-1); break;
                case Hotkey::NextSlot: selectSlot(1); break;
                default: break;
            }
            // The pseudo-3D camera: drag to turn it, wheel to zoom, double click to centre it.
            if (cam.pseudo3d) {
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && !ui->mouseBusy()) {
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
                case SDL_SCANCODE_F1: app->console.reset(); break;
                case SDL_SCANCODE_F2:
                    settings.view = renderers[(viewIndex() + 1) % renderers.size()]->name();
                    ui->notify(std::string("Vista: ") + settings.view);
                    save();
                    break;
                case SDL_SCANCODE_P: paused = !paused; break;
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

        uiState.romLoaded = app->romLoaded;
        uiState.paused = paused;
        uiState.slots = app->slots;
        const UiActions act = ui->frame(uiState);
        if (!act.load.empty()) load(act.load);
        if (act.reset) app->console.reset();
        if (act.togglePause) paused = !paused;
        if (act.quit) running = false;
        if (act.fullscreenChanged) setFullscreen();
        if (act.windowScale) applyWindowScale(window, act.windowScale, ui->menuHeight());
        if (act.saveState >= 0) saveState(act.saveState);
        if (act.loadState >= 0) loadState(act.loadState);
        if (act.consoleChanged) {
            // A new region needs a fresh start, like switching a real console's region.
            applyConsole();
            if (app->romLoaded) app->console.reset();
        }
        if (act.settingsChanged) save();
        // The first frame tells us how tall the menu bar is; grow the window to fit it.
        if (!sizedForMenu && !settings.fullscreen) {
            applyWindowScale(window, settings.windowScale, ui->menuHeight());
            sizedForMenu = true;
        }

        const double frameSeconds = 1.0 / app->console.refreshRate();
        if (cam.pseudo3d) {
            // The right stick turns the camera at up to 90 degrees a second.
            float sx, sy;
            input->rightStick(&sx, &sy);
            const float step = 90.0f * static_cast<float>(frameSeconds);
            cam.yaw = std::clamp(cam.yaw + sx * step, -kMaxYaw, kMaxYaw);
            cam.tilt = std::clamp(cam.tilt + sy * step, kMinTilt, kMaxTilt);
        }

        if (app->romLoaded && !paused) {
            const bool keyboard = !ui->keyboardBusy();
            app->console.setPads(input->poll(0, keyboard), input->poll(1, keyboard));
            app->console.setSampleRate(audio->rate());
            app->console.runFrame();
            ++framesRun;
            audio->push(app->console.audio().samples(), settings.volume, settings.mute || capturing);
            app->console.audio().clearSamples();
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
        if (capturing && (framesRun >= cmd.frames || !app->romLoaded)) {
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
