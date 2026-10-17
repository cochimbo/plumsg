#include "input.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace plumsg {

namespace {

// Names used in the [teclado] / [mando] sections of the .ini file.
constexpr const char* kButtonNames[InputConfig::kButtons] = {"arriba", "abajo", "izquierda", "derecha", "a", "b",
                                                             "c", "start", "x", "y", "z", "mode"};
constexpr const char* kHotkeyNames[InputConfig::kHotkeys] = {
    "efecto_3d", "guardar_estado", "cargar_estado", "ranura_anterior", "ranura_siguiente",
    "menu_estados", "rebobinar", "avance_rapido", "camara_lenta", "biblioteca"};

constexpr int kTriggerThreshold = 16000;  // of 32767: a trigger counts as pressed past halfway

bool padDown(SDL_GameController* pad, SDL_GameControllerButton b) {
    if (b == kPadLeftTrigger) return SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerThreshold;
    if (b == kPadRightTrigger) return SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerThreshold;
    return SDL_GameControllerGetButton(pad, b) != 0;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

int buttonIndex(const std::string& name) {
    for (int i = 0; i < InputConfig::kButtons; ++i)
        if (name == kButtonNames[i]) return i;
    return -1;
}

int hotkeyIndex(const std::string& name) {
    for (int i = 0; i < InputConfig::kHotkeys; ++i)
        if (name == kHotkeyNames[i]) return i;
    return -1;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

}  // namespace

const char* padButtonName(SDL_GameControllerButton b) {
    if (b == kPadLeftTrigger) return "lefttrigger";
    if (b == kPadRightTrigger) return "righttrigger";
    const char* name = SDL_GameControllerGetStringForButton(b);
    return name ? name : "?";
}

SDL_GameControllerButton padButtonFromName(const std::string& name) {
    const std::string n = lower(name);
    if (n == "lefttrigger") return kPadLeftTrigger;
    if (n == "righttrigger") return kPadRightTrigger;
    return SDL_GameControllerGetButtonFromString(n.c_str());
}

SDL_GameControllerButton triggerPulled(const SDL_Event& e) {
    if (e.type != SDL_CONTROLLERAXISMOTION || e.caxis.value <= kTriggerThreshold) return SDL_CONTROLLER_BUTTON_INVALID;
    if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) return kPadLeftTrigger;
    if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return kPadRightTrigger;
    return SDL_CONTROLLER_BUTTON_INVALID;
}

InputConfig InputConfig::defaults() {
    InputConfig c;
    auto set = [&](MdButton b, std::vector<SDL_Scancode> k, std::vector<SDL_GameControllerButton> p) {
        c.keys[static_cast<int>(b)] = std::move(k);
        c.pad[static_cast<int>(b)] = std::move(p);
    };
    set(MdButton::Up, {SDL_SCANCODE_UP}, {SDL_CONTROLLER_BUTTON_DPAD_UP});
    set(MdButton::Down, {SDL_SCANCODE_DOWN}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN});
    set(MdButton::Left, {SDL_SCANCODE_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT});
    set(MdButton::Right, {SDL_SCANCODE_RIGHT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT});
    set(MdButton::A, {SDL_SCANCODE_A}, {SDL_CONTROLLER_BUTTON_X});
    set(MdButton::B, {SDL_SCANCODE_S}, {SDL_CONTROLLER_BUTTON_A});
    set(MdButton::C, {SDL_SCANCODE_D}, {SDL_CONTROLLER_BUTTON_B});
    set(MdButton::Start, {SDL_SCANCODE_RETURN}, {SDL_CONTROLLER_BUTTON_START});
    set(MdButton::X, {SDL_SCANCODE_Q}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER});
    set(MdButton::Y, {SDL_SCANCODE_W}, {SDL_CONTROLLER_BUTTON_Y});
    set(MdButton::Z, {SDL_SCANCODE_E}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER});
    set(MdButton::Mode, {SDL_SCANCODE_RSHIFT}, {});
    c.hotkeyKeys[static_cast<int>(Hotkey::Toggle3D)] = {SDL_SCANCODE_F3};
    c.hotkeyPad[static_cast<int>(Hotkey::Toggle3D)] = {SDL_CONTROLLER_BUTTON_BACK};
    c.hotkeyKeys[static_cast<int>(Hotkey::SaveState)] = {SDL_SCANCODE_F5};
    c.hotkeyKeys[static_cast<int>(Hotkey::LoadState)] = {SDL_SCANCODE_F8};
    c.hotkeyKeys[static_cast<int>(Hotkey::PrevSlot)] = {SDL_SCANCODE_F6};
    c.hotkeyKeys[static_cast<int>(Hotkey::NextSlot)] = {SDL_SCANCODE_F7};
    c.hotkeyKeys[static_cast<int>(Hotkey::StateMenu)] = {SDL_SCANCODE_F4};
    c.hotkeyPad[static_cast<int>(Hotkey::StateMenu)] = {SDL_CONTROLLER_BUTTON_LEFTSTICK};
    c.hotkeyKeys[static_cast<int>(Hotkey::Rewind)] = {SDL_SCANCODE_BACKSPACE};
    c.hotkeyPad[static_cast<int>(Hotkey::Rewind)] = {kPadLeftTrigger};
    c.hotkeyKeys[static_cast<int>(Hotkey::FastForward)] = {SDL_SCANCODE_TAB};
    c.hotkeyPad[static_cast<int>(Hotkey::FastForward)] = {kPadRightTrigger};
    c.hotkeyKeys[static_cast<int>(Hotkey::SlowMotion)] = {SDL_SCANCODE_F9};
    c.hotkeyKeys[static_cast<int>(Hotkey::Library)] = {SDL_SCANCODE_F10};
    return c;
}

