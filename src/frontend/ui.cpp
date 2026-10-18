#include "ui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "game_options.h"
#include "library_names.h"
#include "icon_png.h"
#include "logo_png.h"
#include "../core/system.h"
#include "../net/udp.h"
#include "../render/gl.h"

#define STBI_NO_STDIO
#include "stb_image.h"

namespace plumsg {

namespace {

constexpr int kRows = InputConfig::kButtons + InputConfig::kHotkeys;
// While a game runs the mouse pointer goes away once it has been still this long.
constexpr Uint32 kCursorIdleMs = 2000;
constexpr const char* kRowNames[kRows] = {"Arriba", "Abajo", "Izquierda", "Derecha", "A", "B", "C", "Start",
                                          "X", "Y", "Z", "Mode", "Efecto 3D", "Guardar estado", "Cargar estado",
                                          "Ranura anterior", "Ranura siguiente", "Menú de estados",
                                          "Rebobinar (mantener)", "Avance rápido (mantener)", "Cámara lenta",
                                          "Biblioteca", "Parar emulación"};
static_assert(InputConfig::kButtons == 12 && InputConfig::kHotkeys == 11, "update kRowNames");

std::vector<SDL_Scancode>& keysOf(InputConfig& c, int row, int player) {
    if (row >= InputConfig::kButtons) return c.hotkeyKeys[row - InputConfig::kButtons];
    return player == 0 ? c.keys[row] : c.keys2[row];
}

std::vector<SDL_GameControllerButton>& padOf(InputConfig& c, int row) {
    return row < InputConfig::kButtons ? c.pad[row] : c.hotkeyPad[row - InputConfig::kButtons];
}

std::string fileName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A code of several joined with '+' shown as its first and how many more ("AJ0T-CA9Y +5").
std::string shortCode(const std::string& code) {
    const size_t plus = code.find('+');
    if (plus == std::string::npos) return code;
    size_t more = 0;
    for (char c : code) more += c == '+';
    return code.substr(0, plus) + " +" + std::to_string(more);
}

// What a code writes, one line per write, for its tooltip.
std::string describePatches(const std::vector<CheatPatch>& patches) {
    std::string out;
    char line[64];
    for (const CheatPatch& p : patches) {
        std::snprintf(line, sizeof line, "%s%s %06X = %0*X (%s)", out.empty() ? "" : "\n", p.address >= 0xE00000 ? "RAM" : "ROM",
                      static_cast<unsigned>(p.address), p.byte ? 2 : 4, static_cast<unsigned>(p.value),
                      p.byte ? "byte" : "palabra");
        out += line;
    }
    return out;
}

}  // namespace

Ui::Ui(SDL_Window* window, SDL_GLContext context, Settings& settings, Input& input)
    : window_(window), settings_(settings), input_(input) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window layout is not worth a second settings file
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;
#ifdef _WIN32
    // Segoe UI reads better than the built-in bitmap font; it covers Spanish accents too.
    const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (SDL_RWops* f = SDL_RWFromFile(font, "rb")) {
        SDL_RWclose(f);
        io.Fonts->AddFontFromFileTTF(font, 18.0f);
    }
#endif
    ImGui_ImplSDL2_InitForOpenGL(window_, context);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    loadLogo();
}

Ui::~Ui() {
    library_.clearTextures();
    if (logoTexture_) gl::DeleteTextures(1, &logoTexture_);
    if (iconTexture_) gl::DeleteTextures(1, &iconTexture_);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

// An RGBA texture from an embedded PNG; 0 if it cannot be decoded. With `window`, the
// image is also the window's icon.
static unsigned int textureFromPng(const unsigned char* png, size_t size, int* w, int* h, SDL_Window* window = nullptr) {
    int n = 0;
    stbi_uc* rgba = stbi_load_from_memory(png, static_cast<int>(size), w, h, &n, 4);
    if (!rgba) return 0;
    unsigned int texture = 0;
    gl::GenTextures(1, &texture);
    gl::BindTexture(gl::TEXTURE_2D, texture);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, *w, *h, 0, gl::RGBA, gl::UNSIGNED_BYTE, rgba);
    if (window) {
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(rgba, *w, *h, 32, *w * 4, SDL_PIXELFORMAT_RGBA32)) {
            SDL_SetWindowIcon(window, s);
            SDL_FreeSurface(s);
        }
    }
    stbi_image_free(rgba);
    return texture;
}

void Ui::loadLogo() {
    logoTexture_ = textureFromPng(kLogoPng, sizeof(kLogoPng), &logoWidth_, &logoHeight_);
    // The plum is also the window's icon (on Windows the .exe's icon already is).
    int w = 0, h = 0;
    iconTexture_ = textureFromPng(kIconPng, sizeof(kIconPng), &w, &h, window_);
}

bool Ui::handleEvent(const SDL_Event& e) {
    if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEWHEEL)
        lastMouseMove_ = SDL_GetTicks();
    if (capture_.active) {
        if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) capture_.active = false;
            else if (capture_.column < 2) finishCapture(e.key.keysym.scancode);
            return true;
        }
        if (e.type == SDL_KEYUP || e.type == SDL_TEXTINPUT) return true;
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            if (capture_.column == 2) finishCapture(e.cbutton.button);
            return true;
        }
        if (e.type == SDL_CONTROLLERBUTTONUP) return true;
        if (e.type == SDL_CONTROLLERAXISMOTION) {
            const SDL_GameControllerButton trigger = triggerPulled(e);
            if (capture_.column == 2 && trigger != SDL_CONTROLLER_BUTTON_INVALID) finishCapture(trigger);
            return true;
        }
    }
    if (browser_.isOpen() && browser_.handleEvent(e)) return true;
    if (stateMenu_.open && handleStateMenu(e)) return true;
    if (showLibrary_ && handleLibrary(e)) return true;
    ImGui_ImplSDL2_ProcessEvent(&e);
    return false;
}

void Ui::toggleStateMenu() {
    stateMenu_.open = !stateMenu_.open;
    stateMenu_.selected = settings_.stateSlot;
    stateMenu_.scroll = true;
}

// While the states menu is open it owns the pad and the keyboard: up and down pick a
// slot, A (or Enter) loads it, Y (or Space) saves to it, B (or Esc) closes the menu.
bool Ui::handleStateMenu(const SDL_Event& e) {
    if (e.type != SDL_KEYDOWN && e.type != SDL_CONTROLLERBUTTONDOWN && e.type != SDL_KEYUP &&
        e.type != SDL_CONTROLLERBUTTONUP && e.type != SDL_TEXTINPUT)
        return false;
    if (e.type != SDL_KEYDOWN && e.type != SDL_CONTROLLERBUTTONDOWN) return true;
    if (input_.hotkeyFor(e) == Hotkey::StateMenu) {
        stateMenu_.open = false;
        return true;
    }
    enum { None, Up, Down, Load, Save, Close } action = None;
    if (e.type == SDL_KEYDOWN) {
        switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_UP: action = Up; break;
            case SDL_SCANCODE_DOWN: action = Down; break;
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: action = Load; break;
            case SDL_SCANCODE_SPACE: action = Save; break;
            case SDL_SCANCODE_ESCAPE: action = Close; break;
            default: break;
        }
    } else {
        switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: action = Up; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: action = Down; break;
            case SDL_CONTROLLER_BUTTON_A: action = Load; break;
            case SDL_CONTROLLER_BUTTON_Y: action = Save; break;
            case SDL_CONTROLLER_BUTTON_B: action = Close; break;
            default: break;
        }
    }
    const int count = std::max(1, stateMenu_.slots);
    switch (action) {
        case Up: stateMenu_.selected = (stateMenu_.selected + count - 1) % count; stateMenu_.scroll = true; break;
        case Down: stateMenu_.selected = (stateMenu_.selected + 1) % count; stateMenu_.scroll = true; break;
        case Load: stateMenu_.load = stateMenu_.selected; stateMenu_.open = false; break;
        case Save: stateMenu_.save = stateMenu_.selected; stateMenu_.open = false; break;
        case Close: stateMenu_.open = false; break;
        case None: break;
    }
    if (action == Load || action == Save) {
        settings_.stateSlot = stateMenu_.selected;
        pendingSave_ = true;
    }
    return true;
}

bool Ui::keyboardBusy() const {
    return capture_.active || stateMenu_.open || showLibrary_ || browser_.isOpen() || !error_.empty() ||
           ImGui::GetIO().WantCaptureKeyboard;
}

bool Ui::mouseBusy() const { return ImGui::GetIO().WantCaptureMouse; }

void Ui::finishCapture(int code) {
    const bool game = capture_.game && gameControls_;
    InputConfig c = game ? *gameControls_ : input_.config();
    if (capture_.column == 2) {
        auto& list = padOf(c, capture_.row);
        const auto b = static_cast<SDL_GameControllerButton>(code);
        if (!capture_.append) list.clear();
        if (std::find(list.begin(), list.end(), b) == list.end()) list.push_back(b);
    } else {
        auto& list = keysOf(c, capture_.row, capture_.column);
        const auto k = static_cast<SDL_Scancode>(code);
        if (!capture_.append) list.clear();
        if (std::find(list.begin(), list.end(), k) == list.end()) list.push_back(k);
    }
    capture_.active = false;
    if (game) {
        *gameControls_ = std::move(c);
        pendingGameOptions_ = true;
    } else {
        input_.setConfig(std::move(c));
        pendingSave_ = true;
    }
}

void Ui::notify(std::string message) {
    toast_ = std::move(message);
    toastUntil_ = SDL_GetTicks() + 2500;
}

