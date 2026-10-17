#pragma once

#include <SDL.h>

#include <array>
#include <ostream>
#include <string>
#include <vector>

#include "../core/io.h"

namespace plumsg {

enum class MdButton { Up, Down, Left, Right, A, B, C, Start, X, Y, Z, Mode, Count };

// Emulator actions that can be bound like console buttons but fire once per press.
enum class Hotkey { Toggle3D, SaveState, LoadState, PrevSlot, NextSlot, StateMenu, Count };

// Keyboard and gamepad bindings, read from and written to a plain-text .ini file.
// The keyboard can drive both players ([teclado] and [teclado2]); the first gamepad
// is player 1 and the second player 2, both with the [mando] buttons.
struct InputConfig {
    static constexpr int kButtons = static_cast<int>(MdButton::Count);
    static constexpr int kHotkeys = static_cast<int>(Hotkey::Count);
    std::array<std::vector<SDL_Scancode>, kButtons> keys;
    std::array<std::vector<SDL_Scancode>, kButtons> keys2;
    std::array<std::vector<SDL_GameControllerButton>, kButtons> pad;
    std::array<std::vector<SDL_Scancode>, kHotkeys> hotkeyKeys;
    std::array<std::vector<SDL_GameControllerButton>, kHotkeys> hotkeyPad;
    bool stickAsDpad = true;
    int stickDeadzone = 12000;

    static InputConfig defaults();
    // Loads `path`; if it does not exist, writes the defaults there so the user can edit them.
    static InputConfig loadOrCreate(const std::string& path);
    bool save(const std::string& path) const;
    // Writes the [teclado] and [mando] sections, so other settings can share the file.
    void write(std::ostream& out) const;
};

// Tracks connected controllers and turns the keyboard and pads into both players' pads.
class Input {
public:
    explicit Input(InputConfig config) : config_(std::move(config)) {}
    ~Input();

    void openAll();
    void handleEvent(const SDL_Event& e);
    // `useKeyboard` = false ignores the keyboard (e.g. while a menu has focus).
    PadState poll(int player, bool useKeyboard = true) const;
    // The hotkey this key or pad button press triggers, or Hotkey::Count if none.
    Hotkey hotkeyFor(const SDL_Event& e) const;
    // Right stick of the first pad that has it pushed past the dead zone, each axis -1..1
    // (y positive down); 0,0 when none is. Turns the pseudo-3D camera.
    void rightStick(float* x, float* y) const;

    const InputConfig& config() const { return config_; }
    void setConfig(InputConfig config) { config_ = std::move(config); }
    std::vector<std::string> padNames() const;

private:
    InputConfig config_;
    std::vector<SDL_GameController*> pads_;
};

}  // namespace plumsg