InputConfig InputConfig::loadOrCreate(const std::string& path) {
    InputConfig c = defaults();
    std::ifstream in(path);
    if (!in) {
        c.save(path);
        return c;
    }
    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));

        if (section == "mando" && key == "stick_como_cruceta") {
            c.stickAsDpad = value == "1" || lower(value) == "si" || lower(value) == "true";
            continue;
        }
        if (section == "mando" && key == "zona_muerta") {
            c.stickDeadzone = std::clamp(std::atoi(value.c_str()), 0, 32767);
            continue;
        }
        const int b = buttonIndex(key);
        const int h = hotkeyIndex(key);
        if (b < 0 && h < 0) continue;
        if (section == "teclado2" && b >= 0) {
            auto& list = c.keys2[b];
            list.clear();
            for (const auto& name : splitList(value)) {
                SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
                if (sc != SDL_SCANCODE_UNKNOWN) list.push_back(sc);
            }
        } else if (section == "teclado") {
            auto& list = b >= 0 ? c.keys[b] : c.hotkeyKeys[h];
            list.clear();
            for (const auto& name : splitList(value)) {
                SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
                if (sc != SDL_SCANCODE_UNKNOWN) list.push_back(sc);
            }
        } else if (section == "mando") {
            auto& list = b >= 0 ? c.pad[b] : c.hotkeyPad[h];
            list.clear();
            for (const auto& name : splitList(value)) {
                SDL_GameControllerButton gb = padButtonFromName(name);
                if (gb != SDL_CONTROLLER_BUTTON_INVALID) list.push_back(gb);
            }
        }
    }
    return c;
}

bool InputConfig::save(const std::string& path) const {
    std::ofstream out(path);
    if (!out) return false;
    write(out);
    return static_cast<bool>(out);
}

void InputConfig::write(std::ostream& out) const {
    auto keyList = [&](const char* name, const std::vector<SDL_Scancode>& list) {
        out << name << " = ";
        for (size_t k = 0; k < list.size(); ++k) out << (k ? ", " : "") << SDL_GetScancodeName(list[k]);
        out << "\n";
    };
    auto padList = [&](const char* name, const std::vector<SDL_GameControllerButton>& list) {
        out << name << " = ";
        for (size_t k = 0; k < list.size(); ++k) out << (k ? ", " : "") << padButtonName(list[k]);
        out << "\n";
    };
    out << "# Controles de plumsg. Puedes poner varias teclas o botones separados por comas.\n"
           "# [teclado] es el jugador 1 y [teclado2] el jugador 2; el primer mando es el\n"
           "# jugador 1 y el segundo, el jugador 2.\n"
           "# Teclas: nombres de SDL (Up, Down, Left, Right, A, S, Return, Space, Right Shift...).\n"
           "# Botones de mando: a, b, x, y, back, guide, start, leftstick, rightstick,\n"
           "# leftshoulder, rightshoulder, lefttrigger, righttrigger, dpup, dpdown, dpleft, dpright.\n"
           "# efecto_3d activa y desactiva el efecto pseudo-3D; guardar_estado, cargar_estado,\n"
           "# ranura_anterior y ranura_siguiente manejan los estados guardados; menu_estados\n"
           "# abre un menú para elegir ranura, guardar y cargar con el mando.\n"
           "# rebobinar y avance_rapido funcionan mientras se mantienen pulsados; camara_lenta\n"
           "# la activa y la desactiva. biblioteca abre la lista de juegos con sus carátulas.\n\n[teclado]\n";
    for (int i = 0; i < kButtons; ++i) keyList(kButtonNames[i], keys[i]);
    for (int i = 0; i < kHotkeys; ++i) keyList(kHotkeyNames[i], hotkeyKeys[i]);
    out << "\n[teclado2]\n";
    for (int i = 0; i < kButtons; ++i) keyList(kButtonNames[i], keys2[i]);
    out << "\n[mando]\n";
    for (int i = 0; i < kButtons; ++i) padList(kButtonNames[i], pad[i]);
    for (int i = 0; i < kHotkeys; ++i) padList(kHotkeyNames[i], hotkeyPad[i]);
    out << "stick_como_cruceta = " << (stickAsDpad ? "si" : "no") << "\n";
    out << "zona_muerta = " << stickDeadzone << "\n";
}

Input::~Input() {
    for (auto* p : pads_) SDL_GameControllerClose(p);
}

void Input::openAll() {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        if (SDL_GameController* p = SDL_GameControllerOpen(i)) pads_.push_back(p);
    }
}