UiActions Ui::frame(const UiState& state) {
    UiActions out;
    out.settingsChanged = pendingSave_;
    pendingSave_ = false;
    out.gameOptionsChanged = pendingGameOptions_;
    pendingGameOptions_ = false;

    if (openRequested_) {
        openRequested_ = false;
        std::string path;
        // In full screen the system dialog can open behind the game's window and never
        // be seen, so there the built-in browser is used instead.
        const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
        browseFor_ = BrowseFor::Game;
        switch (fullscreen ? DialogResult::Unavailable : openFileDialog(window_, settings_.lastFolder, Pick::Game, &path)) {
            case DialogResult::Chosen: out.load = path; break;
            case DialogResult::Cancelled: break;
            case DialogResult::Unavailable: browser_.open(settings_.lastFolder); break;
        }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    drawMenuBar(state, out);
    if (showControls_) drawControls(out);
    if (showVideo_) drawVideo(state, out);
    if (showAbout_) drawAbout();
    if (showCheats_) drawCheats(state, out);
    if (showGameOptions_) drawGameOptions(state, out);
    library_.update();
    if (showLibrary_) drawLibrary(out);
    if (showCoverSearch_) drawCoverSearch();
    if (showBios_) drawBios(out);
    if (showNetplay_) drawNetplay(state, out);
    if (editor_ && editor_->isOpen()) {
        EditorRequests r;
        EditorView view = state.editor;
        view.drawSoundfont = [&] { drawSoundfontCombo(out, false); };  // the same list as Sonido's
        editor_->draw(view, r);
        if (r.chooseTrack) chooseTrack();
        if (!r.load.empty()) out.load = r.load;
        out.startRecording = r.startRecording;
        out.stopRecording = r.stopRecording;
        out.assignmentsChanged = r.assignmentsChanged;
    }
    stateMenu_.slots = static_cast<int>(state.slots.size());
    if (stateMenu_.load >= 0) out.loadState = stateMenu_.load;
    if (stateMenu_.save >= 0) out.saveState = stateMenu_.save;
    stateMenu_.load = stateMenu_.save = -1;
    if (stateMenu_.open) drawStateMenu(state);
    if (!error_.empty()) drawError();
    std::string picked;
    if (browser_.draw(&picked)) {
        switch (browseFor_) {
            case BrowseFor::Game: out.load = picked; break;
            case BrowseFor::LibraryFolder:
                if (std::find(settings_.libraryFolders.begin(), settings_.libraryFolders.end(), picked) ==
                    settings_.libraryFolders.end())
                    settings_.libraryFolders.push_back(picked);
                out.settingsChanged = true;
                rescanLibrary();
                break;
            case BrowseFor::Soundfont:
                settings_.soundfont = picked;
                out.settingsChanged = out.soundChanged = true;
                break;
            case BrowseFor::Track:
                if (editor_) editor_->openTrack(picked);
                break;
            case BrowseFor::CdBios: {
                settings_.cdBios[static_cast<size_t>(biosSlot_)] = picked;
                out.settingsChanged = true;
                break;
            }
        }
    }
    drawOverlays(state);
    hideIdleCursor(state);
    return out;
}

// The backend reads the cursor ImGui asks for at the start of the next frame and hides
// the system pointer when it is None.
void Ui::hideIdleCursor(const UiState& state) {
    const bool windowOpen = showControls_ || showVideo_ || showAbout_ || showCheats_ || showGameOptions_ || showLibrary_ ||
                            showCoverSearch_ || showBios_ || showNetplay_ || stateMenu_.open || browser_.isOpen() ||
                            (editor_ && editor_->isOpen()) ||
                            !error_.empty() || capture_.active ||
                            ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || ImGui::GetIO().WantCaptureMouse;
    const bool playing = state.romLoaded && !state.paused;
    if (playing && !windowOpen && SDL_GetTicks() - lastMouseMove_ >= kCursorIdleMs)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
}

void Ui::render() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Ui::drawMenuBar(const UiState& state, UiActions& out) {
    const ImGuiIO& io = ImGui::GetIO();
    const float barHeight = ImGui::GetFrameHeight();
    if (!settings_.showMenu) {
        // Hidden bar: reveal it while the mouse sits at the top edge or a menu is open.
        const bool atTop = io.MousePos.y >= 0 && io.MousePos.y < (peekMenu_ ? barHeight : 4.0f);
        peekMenu_ = atTop || (peekMenu_ && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    }
    menuHeight_ = settings_.showMenu ? static_cast<int>(barHeight) : 0;
    if (!settings_.showMenu && !peekMenu_) return;
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("Archivo")) {
        if (ImGui::MenuItem("Abrir ROM...", "Ctrl+O")) openRequested_ = true;
        if (ImGui::MenuItem("Biblioteca...", "F10", showLibrary_)) toggleLibrary();
        if (ImGui::BeginMenu("Recientes", !settings_.recent.empty())) {
            for (const auto& path : settings_.recent) {
                if (ImGui::MenuItem(fileName(path).c_str())) out.load = path;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Borrar lista")) {
                settings_.recent.clear();
                out.settingsChanged = true;
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Salir")) out.quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Juego")) {
        // In a netplay game these would make the two consoles go different.
        const bool solo = state.romLoaded && !state.netPlaying;
        if (ImGui::MenuItem("Pausa", "P", state.paused, solo)) out.togglePause = true;
        if (ImGui::MenuItem("Reiniciar", "F1", false, solo)) out.reset = true;
        if (ImGui::MenuItem("Parar emulación", "F12", false, state.romLoaded)) out.stop = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Cierra el juego (guarda su partida de cartucho) y vuelve a la\n"
                              "pantalla de inicio, o a la biblioteca si tienes carpetas en ella.");
        if (ImGui::MenuItem("Trucos...", nullptr, false, solo)) {
            out.cheatsFor = state.romPath;
            showCheats_ = true;
        }
        if (ImGui::MenuItem("Opciones de este juego...", nullptr, false, solo)) {
            out.optionsFor = state.romPath;
            showGameOptions_ = true;
        }
        ImGui::SeparatorText("Velocidad");
        if (ImGui::MenuItem("Cámara lenta", "F9", state.slowMotion, solo)) out.toggleSlowMotion = true;
        ImGui::TextDisabled("Rebobinar: mantén Retroceso (o el gatillo izquierdo)");
        ImGui::TextDisabled("Avance rápido: mantén Tab (o el gatillo derecho)");
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt("Segundos de rebobinado", &settings_.rewindSeconds, 0, Settings::kMaxRewindSeconds,
                         settings_.rewindSeconds ? "%d s" : "desactivado", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = out.rewindChanged = true;
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt("Avance rápido", &settings_.fastForward, 2, 10, "%dx", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt("Cámara lenta##velocidad", &settings_.slowMotion, 10, 90, "%d%%", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Estado")) {
        const std::string slot = " (ranura " + std::to_string(settings_.stateSlot) + ")";
        if (ImGui::MenuItem(("Guardar estado" + slot).c_str(), "F5", false, state.romLoaded))
            out.saveState = settings_.stateSlot;
        const bool used = settings_.stateSlot < static_cast<int>(state.slots.size()) &&
                          state.slots[static_cast<size_t>(settings_.stateSlot)].used;
        if (ImGui::MenuItem(("Cargar estado" + slot).c_str(), "F8", false, state.romLoaded && used && !state.netPlaying))
            out.loadState = settings_.stateSlot;
        if (ImGui::MenuItem("Menú de estados...", "F4")) toggleStateMenu();
        ImGui::SeparatorText("Ranura (F6 / F7)");
        for (int i = 0; i < static_cast<int>(state.slots.size()); ++i) {
            const SlotInfo& info = state.slots[static_cast<size_t>(i)];
            const std::string label = std::to_string(i) + "  " + (info.used ? info.when : std::string("vacía"));
            if (ImGui::MenuItem(label.c_str(), nullptr, settings_.stateSlot == i)) {
                settings_.stateSlot = i;
                out.settingsChanged = true;
            }
            if (info.texture && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Image(static_cast<ImTextureID>(info.texture),
                             ImVec2(static_cast<float>(info.width) * 1.5f, static_cast<float>(info.height) * 1.5f));
                ImGui::EndTooltip();
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Consola")) {
        static const char* regions[] = {"Automática (la de la ROM)", "Japón (NTSC)", "EE. UU. (NTSC)", "Europa (PAL)"};
        ImGui::BeginDisabled(state.netPlaying);  // the host's console decides in a netplay game
        ImGui::SeparatorText("Región");
        for (int i = 0; i < 4; ++i)
            if (ImGui::MenuItem(regions[i], nullptr, static_cast<int>(settings_.region) == i)) {
                settings_.region = static_cast<Region>(i);
                out.consoleChanged = out.settingsChanged = true;
            }
        for (int port = 0; port < 2; ++port) {
            ImGui::SeparatorText(port == 0 ? "Mando 1" : "Mando 2");
            ImGui::PushID(port);
            for (PadType t : {PadType::ThreeButton, PadType::SixButton})
                if (ImGui::MenuItem(t == PadType::ThreeButton ? "3 botones" : "6 botones", nullptr,
                                    settings_.padType[static_cast<size_t>(port)] == t)) {
                    settings_.padType[static_cast<size_t>(port)] = t;
                    out.consoleChanged = out.settingsChanged = true;
                }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("BIOS del Mega CD...", nullptr, showBios_)) showBios_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Red")) {
        if (ImGui::MenuItem("Jugar en red...", nullptr, showNetplay_)) showNetplay_ = true;
        if (ImGui::MenuItem("Salir de la partida en red", nullptr, false, state.netSession)) out.netLeave = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Sonido")) {
        drawSoundMenu(state, out);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Opciones")) {
        if (ImGui::MenuItem("Controles...")) showControls_ = true;
        if (ImGui::MenuItem("Vídeo...")) showVideo_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Efecto 3D", nullptr, settings_.render.pseudo3d)) {
            settings_.render.pseudo3d = !settings_.render.pseudo3d;
            out.settingsChanged = true;
        }
        if (ImGui::MenuItem("Pantalla completa", "F11", settings_.fullscreen)) {
            settings_.fullscreen = !settings_.fullscreen;
            out.fullscreenChanged = out.settingsChanged = true;
        }
        if (ImGui::MenuItem("Barra de menús", "Esc", settings_.showMenu)) {
            settings_.showMenu = !settings_.showMenu;
            out.settingsChanged = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Ayuda")) {
        if (ImGui::MenuItem("Acerca de plumsg")) showAbout_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// Volume, and the Roland sound: the FM and PSG music played on a SoundFont.
void Ui::drawSoundMenu(const UiState& state, UiActions& out) {
    if (ImGui::MenuItem("Silenciar", "M", settings_.mute)) {
        settings_.mute = !settings_.mute;
        out.settingsChanged = true;
    }
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Volumen", &settings_.volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;

    ImGui::SeparatorText("Música");
    if (ImGui::MenuItem("Original (YM2612 y PSG)", nullptr, !settings_.roland)) {
        settings_.roland = false;
        out.settingsChanged = out.soundChanged = true;
    }
    if (ImGui::MenuItem("Sintetizador: la música con un SoundFont", nullptr, settings_.roland)) {
        settings_.roland = true;
        out.settingsChanged = out.soundChanged = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Las notas del YM2612 y del PSG suenan con instrumentos General MIDI del SoundFont\n"
                          "elegido, como en un módulo SC-55. Las voces y la batería digital (el DAC) siguen siendo las del juego.\n"
                          "Cada sonido FM recibe un instrumento por reglas, que se cambia a mano en el\n"
                          "Editor de instrumentos.");
    ImGui::BeginDisabled(!settings_.roland);
    if (state.disc && settings_.roland)
        ImGui::TextDisabled("Los discos de Mega CD suenan siempre originales:\nsu música son pistas de audio del disco.");
    if (ImGui::MenuItem("Efectos de sonido originales", nullptr, settings_.effectsOriginal)) {
        settings_.effectsOriginal = !settings_.effectsOriginal;
        out.settingsChanged = out.soundChanged = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Los efectos (anillos, saltos, golpes...) suenan como en la consola y solo la música\n"
                          "pasa por el SoundFont. Para saber cuáles son, usa antes Sacar canciones en el Editor\n"
                          "de instrumentos. Cada efecto se puede cambiar también a mano en el editor.");
    drawSoundfontMenu(out);
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Sonido original", &settings_.chipMix, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cuánto se oye además la música original del YM2612 y el PSG.");
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = out.soundChanged = true;
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Reverb", &settings_.reverb, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = out.soundChanged = true;
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Chorus", &settings_.chorus, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = out.soundChanged = true;
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::MenuItem("Editor de instrumentos...")) {
        out.openEditor = true;
        out.editorFor = state.romLoaded ? state.romPath : std::string();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Graba un trozo de la música del juego (o abre un .vgm), escucha cada instrumento\n"
                          "y elige con qué instrumento General MIDI suena en el sintetizador.");
}

void Ui::setSoundfonts(std::string included, std::string folder) {
    includedSoundfont_ = std::move(included);
    soundfontFolder_ = std::move(folder);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(soundfontFolder_), ec);
}

// The SoundFonts to choose from, in a drop-down: the included one, and every .sf2 in
// the user's folder (read while it is open, so new downloads show up at once).
void Ui::drawSoundfontMenu(UiActions& out) {
    drawSoundfontCombo(out);
    if (ImGui::MenuItem("Abrir la carpeta de SoundFonts")) SDL_OpenURL(("file:///" + soundfontFolder_).c_str());
    if (ImGui::MenuItem("Elegir otro (.sf2)...")) chooseSoundfont(out);
}

void Ui::drawSoundfontCombo(UiActions& out, bool labelled) {
    auto choose = [&](const std::string& path) {
        settings_.soundfont = path;
        out.settingsChanged = out.soundChanged = true;
    };
    auto name = [](const std::string& path) { return std::filesystem::u8path(path).stem().u8string(); };
    const std::string current = settings_.soundfont.empty() ? std::string("GeneralUser GS (incluido)") : name(settings_.soundfont);
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo(labelled ? "SoundFont" : "##soundfont", current.c_str())) {
        if (ImGui::Selectable("GeneralUser GS (incluido)", settings_.soundfont.empty())) choose("");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", includedSoundfont_.c_str());
        std::vector<std::string> fonts;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(soundfontFolder_), ec)) {
            std::string ext = e.path().extension().u8string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (e.is_regular_file(ec) && ext == ".sf2") fonts.push_back(e.path().u8string());
        }
        std::sort(fonts.begin(), fonts.end());
        bool listed = settings_.soundfont.empty();
        for (const std::string& f : fonts) {
            const bool on = settings_.soundfont == f;
            listed = listed || on;
            if (ImGui::Selectable(name(f).c_str(), on)) choose(f);
        }
        if (!listed) ImGui::Selectable(current.c_str(), true);  // chosen from elsewhere
        if (fonts.empty()) ImGui::TextDisabled("Deja tus .sf2 en la carpeta de SoundFonts\ny aparecerán aquí.");
        ImGui::EndCombo();
    }
    if (!labelled && ImGui::IsItemHovered()) ImGui::SetTooltip("El SoundFont del sintetizador");
}

void Ui::chooseSoundfont(UiActions& out) {
    const std::string start = settings_.soundfont.empty() ? settings_.lastFolder
                                                          : settings_.soundfont.substr(0, settings_.soundfont.find_last_of("/\\"));
    std::string chosen;
    browseFor_ = BrowseFor::Soundfont;
    const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    switch (fullscreen ? DialogResult::Unavailable : openFileDialog(window_, start, Pick::AnyFile, &chosen)) {
        case DialogResult::Chosen:
            settings_.soundfont = chosen;
            out.settingsChanged = out.soundChanged = true;
            break;
        case DialogResult::Cancelled: break;
        case DialogResult::Unavailable: browser_.open(start, Pick::AnyFile); break;
    }
}

// A .vgm or .vgz for the instrument editor.
void Ui::chooseTrack() {
    std::string chosen;
    browseFor_ = BrowseFor::Track;
    const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    switch (fullscreen ? DialogResult::Unavailable : openFileDialog(window_, settings_.lastFolder, Pick::AnyFile, &chosen)) {
        case DialogResult::Chosen:
            if (editor_) editor_->openTrack(chosen);
            break;
        case DialogResult::Cancelled: break;
        case DialogResult::Unavailable: browser_.open(settings_.lastFolder, Pick::AnyFile); break;
    }
}

bool Ui::drawBindings(InputConfig& c, bool game) {
    bool changed = false;
    if (ImGui::BeginTable(game ? "game_bindings" : "bindings", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Acción", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Teclado (jugador 1)");
        ImGui::TableSetupColumn("Teclado (jugador 2)");
        ImGui::TableSetupColumn("Mando");
        ImGui::TableHeadersRow();
        for (int row = 0; row < (game ? InputConfig::kButtons : kRows); ++row) {
            ImGui::PushID(row);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kRowNames[row]);
            for (int col = 0; col < 3; ++col) {
                const bool pad = col == 2;
                ImGui::TableNextColumn();
                if (col == 1 && row >= InputConfig::kButtons) continue;  // hotkeys: one keyboard list
                ImGui::PushID(col);
                std::string label;
                if (capture_.active && capture_.game == game && capture_.row == row && capture_.column == col) {
                    label = pad ? "Pulsa un botón..." : "Pulsa una tecla...";
                } else if (pad) {
                    for (auto b : padOf(c, row)) label += (label.empty() ? "" : ", ") + std::string(padButtonName(b));
                } else {
                    for (auto k : keysOf(c, row, col)) label += (label.empty() ? "" : ", ") + std::string(SDL_GetScancodeName(k));
                }
                if (label.empty()) label = "-";
                const float small = ImGui::GetFrameHeight() * 2 + ImGui::GetStyle().ItemSpacing.x * 2;
                if (ImGui::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - small, 0)))
                    capture_ = {true, row, col, false, game};
                ImGui::SameLine();
                if (ImGui::Button("+", ImVec2(ImGui::GetFrameHeight(), 0))) capture_ = {true, row, col, true, game};
                ImGui::SameLine();
                if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) {
                    if (pad) padOf(c, row).clear();
                    else keysOf(c, row, col).clear();
                    capture_.active = false;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    return changed;
}

void Ui::drawControls(UiActions& out) {
    ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Controles", &showControls_)) {
        ImGui::End();
        return;
    }
    if (!showControls_ && !capture_.game) capture_.active = false;

    ImGui::TextWrapped("Pulsa una casilla y luego la tecla o el botón del mando. \"+\" añade otro, "
                       "\"x\" lo borra, Esc cancela.");
    if (input_.hasGameButtons())
        ImGui::TextDisabled("El juego abierto tiene sus propios botones (en Opciones de este juego); "
                            "aquí cambias los generales.");
    InputConfig c = input_.config();
    bool changed = drawBindings(c, false);

    ImGui::Spacing();
    changed |= ImGui::Checkbox("Usar el stick izquierdo como cruceta", &c.stickAsDpad);
    ImGui::BeginDisabled(!c.stickAsDpad);
    int dead = c.stickDeadzone * 100 / 32767;
    if (ImGui::SliderInt("Zona muerta del stick", &dead, 0, 95, "%d%%")) {
        c.stickDeadzone = dead * 32767 / 100;
        changed = true;
    }
    ImGui::EndDisabled();

    const auto pads = input_.padNames();
    if (pads.empty()) {
        ImGui::TextDisabled("No hay mandos conectados.");
    } else {
        ImGui::Text("Mandos conectados:");
        for (const auto& p : pads) ImGui::BulletText("%s", p.c_str());
    }

    ImGui::Spacing();
    if (ImGui::Button("Restaurar predeterminados")) {
        const InputConfig d = InputConfig::defaults();
        c.keys = d.keys;
        c.keys2 = d.keys2;
        c.pad = d.pad;
        c.hotkeyKeys = d.hotkeyKeys;
        c.hotkeyPad = d.hotkeyPad;
        c.stickAsDpad = d.stickAsDpad;
        c.stickDeadzone = d.stickDeadzone;
        capture_.active = false;
        changed = true;
    }
    if (changed) {
        input_.setConfig(std::move(c));
        out.settingsChanged = true;
    }
    ImGui::End();
}

void Ui::drawVideo(const UiState& state, UiActions& out) {
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Vídeo", &showVideo_)) {
        ImGui::End();
        return;
    }
    bool changed = false;

    if (ImGui::BeginCombo("Vista", settings_.view.c_str())) {
        for (const char* v : state.views) {
            if (ImGui::Selectable(v, settings_.view == v)) {
                settings_.view = v;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("2D: la imagen normal de la Mega Drive.\n"
                          "capas (depuración): pinta cada píxel según su capa (plano B azul, plano A\n"
                          "verde, ventana cian, sprites rojo, fondo gris; amarillento si tiene\n"
                          "prioridad alta); no son los colores del juego.");

    ImGui::SeparatorText("Imagen");
    int scale = static_cast<int>(settings_.render.scale);
    changed |= ImGui::RadioButton("Escala entera", &scale, static_cast<int>(ScaleMode::Integer));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Ajustar", &scale, static_cast<int>(ScaleMode::Fit));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Estirar", &scale, static_cast<int>(ScaleMode::Stretch));
    settings_.render.scale = static_cast<ScaleMode>(scale);
    changed |= ImGui::Checkbox("Suavizar píxeles", &settings_.render.smooth);
    changed |= ImGui::Checkbox("Píxeles cuadrados (sin estirar a 4:3)", &settings_.render.squarePixels);
    int filter = static_cast<int>(settings_.render.filter);
    ImGui::TextUnformatted("Filtro 2D:");
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Ninguno", &filter, static_cast<int>(Filter2D::None));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Scanlines", &filter, static_cast<int>(Filter2D::Scanlines));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("CRT", &filter, static_cast<int>(Filter2D::Crt));
    settings_.render.filter = static_cast<Filter2D>(filter);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scanlines: las líneas oscuras entre las líneas de la imagen, como en una tele.\n"
                          "CRT: además, la pantalla un poco curva, la máscara de colores y las esquinas\n"
                          "más oscuras. Solo en la vista 2D; se ve mejor a pantalla completa.");
    ImGui::BeginDisabled(settings_.render.filter == Filter2D::None);
    float strength = settings_.render.filterStrength * 100.0f;
    if (ImGui::SliderFloat("Intensidad del filtro", &strength, 0.0f, 100.0f, "%.0f %%"))
        settings_.render.filterStrength = strength / 100.0f;
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Ventana");
    ImGui::BeginDisabled(settings_.fullscreen);
    ImGui::TextUnformatted("Tamaño:");
    for (int s = 1; s <= 6; ++s) {
        ImGui::SameLine();
        const std::string label = std::to_string(s) + "x";
        if (ImGui::Button(label.c_str())) {
            settings_.windowScale = s;
            out.windowScale = s;
            changed = true;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Pantalla completa (F11)", &settings_.fullscreen)) {
        out.fullscreenChanged = true;
        changed = true;
    }
    changed |= ImGui::Checkbox("Mostrar la barra de menús (Esc)", &settings_.showMenu);

    ImGui::SeparatorText("Pseudo-3D");
    ImGui::BeginDisabled(!state.romLoaded);
    bool profile = state.hasProfile;
    if (ImGui::Checkbox("Guardar el 3D solo para este juego", &profile)) out.profile = profile ? 1 : 0;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(state.romLoaded ? "Los ajustes de abajo (también el efecto 3D y la cámara) se guardan\n"
                                            "para %s y se recuperan al abrirlo.\n"
                                            "Al desmarcarlo, el juego vuelve a usar los ajustes generales."
                                          : "Abre una ROM para guardarle un perfil 3D propio.",
                          state.gameName.c_str());
    if (state.hasProfile) {
        ImGui::SameLine();
        ImGui::TextDisabled("(perfil de %s)", state.gameName.c_str());
    }
    changed |= ImGui::Checkbox("Efecto 3D", &settings_.render.pseudo3d);
    ImGui::SameLine();
    ImGui::TextDisabled("(atajo en Opciones > Controles)");
    RenderOptions& r = settings_.render;
    ImGui::BeginDisabled(!r.pseudo3d);
    int style = static_cast<int>(r.style);
    ImGui::TextUnformatted("Estilo:");
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Vóxeles", &style, static_cast<int>(Style3D::Voxels));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Capas", &style, static_cast<int>(Style3D::Layers));
    r.style = static_cast<Style3D>(style);
    // Sliders save once released rather than on every frame of the drag.
    float depth = r.depth * 100.0f, thickness = r.thickness * 100.0f;
    if (ImGui::SliderFloat("Profundidad", &depth, 0.0f, 100.0f, "%.0f %%")) r.depth = depth / 100.0f;
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::Checkbox("Inclinar la cámara", &r.tiltCamera);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sin marcar, la cámara mira de frente y la imagen se ve como en 2D;\n"
                          "la profundidad se nota con la iluminación y la cámara viva.");
    ImGui::BeginDisabled(!r.tiltCamera);
    ImGui::SliderFloat("Inclinación", &r.tilt, kMinTilt, kMaxTilt, "%.0f°");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SliderFloat("Giro", &r.yaw, -kMaxYaw, kMaxYaw, "%.0f°");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndDisabled();
    ImGui::SliderFloat("Zoom", &r.zoom, kMinZoom, kMaxZoom, "%.2fx");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::SliderFloat("Grosor", &thickness, 0.0f, 100.0f, "%.0f %%")) r.thickness = thickness / 100.0f;
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::BeginDisabled(r.style != Style3D::Voxels);
    float extrusion = r.extrusion * 100.0f;
    if (ImGui::SliderFloat("Extrusión", &extrusion, 0.0f, 100.0f, "%.0f %%")) r.extrusion = extrusion / 100.0f;
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Iluminación", &r.lighting);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Luz lateral sobre los bloques y sombras suaves de cada plano sobre los de detrás.\n"
                          "Hace que el relieve se vea también con la cámara de frente.");
    changed |= ImGui::Checkbox("Cámara viva", &r.liveCamera);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("La cámara se balancea unos grados siguiendo la acción,\n"
                          "así las capas se mueven entre sí aunque mires de frente.");
    changed |= ImGui::Checkbox("Sombra de los sprites", &r.shadows);
    changed |= ImGui::Checkbox("Plano B con prioridad delante", &r.highBInFront);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Los tiles del plano B con prioridad alta se ponen delante, con los del plano A.");
    changed |= ImGui::Checkbox("Plano B plano (como el cielo)", &r.flatB);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Para juegos que pintan el cielo en el plano B: no lo levanta en 3D.");
    if (ImGui::Button("Valores por defecto")) {
        const RenderOptions defaults;
        r.style = defaults.style;
        r.depth = defaults.depth;
        r.tilt = defaults.tilt;
        r.yaw = defaults.yaw;
        r.zoom = defaults.zoom;
        r.extrusion = defaults.extrusion;
        r.thickness = defaults.thickness;
        r.shadows = defaults.shadows;
        r.tiltCamera = defaults.tiltCamera;
        r.lighting = defaults.lighting;
        r.liveCamera = defaults.liveCamera;
        r.highBInFront = defaults.highBInFront;
        r.flatB = defaults.flatB;
        changed = true;
    }
    ImGui::TextDisabled("Cámara: arrastra con el ratón o usa el stick derecho para girar,\n"
                        "la rueda para acercar y doble clic o el clic del stick para centrarla.");
    ImGui::EndDisabled();

    out.settingsChanged |= changed;
    ImGui::End();
}

void Ui::drawStateMenu(const UiState& state) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(470, std::min(vp->Size.y - 40.0f, 520.0f)), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    if (!ImGui::Begin("Estados", &stateMenu_.open, flags)) {
        ImGui::End();
        return;
    }
    if (!state.romLoaded) ImGui::TextDisabled("Abre una ROM para guardar estados.");
    ImGui::TextDisabled("A / Intro: cargar   Y / Espacio: guardar   B / Esc: cerrar");
    ImGui::Separator();
    ImGui::BeginChild("ranuras");
    const float thumbH = 56.0f;
    for (int i = 0; i < static_cast<int>(state.slots.size()); ++i) {
        const SlotInfo& info = state.slots[static_cast<size_t>(i)];
        ImGui::PushID(i);
        const bool selected = i == stateMenu_.selected;
        const ImVec2 start = ImGui::GetCursorPos();
        // The whole row is clickable with the mouse too: a click selects, a double click loads.
        if (ImGui::Selectable("##fila", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, thumbH))) {
            stateMenu_.selected = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && info.used && state.romLoaded) {
                stateMenu_.load = i;
                stateMenu_.open = false;
                settings_.stateSlot = i;
                pendingSave_ = true;
            }
        }
        if (selected && stateMenu_.scroll) {
            ImGui::SetScrollHereY(0.5f);
            stateMenu_.scroll = false;
        }
        ImGui::SetCursorPos(start);
        const float thumbW = info.height ? thumbH * static_cast<float>(info.width) / static_cast<float>(info.height) : thumbH * 1.43f;
        if (info.texture) ImGui::Image(static_cast<ImTextureID>(info.texture), ImVec2(thumbW, thumbH));
        else ImGui::Dummy(ImVec2(thumbW, thumbH));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("Ranura %d%s", i, i == settings_.stateSlot ? "  (actual)" : "");
        if (info.used) ImGui::TextUnformatted(info.when.c_str());
        else ImGui::TextDisabled("vacía");
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