void Input::handleEvent(const SDL_Event& e) {
    pulled_ = SDL_CONTROLLER_BUTTON_INVALID;
    if (e.type == SDL_CONTROLLERAXISMOTION &&
        (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
        auto it = std::find_if(triggers_.begin(), triggers_.end(), [&](const auto& t) { return t.first == e.caxis.which; });
        if (it == triggers_.end()) it = triggers_.insert(triggers_.end(), {e.caxis.which, 0});
        const int bit = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 1 : 2;
        const bool down = e.caxis.value > kTriggerThreshold;
        if (down && !(it->second & bit)) pulled_ = triggerPulled(e);
        it->second = down ? it->second | bit : it->second & ~bit;
        return;
    }
    if (e.type == SDL_CONTROLLERDEVICEADDED) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(e.cdevice.which);
        for (auto* p : pads_)
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == id) return;
        if (SDL_GameController* p = SDL_GameControllerOpen(e.cdevice.which)) pads_.push_back(p);
    } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
        auto it = std::find_if(pads_.begin(), pads_.end(), [&](SDL_GameController* p) {
            return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == e.cdevice.which;
        });
        if (it != pads_.end()) {
            SDL_GameControllerClose(*it);
            pads_.erase(it);
        }
    }
}

Hotkey Input::hotkeyFor(const SDL_Event& e) const {
    for (int h = 0; h < InputConfig::kHotkeys; ++h) {
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            for (SDL_Scancode sc : config_.hotkeyKeys[h])
                if (sc == e.key.keysym.scancode) return static_cast<Hotkey>(h);
        } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            for (SDL_GameControllerButton gb : config_.hotkeyPad[h])
                if (gb == e.cbutton.button) return static_cast<Hotkey>(h);
        } else if (e.type == SDL_CONTROLLERAXISMOTION && pulled_ != SDL_CONTROLLER_BUTTON_INVALID) {
            for (SDL_GameControllerButton gb : config_.hotkeyPad[h])
                if (gb == pulled_) return static_cast<Hotkey>(h);
        }
    }
    return Hotkey::Count;
}

bool Input::held(Hotkey h, bool useKeyboard) const {
    const int i = static_cast<int>(h);
    if (useKeyboard) {
        const Uint8* kb = SDL_GetKeyboardState(nullptr);
        for (SDL_Scancode sc : config_.hotkeyKeys[i])
            if (kb[sc]) return true;
    }
    for (auto* p : pads_)
        for (SDL_GameControllerButton gb : config_.hotkeyPad[i])
            if (padDown(p, gb)) return true;
    return false;
}

std::vector<std::string> Input::padNames() const {
    std::vector<std::string> names;
    for (auto* p : pads_) {
        const char* n = SDL_GameControllerName(p);
        names.push_back(n ? n : "Mando");
    }
    return names;
}

void Input::rightStick(float* x, float* y) const {
    *x = *y = 0;
    for (auto* p : pads_) {
        const int ax = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTX);
        const int ay = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTY);
        if (std::abs(ax) < config_.stickDeadzone && std::abs(ay) < config_.stickDeadzone) continue;
        *x = static_cast<float>(ax) / 32767.0f;
        *y = static_cast<float>(ay) / 32767.0f;
        return;
    }
}

PadState Input::poll(int player, bool useKeyboard) const {
    std::array<bool, InputConfig::kButtons> down{};
    const Uint8* kb = SDL_GetKeyboardState(nullptr);
    const auto& keys = player == 0 ? config_.keys : config_.keys2;
    SDL_GameController* pad = static_cast<size_t>(player) < pads_.size() ? pads_[static_cast<size_t>(player)] : nullptr;
    for (int b = 0; b < InputConfig::kButtons; ++b) {
        if (useKeyboard)
            for (SDL_Scancode sc : keys[b]) down[b] = down[b] || kb[sc];
        if (pad)
            for (SDL_GameControllerButton gb : config_.pad[b]) down[b] = down[b] || padDown(pad, gb);
    }
    if (config_.stickAsDpad && pad) {
        const int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        const int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        down[static_cast<int>(MdButton::Left)] |= x < -config_.stickDeadzone;
        down[static_cast<int>(MdButton::Right)] |= x > config_.stickDeadzone;
        down[static_cast<int>(MdButton::Up)] |= y < -config_.stickDeadzone;
        down[static_cast<int>(MdButton::Down)] |= y > config_.stickDeadzone;
    }
    auto is = [&](MdButton b) { return down[static_cast<int>(b)]; };
    PadState out;
    out.up = is(MdButton::Up);
    out.down = is(MdButton::Down);
    out.left = is(MdButton::Left);
    out.right = is(MdButton::Right);
    out.a = is(MdButton::A);
    out.b = is(MdButton::B);
    out.c = is(MdButton::C);
    out.start = is(MdButton::Start);
    out.x = is(MdButton::X);
    out.y = is(MdButton::Y);
    out.z = is(MdButton::Z);
    out.mode = is(MdButton::Mode);
    return out;
}

}  // namespace plumsg