void Ui::drawAbout() {
    if (!ImGui::Begin("Acerca de plumsg", &showAbout_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (iconTexture_) {
        const float w = ImGui::GetFontSize() * 6.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f));
        ImGui::Image(static_cast<ImTextureID>(iconTexture_), ImVec2(w, w));
    }
    if (logoTexture_) {
        const float w = ImGui::GetFontSize() * 22.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f));
        ImGui::Image(static_cast<ImTextureID>(logoTexture_), ImVec2(w, w * logoHeight_ / logoWidth_));
    }
    ImGui::TextUnformatted("plumsg, emulador de Sega Mega Drive / Genesis con efecto pseudo-3D.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Arrastra una ROM a la ventana o usa Archivo > Abrir ROM.");
    ImGui::SeparatorText("Atajos");
    ImGui::BulletText("Ctrl+O  abrir ROM;  F10  biblioteca de juegos");
    ImGui::BulletText("F1  reiniciar;  F12  parar la emulación y volver al inicio");
    ImGui::BulletText("F2  cambiar de vista");
    ImGui::BulletText("P  pausa");
    ImGui::BulletText("M  silenciar el sonido");
    ImGui::BulletText("F11  pantalla completa");
    ImGui::BulletText("Esc  mostrar u ocultar la barra de menús");
    ImGui::BulletText("F3  efecto 3D (se puede cambiar en Opciones > Controles)");
    ImGui::BulletText("F5 / F8  guardar / cargar estado; F6 / F7  cambiar de ranura");
    ImGui::BulletText("F4 o clic del stick izquierdo  menú de estados, manejable con el mando");
    ImGui::BulletText("Retroceso o gatillo izquierdo (mantener)  rebobinar");
    ImGui::BulletText("Tab o gatillo derecho (mantener)  avance rápido;  F9  cámara lenta");
    ImGui::SeparatorText("Mando (teclado)");
    ImGui::BulletText("Flechas  cruceta;  A S D  botones A B C;  Q W E  botones X Y Z");
    ImGui::BulletText("Intro  Start;  Mayús derecha  Mode");
    ImGui::End();
}

void Ui::drawError() {
    const char* const kTitle = "plumsg##error";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(error_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    // The mouse may be hidden in full screen: Enter, Esc or the pad's A close it too.
    bool close = ImGui::Button("Aceptar", ImVec2(ImGui::GetFontSize() * 8.0f, 0));
    for (ImGuiKey key : {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape, ImGuiKey_GamepadFaceDown})
        close |= ImGui::IsKeyPressed(key, false);
    if (close) {
        error_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Ui::drawOverlays(const UiState& state) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    const float top = vp->Pos.y + static_cast<float>(menuHeight_);

    if (!state.romLoaded) {
        // Behind every window, so it never shows through the option windows.
        // The marquee above the hint, as wide as it is or 60% of the window if narrower.
        const char* hint = "Arrastra una ROM aquí, usa Archivo > Abrir ROM o abre la biblioteca (F10)";
        const ImVec2 size = ImGui::CalcTextSize(hint);
        const float logoW = logoTexture_ ? std::min(static_cast<float>(logoWidth_), vp->Size.x * 0.6f) : 0.0f;
        const float logoH = logoTexture_ ? logoW * logoHeight_ / logoWidth_ : 0.0f;
        const float gap = logoTexture_ ? size.y * 1.5f : 0.0f;
        // The plum above it, if the window is tall enough to leave the game area some room.
        const float room = vp->Size.y - menuHeight_ - logoH - gap - size.y;
        const float iconS = iconTexture_ ? std::min(logoW * 0.45f, room * 0.45f) : 0.0f;
        const float iconGap = iconS > size.y * 3.0f ? size.y : 0.0f;
        const float plum = iconGap > 0.0f ? iconS : 0.0f;
        const float y = top + (room - plum - iconGap) * 0.5f;
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (plum > 0.0f) {
            const ImVec2 a(vp->Pos.x + (vp->Size.x - plum) * 0.5f, y);
            draw->AddImage(static_cast<ImTextureID>(iconTexture_), a, ImVec2(a.x + plum, a.y + plum));
        }
        const float logoY = y + plum + iconGap;
        if (logoTexture_) {
            const ImVec2 a(vp->Pos.x + (vp->Size.x - logoW) * 0.5f, logoY);
            draw->AddImage(static_cast<ImTextureID>(logoTexture_), a, ImVec2(a.x + logoW, a.y + logoH));
        }
        const ImVec2 pos(vp->Pos.x + (vp->Size.x - size.x) * 0.5f, logoY + logoH + gap);
        draw->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), hint);
    }
    const char* badge = nullptr;
    if (state.romLoaded) {
        if (state.paused) badge = "PAUSA";
        else if (!state.speed.empty()) badge = state.speed.c_str();
    }
    if (badge) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 10, top + 10), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##pausa", nullptr, flags);
        ImGui::TextUnformatted(badge);
        ImGui::End();
    }
    if (!toast_.empty() && SDL_TICKS_PASSED(SDL_GetTicks(), toastUntil_)) toast_.clear();
    if (!toast_.empty()) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 10, vp->Pos.y + vp->Size.y - 10), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##aviso", nullptr, flags);
        ImGui::TextUnformatted(toast_.c_str());
        ImGui::End();
    }
}

void Ui::toggleLibrary() {
    showLibrary_ = !showLibrary_;
    if (!showLibrary_) return;
    if (!library_.scanned() || library_.folders() != settings_.libraryFolders) rescanLibrary();
    libraryView_.scroll = true;
}

void Ui::rescanLibrary() {
    library_.scan(settings_.libraryFolders);
    libraryView_.selected = 0;
    libraryView_.scroll = true;
}

std::vector<size_t> Ui::visibleGames() {
    const std::string filter = searchKey(libraryView_.search);
    std::vector<size_t> out;
    const auto& games = library_.games();
    for (size_t i = 0; i < games.size(); ++i) {
        if (settings_.librarySystem >= 0 && static_cast<int>(games[i].system) != settings_.librarySystem) continue;
        if (filter.empty() || games[i].key.find(filter) != std::string::npos) out.push_back(i);
    }
    return out;
}

// While the library is open it owns the pad and the keyboard (except while typing in
// the search box): the arrows move through the grid, A (or Enter) plays the game, B
// (or Esc) closes it, Y (or Ctrl+F) goes to the search box, and the shoulder buttons
// (or Page Up and Page Down) change the console tab.
bool Ui::handleLibrary(const SDL_Event& e) {
    if (e.type != SDL_KEYDOWN && e.type != SDL_CONTROLLERBUTTONDOWN && e.type != SDL_KEYUP &&
        e.type != SDL_CONTROLLERBUTTONUP && e.type != SDL_TEXTINPUT)
        return false;
    if (browser_.isOpen() || showBios_ || showCheats_ || showGameOptions_ || showCoverSearch_) return false;
    const bool typing = ImGui::GetIO().WantTextInput;
    if (typing && e.type != SDL_CONTROLLERBUTTONDOWN && e.type != SDL_CONTROLLERBUTTONUP) {
        // The search box gets the keys; Enter or the arrows hand them back to the grid.
        if (e.type == SDL_KEYDOWN && (e.key.keysym.scancode == SDL_SCANCODE_RETURN ||
                                      e.key.keysym.scancode == SDL_SCANCODE_DOWN)) {
            ImGui::SetWindowFocus(nullptr);
            return true;
        }
        return false;
    }
    // Releases always reach ImGui, so it never thinks a key is still held.
    if (e.type != SDL_KEYDOWN && e.type != SDL_CONTROLLERBUTTONDOWN) return e.type == SDL_TEXTINPUT;
    if (input_.hotkeyFor(e) == Hotkey::Library) {
        showLibrary_ = false;
        return true;
    }
    enum { None, Up, Down, Left, Right, Play, Close, Search, PrevTab, NextTab, Menu } action = None;
    if (e.type == SDL_KEYDOWN) {
        const bool ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;
        const bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
        switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_APPLICATION: action = Menu; break;
            case SDL_SCANCODE_F10: if (shift) action = Menu; else return false; break;
            case SDL_SCANCODE_UP: action = Up; break;
            case SDL_SCANCODE_DOWN: action = Down; break;
            case SDL_SCANCODE_LEFT: action = Left; break;
            case SDL_SCANCODE_RIGHT: action = Right; break;
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: action = Play; break;
            case SDL_SCANCODE_ESCAPE: action = Close; break;
            case SDL_SCANCODE_PAGEUP: action = PrevTab; break;
            case SDL_SCANCODE_PAGEDOWN: action = NextTab; break;
            case SDL_SCANCODE_F: if (ctrl) action = Search; break;
            default: return false;  // let ImGui have the rest (clicks on buttons, Tab...)
        }
    } else {
        switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: action = Up; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: action = Down; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: action = Left; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: action = Right; break;
            case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: action = Play; break;
            case SDL_CONTROLLER_BUTTON_B: action = Close; break;
            case SDL_CONTROLLER_BUTTON_Y: action = Search; break;
            case SDL_CONTROLLER_BUTTON_X: action = Menu; break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: action = PrevTab; break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: action = NextTab; break;
            default: break;
        }
    }
    // With a game's menu open the arrows move through it, A picks and B closes it.
    if (libraryView_.menuGame >= 0) {
        switch (action) {
            case Up: libraryView_.menuItem = (libraryView_.menuItem + LibraryView::kItems - 1) % LibraryView::kItems; break;
            case Down: libraryView_.menuItem = (libraryView_.menuItem + 1) % LibraryView::kItems; break;
            case Play: libraryView_.menuAction = libraryView_.menuItem; break;
            case Close: case Menu: libraryView_.menuGame = -1; break;
            default: break;
        }
        return true;
    }
    if (action == PrevTab || action == NextTab) {
        // Tabs: -1 (all), then each console; wraps around.
        int tab = settings_.librarySystem + 1 + (action == NextTab ? 1 : -1);
        tab = (tab + kSystemCount + 1) % (kSystemCount + 1);
        settings_.librarySystem = tab - 1;
        libraryView_.selected = 0;
        libraryView_.scroll = true;
        libraryView_.tabChanged = true;
        pendingSave_ = true;
        return true;
    }
    const int count = static_cast<int>(visibleGames().size());
    const int cols = std::max(1, libraryView_.columns);
    int& sel = libraryView_.selected;
    switch (action) {
        case Up: if (sel - cols >= 0) sel -= cols; break;
        case Down: sel = std::min(count - 1, sel + cols); break;
        case Left: sel = std::max(0, sel - 1); break;
        case Right: sel = std::min(count - 1, sel + 1); break;
        case Play: if (count > 0) libraryView_.load = sel; break;
        case Close: showLibrary_ = false; break;
        case Search: libraryView_.focusSearch = true; break;
        case Menu:
            if (count > 0) {
                libraryView_.menuGame = sel;
                libraryView_.menuItem = 0;
                libraryView_.openMenu = true;
            }
            break;
        default: break;
    }
    sel = std::clamp(sel, 0, std::max(0, count - 1));
    libraryView_.scroll = action == Up || action == Down || action == Left || action == Right;
    return true;
}

// The library's folders: each with a button to take it out, and one to add another.
void Ui::drawLibraryFolders(UiActions& out) {
    int remove = -1;
    for (int i = 0; i < static_cast<int>(settings_.libraryFolders.size()); ++i) {
        ImGui::PushID(i);
        if (ImGui::SmallButton("Quitar")) remove = i;
        ImGui::SameLine();
        ImGui::TextUnformatted(settings_.libraryFolders[static_cast<size_t>(i)].c_str());
        ImGui::PopID();
    }
    if (remove >= 0) {
        settings_.libraryFolders.erase(settings_.libraryFolders.begin() + remove);
        out.settingsChanged = true;
        rescanLibrary();
    }
    if (ImGui::Button("Añadir carpeta...")) {
        browseFor_ = BrowseFor::LibraryFolder;
        browser_.open(settings_.libraryFolders.empty() ? settings_.lastFolder : settings_.libraryFolders.back(), Pick::Folder);
    }
}

void Ui::drawLibrary(UiActions& out) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float top = static_cast<float>(menuHeight_);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!ImGui::Begin("Biblioteca", &showLibrary_, flags)) {
        ImGui::End();
        return;
    }
    if (pendingSave_) {
        out.settingsChanged = true;
        pendingSave_ = false;
    }
    auto& games = library_.games();

    if (settings_.libraryFolders.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Elige la carpeta donde tienes tus juegos de Mega Drive, 32X o Mega CD.");
        ImGui::TextDisabled("Se buscan también en sus subcarpetas, y luego puedes añadir más carpetas.");
        if (ImGui::Button("Elegir carpeta...")) {
            browseFor_ = BrowseFor::LibraryFolder;
            browser_.open(settings_.lastFolder, Pick::Folder);
        }
        ImGui::End();
        return;
    }

    const std::string foldersLabel = settings_.libraryFolders.size() == 1
                                         ? "Carpeta: " + settings_.libraryFolders[0]
                                         : std::to_string(settings_.libraryFolders.size()) + " carpetas";
    if (ImGui::TreeNodeEx("carpetas", showFolders_ ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s", foldersLabel.c_str())) {
        showFolders_ = true;
        drawLibraryFolders(out);
        ImGui::TreePop();
    } else {
        showFolders_ = false;
    }
    if (ImGui::Button("Volver a buscar")) rescanLibrary();
    ImGui::SameLine();
    int missing = 0;
    for (const auto& g : games) missing += g.cover == Library::Cover::Missing;
    ImGui::BeginDisabled(missing == 0 || library_.pending() > 0);
    if (ImGui::Button("Descargar carátulas que faltan")) library_.downloadMissing();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Busca las carátulas en libretro-thumbnails (internet) por el nombre de cada juego\n"
                          "y las guarda en la carpeta caratulas de plumsg. También puedes poner tus\n"
                          "propias imágenes: un .png o .jpg con el mismo nombre que el juego, a su lado.");
    ImGui::SameLine();
    if (ImGui::Button("BIOS del Mega CD...")) showBios_ = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Abrir la biblioteca al empezar", &settings_.libraryAtStart)) out.settingsChanged = true;

    // One tab per console, with how many games each has.
    int counts[kSystemCount] = {};
    for (const auto& g : games) ++counts[static_cast<int>(g.system)];
    if (ImGui::BeginTabBar("sistemas")) {
        for (int tab = -1; tab < kSystemCount; ++tab) {
            const int n = tab < 0 ? static_cast<int>(games.size()) : counts[tab];
            const std::string label = std::string(tab < 0 ? "Todos" : systemName(static_cast<ConsoleSystem>(tab))) +
                                      " (" + std::to_string(n) + ")###tab" + std::to_string(tab);
            // Tabs changed with the pad or the keyboard are selected by hand.
            const ImGuiTabItemFlags f =
                libraryView_.tabChanged && settings_.librarySystem == tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label.c_str(), nullptr, f)) {
                if (!libraryView_.tabChanged && settings_.librarySystem != tab) {
                    settings_.librarySystem = tab;
                    libraryView_.selected = 0;
                    out.settingsChanged = true;
                }
                ImGui::EndTabItem();
            }
        }
        libraryView_.tabChanged = false;
        ImGui::EndTabBar();
    }

    if (libraryView_.focusSearch) {
        ImGui::SetKeyboardFocusHere();
        libraryView_.focusSearch = false;
    }
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputTextWithHint("##buscar", "Buscar (Ctrl+F o Y)", libraryView_.search, sizeof libraryView_.search))
        libraryView_.selected = 0;
    ImGui::SameLine();
    const std::vector<size_t> visible = visibleGames();
    if (library_.pending() > 0) ImGui::Text("%d juegos. Buscando carátulas, quedan %d...", static_cast<int>(visible.size()), library_.pending());
    else ImGui::Text("%d juegos. %s", static_cast<int>(visible.size()), library_.downloadStatus().c_str());
    ImGui::TextDisabled("Flechas o cruceta: elegir   A o Intro: jugar   X, botón derecho o tecla de menú: trucos, opciones e instrumentos\n"
                        "B o Esc: cerrar   L / R o RePág / AvPág: consola");
    ImGui::Separator();

    if (games.empty()) ImGui::TextDisabled("No hay juegos (.md, .bin, .gen, .smd, .32x, .zip, .cue, .chd, .iso) en estas carpetas.");
    else if (visible.empty() && libraryView_.search[0]) ImGui::TextDisabled("Ningún juego coincide con la búsqueda.");
    else if (visible.empty()) ImGui::TextDisabled("No hay juegos de esta consola en tus carpetas.");

    const float coverW = 150.0f, coverH = 210.0f;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float textH = ImGui::GetTextLineHeight() * 2 + 4;
    const ImVec2 card(coverW + 12, coverH + textH + 12);
    ImGui::BeginChild("juegos");
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>((avail + style.ItemSpacing.x) / (card.x + style.ItemSpacing.x)));
    libraryView_.columns = cols;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int n = 0; n < static_cast<int>(visible.size()); ++n) {
        Library::Game& g = games[visible[static_cast<size_t>(n)]];
        if (n % cols) ImGui::SameLine();
        ImGui::PushID(n);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool selected = n == libraryView_.selected;
        if (ImGui::Selectable("##juego", selected, ImGuiSelectableFlags_AllowDoubleClick, card)) {
            libraryView_.selected = n;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) libraryView_.load = n;
        }
        // On release, like ImGui's own context menus: the press closes open popups.
        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            libraryView_.selected = n;
            libraryView_.menuGame = n;
            libraryView_.menuItem = 0;
            libraryView_.openMenu = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", systemName(g.system), g.path.c_str());
        if (selected && libraryView_.scroll) {
            ImGui::SetScrollHereY(0.5f);
            libraryView_.scroll = false;
        }
        // The cover, fitted into its box; a plain box with the console's name if there is none.
        const ImVec2 box(pos.x + 6, pos.y + 6);
        if (g.texture && g.height > 0) {
            float w = coverW, h = coverW * static_cast<float>(g.height) / static_cast<float>(g.width);
            if (h > coverH) {
                h = coverH;
                w = coverH * static_cast<float>(g.width) / static_cast<float>(g.height);
            }
            const ImVec2 a(box.x + (coverW - w) * 0.5f, box.y + (coverH - h));
            draw->AddImage(static_cast<ImTextureID>(g.texture), a, ImVec2(a.x + w, a.y + h));
        } else {
            draw->AddRectFilled(box, ImVec2(box.x + coverW, box.y + coverH), IM_COL32(40, 44, 56, 255), 4.0f);
            const char* label = g.cover == Library::Cover::Pending ? "..." : systemName(g.system);
            const ImVec2 size = ImGui::CalcTextSize(label);
            draw->AddText(ImVec2(box.x + (coverW - size.x) * 0.5f, box.y + (coverH - size.y) * 0.5f),
                          IM_COL32(140, 146, 160, 255), label);
        }
        // On the "Todos" tab, a badge on 32X and Mega CD covers says which console.
        if (settings_.librarySystem < 0 && g.system != ConsoleSystem::MegaDrive) {
            const char* badge = systemName(g.system);
            const ImVec2 size = ImGui::CalcTextSize(badge);
            const ImVec2 b0(box.x + coverW - size.x - 10, box.y + 4);
            const ImU32 colour = g.system == ConsoleSystem::Mega32X ? IM_COL32(200, 60, 50, 230) : IM_COL32(40, 110, 200, 230);
            draw->AddRectFilled(b0, ImVec2(b0.x + size.x + 8, b0.y + size.y + 4), colour, 3.0f);
            draw->AddText(ImVec2(b0.x + 4, b0.y + 2), IM_COL32(255, 255, 255, 255), badge);
        }
        if (selected)
            draw->AddRect(ImVec2(pos.x + 2, pos.y + 2), ImVec2(pos.x + card.x - 2, pos.y + card.y - 2),
                          ImGui::GetColorU32(ImGuiCol_NavCursor), 6.0f, 0, 3.0f);
        // Two lines of title, cut to the card's width; the tags in grey after it.
        const ImVec4 clip(pos.x + 4, pos.y, pos.x + card.x - 4, pos.y + card.y);
        draw->AddText(nullptr, 0.0f, ImVec2(pos.x + 6, box.y + coverH + 4), ImGui::GetColorU32(ImGuiCol_Text),
                      g.title.c_str(), nullptr, coverW, &clip);
        ImGui::PopID();
    }
    ImGui::EndChild();

    // The game's menu, opened with the right button, X on the pad or the menu key.
    if (libraryView_.openMenu) {
        ImGui::OpenPopup("menu_juego");
        libraryView_.openMenu = false;
    }
    if (ImGui::BeginPopup("menu_juego")) {
        const int n = libraryView_.menuGame;
        if (n < 0 || n >= static_cast<int>(visible.size())) {
            ImGui::CloseCurrentPopup();
        } else {
            const Library::Game& g = games[visible[static_cast<size_t>(n)]];
            ImGui::TextDisabled("%s", g.title.c_str());
            ImGui::Separator();
            static const char* items[LibraryView::kItems] = {"Jugar", "Trucos...", "Opciones de este juego...",
                                                             "Editor de instrumentos...", "Buscar carátula..."};
            int picked = libraryView_.menuAction;
            for (int i = 0; i < LibraryView::kItems; ++i)
                if (ImGui::Selectable(items[i], libraryView_.menuItem == i, ImGuiSelectableFlags_NoAutoClosePopups)) picked = i;
            switch (picked) {
                case 0: libraryView_.load = n; break;
                case 1: out.cheatsFor = g.path; showCheats_ = true; break;
                case 2: out.optionsFor = g.path; showGameOptions_ = true; break;
                case 3:
                    out.editorFor = g.path;
                    out.openEditor = true;
                    showLibrary_ = false;
                    break;
                case 4: openCoverSearch(visible[static_cast<size_t>(n)]); break;
                default: break;
            }
            if (picked >= 0) {
                libraryView_.menuGame = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    } else {
        libraryView_.menuGame = -1;  // closed by clicking elsewhere
    }
    libraryView_.menuAction = -1;

    if (libraryView_.load >= 0 && libraryView_.load < static_cast<int>(visible.size())) {
        out.load = games[visible[static_cast<size_t>(libraryView_.load)]].path;
        showLibrary_ = false;
    }
    libraryView_.load = -1;
    ImGui::End();
}

void Ui::openCoverSearch(size_t game) {
    const auto& games = library_.games();
    if (game >= games.size()) return;
    const Library::Game& g = games[game];
    coverSearch_.game = game;
    coverSearch_.path = g.path;
    std::snprintf(coverSearch_.query, sizeof coverSearch_.query, "%s", g.title.c_str());
    coverSearch_.focus = true;
    library_.endCoverSearch();
    startCoverSearch();
    showCoverSearch_ = true;
}

void Ui::startCoverSearch() {
    coverSearch_.selected = -1;
    coverSearch_.autoPreview = true;
    library_.searchCovers(library_.games()[coverSearch_.game].system, coverSearch_.query);
}

// Looking for a game's cover by hand: the search term can be changed, and the box art
// names that look like it are listed, the closest one shown; clicking another shows
// that one, and the one shown can be used.
void Ui::drawCoverSearch() {
    auto& games = library_.games();
    const bool valid = showLibrary_ && coverSearch_.game < games.size() && games[coverSearch_.game].path == coverSearch_.path;
    if (!valid) showCoverSearch_ = false;
    ImGui::SetNextWindowSize(ImVec2(720, 460), ImGuiCond_Appearing);
    if (showCoverSearch_ && ImGui::Begin("Buscar carátula", &showCoverSearch_, ImGuiWindowFlags_NoCollapse)) {
        Library::Game& g = games[coverSearch_.game];
        const Library::CoverSearch& search = library_.coverSearch();
        ImGui::Text("%s", g.title.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g.path.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", systemName(g.system));
        if (coverSearch_.focus) {
            ImGui::SetKeyboardFocusHere();
            coverSearch_.focus = false;
        }
        ImGui::SetNextItemWidth(-90.0f);
        bool go = ImGui::InputTextWithHint("##nombre", "Nombre del juego", coverSearch_.query, sizeof coverSearch_.query,
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        go |= ImGui::Button("Buscar", ImVec2(-1, 0));
        if (go && coverSearch_.query[0]) startCoverSearch();
        if (coverSearch_.autoPreview && !search.busy) {
            coverSearch_.autoPreview = false;
            if (!search.names.empty()) {
                coverSearch_.selected = 0;
                library_.previewCover(g.system, search.names[0]);
            }
        }
        if (search.busy) ImGui::TextDisabled("Buscando...");
        else if (!search.status.empty()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "%s", search.status.c_str());
        else ImGui::TextDisabled("%d parecidas. Pulsa una para verla.", static_cast<int>(search.names.size()));

        const float previewW = 220.0f;
        ImGui::BeginChild("nombres", ImVec2(-previewW - ImGui::GetStyle().ItemSpacing.x, 0), ImGuiChildFlags_Borders);
        for (int i = 0; i < static_cast<int>(search.names.size()); ++i) {
            const std::string& name = search.names[static_cast<size_t>(i)];
            if (ImGui::Selectable(name.c_str(), coverSearch_.selected == i)) {
                coverSearch_.selected = i;
                library_.previewCover(g.system, name);
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginGroup();
        const float boxH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pos, ImVec2(pos.x + previewW, pos.y + boxH), IM_COL32(40, 44, 56, 255), 4.0f);
        const bool shown = search.preview && search.height > 0 && coverSearch_.selected >= 0 &&
                           coverSearch_.selected < static_cast<int>(search.names.size()) &&
                           search.previewName == search.names[static_cast<size_t>(coverSearch_.selected)];
        if (shown) {
            float w = previewW, h = previewW * static_cast<float>(search.height) / static_cast<float>(search.width);
            if (h > boxH) {
                h = boxH;
                w = boxH * static_cast<float>(search.width) / static_cast<float>(search.height);
            }
            const ImVec2 a(pos.x + (previewW - w) * 0.5f, pos.y + (boxH - h) * 0.5f);
            draw->AddImage(static_cast<ImTextureID>(search.preview), a, ImVec2(a.x + w, a.y + h));
        }
        ImGui::Dummy(ImVec2(previewW, boxH));
        ImGui::BeginDisabled(!shown || search.busy);
        if (ImGui::Button("Usar esta carátula", ImVec2(previewW, 0)) && library_.useCover(coverSearch_.game))
            showCoverSearch_ = false;
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::End();
    } else if (showCoverSearch_) {
        ImGui::End();
    }
    if (!showCoverSearch_) library_.endCoverSearch();
}

// The Mega CD BIOS of each region. plumsg cannot include them: they are Sega's.
void Ui::drawBios(UiActions& out) {
    ImGui::SetNextWindowSize(ImVec2(680, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("BIOS del Mega CD", &showBios_)) {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowAppearing()) biosChecked_.clear();  // the files may have changed meanwhile
    ImGui::TextWrapped("El Mega CD necesita la BIOS de la consola para arrancar los discos, y plumsg no la "
                       "incluye. Indica dónde tienes la de cada región (un archivo de 128 KB, como "
                       "bios_CD_U.bin); cada disco usa la de su región.");
    ImGui::Spacing();
    static const char* regionNames[3] = {"Japón (Mega-CD)", "EE. UU. (Sega CD)", "Europa (Mega-CD)"};
    static const Region regions[3] = {Region::JapanNtsc, Region::UsaNtsc, Region::EuropePal};
    if (ImGui::BeginTable("bios", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Región", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Archivo");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < 3; ++i) {
            std::string& path = settings_.cdBios[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(regionNames[i]);
            ImGui::TableNextColumn();
            if (path.empty()) {
                ImGui::TextDisabled("Sin elegir");
            } else {
                ImGui::TextUnformatted(fileName(path).c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
                auto checked = biosChecked_.find(path);
                if (checked == biosChecked_.end()) checked = biosChecked_.emplace(path, identifyCdBiosFile(path)).first;
                const BiosInfo& info = checked->second;
                if (!info.valid) {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "No parece una BIOS: %s", info.problem.c_str());
                } else if (info.region != Region::Auto && info.region != regions[i]) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Parece de otra región");
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "Correcta%s%s", info.name.empty() ? "" : ": ",
                                       info.name.c_str());
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::Button("Elegir...")) {
                biosSlot_ = i;
                browseFor_ = BrowseFor::CdBios;
                std::string start = path.empty() ? (settings_.libraryFolders.empty() ? settings_.lastFolder
                                                                                     : settings_.libraryFolders[0])
                                                 : path.substr(0, path.find_last_of("/\\"));
                std::string chosen;
                const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
                switch (fullscreen ? DialogResult::Unavailable : openFileDialog(window_, start, Pick::AnyFile, &chosen)) {
                    case DialogResult::Chosen:
                        path = chosen;
                        out.settingsChanged = true;
                        break;
                    case DialogResult::Cancelled: break;
                    case DialogResult::Unavailable: browser_.open(start, Pick::AnyFile); break;
                }
            }
            if (!path.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Quitar")) {
                    path.clear();
                    out.settingsChanged = true;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(settings_.libraryFolders.empty());
    if (ImGui::Button("Buscar en las carpetas de la biblioteca")) {
        const std::array<std::string, 3> found = findCdBios(settings_.libraryFolders);
        int added = 0;
        for (size_t i = 0; i < 3; ++i)
            if (!found[i].empty() && found[i] != settings_.cdBios[i]) {
                settings_.cdBios[i] = found[i];
                ++added;
            }
        if (added) out.settingsChanged = true;
        const int total = static_cast<int>(!found[0].empty()) + !found[1].empty() + !found[2].empty();
        biosStatus_ = total == 0 ? "No se ha encontrado ninguna BIOS del Mega CD en esas carpetas."
                                 : "Encontradas " + std::to_string(total) + " de 3.";
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Busca archivos de 128 KB con la cabecera de la BIOS del Mega CD en las carpetas\n"
                          "de la biblioteca y sus subcarpetas, y elige una para cada región.");
    if (!biosStatus_.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(biosStatus_.c_str());
    }
    ImGui::End();
}

void Ui::drawNetplay(const UiState& state, UiActions& out) {
    if (!ImGui::Begin("Jugar en red", &showNetplay_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
    if (ImGui::IsWindowAppearing()) {
        localAddresses_ = UdpTransport::localAddresses();
        std::snprintf(netAddress_, sizeof netAddress_, "%s", settings_.netAddress.c_str());
    }
    if (state.netSession) {
        ImGui::TextWrapped("%s", state.netStatus.c_str());
        if (state.netProgress >= 0) ImGui::ProgressBar(state.netProgress, ImVec2(ImGui::GetFontSize() * 36.0f, 0));
        ImGui::Spacing();
        if (ImGui::Button("Salir de la partida")) out.netLeave = true;
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Dos jugadores, cada uno en su ordenador y con el mismo juego abierto (la misma ROM). "
                       "Quien aloja es el jugador 1 y quien se une, el jugador 2; cada uno juega con sus "
                       "controles del jugador 1. La partida empieza donde la tenga el anfitrión.");
    if (!state.romLoaded)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Abre primero el juego.");
    ImGui::BeginDisabled(!state.romLoaded);

    ImGui::SeparatorText("Alojar la partida (jugador 1)");
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Puerto UDP", &settings_.netPort, 0, 0)) settings_.netPort = std::clamp(settings_.netPort, 1, 65535);
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
    ImGui::SameLine();
    if (ImGui::Button("Alojar")) out.netHost = out.settingsChanged = true;
    if (!localAddresses_.empty())
        ImGui::TextWrapped("En casa, tu amigo se une a %s:%d.", localAddresses_.front().c_str(), settings_.netPort);
    ImGui::TextDisabled("Por internet: abre (redirige) el puerto UDP %d de tu router hacia este ordenador y "
                        "dale tu IP pública; o usad los dos una red privada como Tailscale o ZeroTier "
                        "y no hace falta abrir nada.", settings_.netPort);

    ImGui::SeparatorText("Unirse a una partida (jugador 2)");
    ImGui::SetNextItemWidth(300);
    const bool enter = ImGui::InputTextWithHint("Anfitrión", "IP o nombre, y :puerto si no es el de arriba",
                                                netAddress_, sizeof netAddress_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Unirse") || enter) && netAddress_[0]) {
        settings_.netAddress = netAddress_;
        out.netJoin = out.settingsChanged = true;
    }

    ImGui::SeparatorText("Ajustes");
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Retardo de los mandos", &settings_.netInputDelay, 0, 6, "%d fotogramas", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) out.settingsChanged = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Cuántos fotogramas tarda en hacer efecto lo que pulsas. Con más retardo hay menos\n"
                          "saltos cuando la conexión es lenta; con 0 responde al instante pero salta más.\n"
                          "Vale el del anfitrión. 2 va bien casi siempre.");
    ImGui::EndDisabled();
    ImGui::PopTextWrapPos();
    ImGui::End();
}

void Ui::drawCheats(const UiState& state, UiActions& out) {
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (cheatsRefit_ > 0) ImGui::SetNextWindowSize(ImVec2(cheatsWidth_, 0));  // grow to fit the list that arrived
    if (!ImGui::Begin("Trucos", &showCheats_)) {
        ImGui::End();
        return;
    }
    if (!state.cheats) {
        ImGui::TextDisabled("Abre una ROM, o elige un juego en la biblioteca con el botón derecho.");
        ImGui::End();
        return;
    }
    std::vector<Cheat>& cheats = *state.cheats;
    ImGui::TextWrapped("Códigos Game Genie (ABCD-EFGH) o Action Replay (FF0A12:0009) de %s. "
                       "Se guardan para este juego y se activan al abrirlo.", state.cheatsGame.c_str());
    ImGui::Spacing();
    int remove = -1;
    if (cheats.empty()) ImGui::TextDisabled("Este juego no tiene trucos todavía.");
    else if (ImGui::BeginTable("trucos", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("Código", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Nombre");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        for (int i = 0; i < static_cast<int>(cheats.size()); ++i) {
            Cheat& c = cheats[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##activo", &c.enabled)) out.cheatsChanged = true;
            ImGui::TableNextColumn();
            std::vector<CheatPatch> patches;
            std::string error;
            const bool valid = decodeCheatCodes(c.code, &patches, &error);
            const std::string shown = shortCode(c.code);
            if (valid) ImGui::TextUnformatted(shown.c_str());
            else ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", shown.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", valid ? describePatches(patches).c_str() : error.c_str());
            ImGui::TableNextColumn();
            char name[96];
            std::snprintf(name, sizeof name, "%s", c.name.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##nombre", name, sizeof name)) c.name = name;
            if (ImGui::IsItemDeactivatedAfterEdit()) out.cheatsChanged = true;
            ImGui::TableNextColumn();
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) remove = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) {
        cheats.erase(cheats.begin() + remove);
        out.cheatsChanged = true;
    }

    ImGui::SeparatorText("Añadir");
    ImGui::SetNextItemWidth(130);
    bool add = ImGui::InputTextWithHint("##codigo", "Código", cheatCode_, sizeof cheatCode_,
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Añadir").x - ImGui::GetStyle().FramePadding.x * 2 -
                            ImGui::GetStyle().ItemSpacing.x);
    add |= ImGui::InputTextWithHint("##nombre_nuevo", "Nombre (opcional)", cheatName_, sizeof cheatName_,
                                    ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    add |= ImGui::Button("Añadir");
    if (add) {
        std::vector<CheatPatch> patches;
        if (decodeCheatCodes(cheatCode_, &patches, &cheatError_)) {
            cheats.push_back({normalizeCheat(cheatCode_), cheatName_, true});
            cheatCode_[0] = cheatName_[0] = '\0';
            cheatError_.clear();
            out.cheatsChanged = true;
        }
    }
    if (!cheatError_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", cheatError_.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Los Game Genie cambian la ROM; los Action Replay fijan un valor en la RAM en cada cuadro. "
                       "Algunos juegos comprueban la ROM al arrancar: si no arranca con un truco, reinicia sin él.");
    ImGui::PopStyleColor();
    drawCheatDatabase(state, out);
    ImGui::End();
}

// The game's ready-made cheat lists from libretro-database, looked for when the window
// is opened for it: pick a list (the one named like the ROM comes first) and add codes.
void Ui::drawCheatDatabase(const UiState& state, UiActions& out) {
    ImGui::SeparatorText("Trucos de internet");
    if (cheatDb_.view().game != state.cheatsPath) {
        cheatFilter_[0] = '\0';
        cheatDb_.search(state.cheatsPath);
    }
    const bool wasBusy = cheatDb_.view().busy;
    cheatDb_.update();
    const CheatDatabase::View& db = cheatDb_.view();
    // Two frames: the first lays the list out, the second fits the window to it.
    if (cheatsRefit_ > 0) --cheatsRefit_;
    if (wasBusy && !db.busy) cheatsRefit_ = 2;
    cheatsWidth_ = ImGui::GetWindowWidth();
    std::vector<Cheat>& cheats = *state.cheats;

    if (db.files.size() > 1 || (!db.files.empty() && db.file != db.files.front())) {
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Lista").x - ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::BeginCombo("Lista", db.file.c_str())) {
            for (const std::string& f : db.files)
                if (ImGui::Selectable(f.c_str(), f == db.file)) cheatDb_.load(f);
            ImGui::EndCombo();
        }
    } else if (!db.file.empty()) {
        ImGui::TextDisabled("Lista: %s", db.file.c_str());
    }
    if (db.busy) {
        static const char* dots[] = {"", ".", "..", "..."};
        ImGui::TextDisabled("Buscando en la base de trucos de libretro%s", dots[(SDL_GetTicks() / 300) % 4]);
    }
    if (!db.status.empty()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", db.status.c_str());
    if (!db.busy && !db.status.empty() && ImGui::Button("Reintentar")) cheatDb_.search(state.cheatsPath);

    if (!db.cheats.empty()) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filtro", "Filtrar (lives, energy, invincible...)", cheatFilter_, sizeof cheatFilter_);
        const std::string filter = searchKey(cheatFilter_);
        const float rowH = ImGui::GetFrameHeightWithSpacing();
        const float height = std::min(static_cast<float>(db.cheats.size()) * rowH + rowH, rowH * 9.5f);
        if (ImGui::BeginTable("trucos_libretro", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                              ImVec2(0, height))) {
            const float addW = ImGui::CalcTextSize("Añadido").x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Truco");
            ImGui::TableSetupColumn("Código", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, addW);
            ImGui::TableHeadersRow();
            int shown = 0;
            for (size_t i = 0; i < db.cheats.size(); ++i) {
                const Cheat& c = db.cheats[i];
                if (!filter.empty() && searchKey(c.name).find(filter) == std::string::npos) continue;
                ++shown;
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(c.name.empty() ? "(sin nombre)" : c.name.c_str());
                if (ImGui::IsItemHovered() && ImGui::CalcTextSize(c.name.c_str()).x > ImGui::GetColumnWidth())
                    ImGui::SetTooltip("%s", c.name.c_str());
                ImGui::TableNextColumn();
                std::vector<CheatPatch> patches;
                std::string error;
                const bool valid = decodeCheatCodes(c.code, &patches, &error);
                const std::string code = shortCode(c.code);
                ImGui::AlignTextToFramePadding();
                if (valid) ImGui::TextUnformatted(code.c_str());
                else ImGui::TextDisabled("%s", code.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", valid ? describePatches(patches).c_str() : error.c_str());
                ImGui::TableNextColumn();
                bool added = false;
                for (const Cheat& mine : cheats) added |= mine.code == c.code;
                if (!valid) {
                    ImGui::TextDisabled("No vale");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", error.c_str());
                } else if (added) {
                    ImGui::BeginDisabled();
                    ImGui::Button("Añadido", ImVec2(-1, 0));
                    ImGui::EndDisabled();
                } else if (ImGui::Button("Añadir", ImVec2(-1, 0))) {
                    cheats.push_back({c.code, c.name, true});
                    out.cheatsChanged = true;
                    // Many games check the ROM and stop (Sonic's red screen) unless the
                    // list's master code goes with the codes that patch it.
                    bool patchesRom = false;
                    for (const CheatPatch& p : patches) patchesRom |= p.address < 0x400000;
                    for (const Cheat& m : db.cheats) {
                        if (!patchesRom || searchKey(m.name).find("master code") == std::string::npos) continue;
                        bool have = false;
                        for (const Cheat& mine : cheats) have |= mine.code == m.code;
                        if (!have) {
                            cheats.push_back({m.code, m.name, true});
                            notify("Añadido también su código maestro, sin el que el juego no arrancaría");
                        }
                    }
                }
                ImGui::PopID();
            }
            if (shown == 0) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Ningún truco tiene ese nombre.");
            }
            ImGui::EndTable();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Listas de libretro-database, la base de trucos de RetroArch (licencia CC BY-SA 4.0). "
                       "Los nombres vienen en inglés; los añadidos quedan activados en la lista de arriba, "
                       "con el código maestro (Master Code) del juego cuando lo pide.");
    ImGui::PopStyleColor();
}


// The game's own console buttons, which take the place of the general ones (Opciones >
// Controles) while it is loaded. The hotkeys and the stick stay the general ones.
void Ui::drawGameControls(const GameOptionsView& v, UiActions& out) {
    std::set<std::string>& own = *v.own;
    bool mine = own.count(kGameControls) != 0;
    if (ImGui::Checkbox("Botones propios de este juego", &mine)) {
        if (mine) {
            own.insert(kGameControls);
            v.controls->copyButtons(input_.config());  // starting from the general ones
        } else {
            own.erase(kGameControls);
            if (capture_.game) capture_.active = false;
        }
        out.gameOptionsChanged = true;
    }
    if (!mine) {
        ImGui::TextDisabled("Usa los de Opciones > Controles.");
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Volver a los generales")) {
        own.erase(kGameControls);
        if (capture_.game) capture_.active = false;
        out.gameOptionsChanged = true;
        return;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Deja de usar estos y vuelve a los de Opciones > Controles");
    ImGui::TextWrapped("Pulsa una casilla y luego la tecla o el botón del mando. \"+\" añade otro, "
                       "\"x\" lo borra, Esc cancela.");
    if (drawBindings(*v.controls, true)) out.gameOptionsChanged = true;
}

// The options this game keeps a value of its own for, in two tabs: Ajustes and Controles.
void Ui::drawGameOptions(const UiState& state, UiActions& out) {
    ImGui::SetNextWindowSize(ImVec2(700, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Opciones de este juego", &showGameOptions_)) {
        ImGui::End();
        return;
    }
    if (!showGameOptions_ && capture_.game) capture_.active = false;
    const GameOptionsView& v = state.gameOptions;
    if (gameControls_ != v.controls && capture_.game) capture_.active = false;  // another game
    gameControls_ = v.controls;
    if (v.game.empty() || !v.values || !v.own || !v.general) {
        ImGui::TextDisabled("Abre un juego, o elige uno en la biblioteca con el botón derecho.");
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Opciones propias de %s. Marca una opción para que este juego use su propio valor; "
                       "las que no marques siguen a las generales.", v.game.c_str());
    if (v.loaded && state.netPlaying) ImGui::TextDisabled("En una partida en red no se pueden cambiar.");
    ImGui::BeginDisabled(v.loaded && state.netPlaying);
    if (ImGui::BeginTabBar("opciones_juego")) {
        if (ImGui::BeginTabItem("Ajustes")) {
            drawGameSettings(state, out);
            ImGui::EndTabItem();
        }
        if (v.controls && ImGui::BeginTabItem("Controles", nullptr, gameControlsTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawGameControls(v, out);
            ImGui::EndTabItem();
        }
        gameControlsTab_ = false;
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    ImGui::End();
}

// The Ajustes tab: each option has a box; ticked, the game has its own value (edited
// here); unticked, it follows the general one (shown greyed).
void Ui::drawGameSettings(const UiState& state, UiActions& out) {
    const GameOptionsView& v = state.gameOptions;
    Settings& s = *v.values;
    std::set<std::string>& own = *v.own;
    std::string group;
    for (const GameOption& opt : gameOptions()) {
        const std::string key = opt.key;
        if (group != opt.group) {
            group = opt.group;
            ImGui::SeparatorText(opt.group);
        }
        ImGui::PushID(opt.key);
        bool mine = own.count(key) != 0;
        bool edited = false;
        if (ImGui::Checkbox("##propio", &mine)) {
            if (mine) own.insert(key);
            else {
                own.erase(key);
                opt.copy(*v.general, s);
            }
            edited = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(mine ? "Propio de este juego" : "Usa el valor general");
        ImGui::SameLine();
        ImGui::BeginDisabled(!mine);
        ImGui::SetNextItemWidth(220);
        if (key == "vista") {
            if (ImGui::BeginCombo(opt.label, s.view.c_str())) {
                for (const char* view : state.views)
                    if (ImGui::Selectable(view, s.view == view)) {
                        s.view = view;
                        edited = true;
                    }
                ImGui::EndCombo();
            }
        } else if (key == "escalado") {
            static const char* names[] = {"Escala entera", "Ajustar", "Estirar"};
            int i = static_cast<int>(s.render.scale);
            if (ImGui::Combo(opt.label, &i, names, 3)) {
                s.render.scale = static_cast<ScaleMode>(i);
                edited = true;
            }
        } else if (key == "suavizado") {
            edited |= ImGui::Checkbox("Suavizar píxeles", &s.render.smooth);
        } else if (key == "filtro") {
            static const char* names[] = {"Ninguno", "Scanlines", "CRT"};
            int i = static_cast<int>(s.render.filter);
            if (ImGui::Combo(opt.label, &i, names, 3)) {
                s.render.filter = static_cast<Filter2D>(i);
                edited = true;
            }
        } else if (key == "intensidad_filtro") {
            float strength = s.render.filterStrength * 100.0f;
            if (ImGui::SliderFloat(opt.label, &strength, 0.0f, 100.0f, "%.0f %%")) s.render.filterStrength = strength / 100.0f;
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else if (key == "pixeles_cuadrados") {
            edited |= ImGui::Checkbox("Píxeles cuadrados (sin estirar a 4:3)", &s.render.squarePixels);
        } else if (key == "perfil_3d") {
            ImGui::TextUnformatted(opt.label);
            ImGui::SameLine();
            ImGui::TextDisabled(v.loaded ? "(se ajustan en Opciones > Vídeo)" : "(se ajustan en Vídeo con el juego abierto)");
        } else if (key == "volumen") {
            ImGui::SliderInt(opt.label, &s.volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else if (key == "silencio") {
            edited |= ImGui::Checkbox("Silenciar", &s.mute);
        } else if (key == "modo") {
            int i = s.roland ? 1 : 0;
            static const char* names[] = {"Original (YM2612 y PSG)", "Sintetizador (SoundFont)"};
            if (ImGui::Combo(opt.label, &i, names, 2)) {
                s.roland = i == 1;
                edited = true;
            }
        } else if (key == "soundfont") {
            char path[512];
            std::snprintf(path, sizeof path, "%s", s.soundfont.c_str());
            if (ImGui::InputTextWithHint(opt.label, "el incluido", path, sizeof path)) s.soundfont = path;
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else if (key == "mezcla_chip" || key == "reverb" || key == "chorus") {
            int& v = key == "mezcla_chip" ? s.chipMix : key == "reverb" ? s.reverb : s.chorus;
            ImGui::SliderInt(opt.label, &v, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else if (key == "efectos") {
            edited |= ImGui::Checkbox("Efectos de sonido originales", &s.effectsOriginal);
        } else if (key == "region") {
            static const char* names[] = {"Automática (la de la ROM)", "Japón (NTSC)", "EE. UU. (NTSC)", "Europa (PAL)"};
            int i = static_cast<int>(s.region);
            if (ImGui::Combo(opt.label, &i, names, 4)) {
                s.region = static_cast<Region>(i);
                edited = true;
            }
        } else if (key == "mandos") {
            static const char* names[] = {"3 botones", "6 botones"};
            for (size_t port = 0; port < 2; ++port) {
                if (port) ImGui::SameLine();
                int i = s.padType[port] == PadType::ThreeButton ? 0 : 1;
                ImGui::SetNextItemWidth(110);
                if (ImGui::Combo(port ? "Mando 2" : "Mando 1", &i, names, 2)) {
                    s.padType[port] = i == 0 ? PadType::ThreeButton : PadType::SixButton;
                    edited = true;
                }
            }
        } else if (key == "rebobinado_segundos") {
            ImGui::SliderInt(opt.label, &s.rewindSeconds, 0, Settings::kMaxRewindSeconds,
                             s.rewindSeconds ? "%d s" : "desactivado", ImGuiSliderFlags_AlwaysClamp);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else if (key == "avance_rapido") {
            ImGui::SliderInt(opt.label, &s.fastForward, 2, 10, "%dx", ImGuiSliderFlags_AlwaysClamp);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        } else if (key == "camara_lenta") {
            ImGui::SliderInt(opt.label, &s.slowMotion, 10, 90, "%d%%", ImGuiSliderFlags_AlwaysClamp);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        if (!edited) continue;
        out.gameOptionsChanged = true;
        if (v.loaded && (key == "region" || key == "mandos")) out.consoleChanged = true;
        if (v.loaded && key == "rebobinado_segundos") out.rewindChanged = true;
        if (v.loaded && std::string(opt.group) == "Sonido") out.soundChanged = true;
    }
}


}  // namespace plumsg
