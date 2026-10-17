#pragma once

#include <SDL.h>

#include <string>
#include <vector>

#include "file_dialog.h"
#include "input.h"
#include "settings.h"

namespace plumsg {

// What the user asked for this frame; main() carries it out.
struct UiActions {
    std::string load;            // ROM to open
    bool reset = false;
    bool togglePause = false;
    bool quit = false;
    int windowScale = 0;         // resize the window to this many times 320x224
    int saveState = -1;          // slot to save to / load from, 0-9 (-1 none)
    int loadState = -1;
    bool consoleChanged = false;  // region or pad types changed
    bool fullscreenChanged = false;
    bool settingsChanged = false;  // something in Settings or the controls changed: save the .ini
};

// What the Estado menu shows for each savestate slot.
struct SlotInfo {
    bool used = false;
    std::string when;          // date and time it was saved
    unsigned int texture = 0;  // OpenGL texture with its thumbnail, 0 if none
    int width = 0, height = 0;
};

struct UiState {
    bool romLoaded = false;
    bool paused = false;
    std::vector<const char*> views;  // Renderer::name() of each available view
    std::vector<SlotInfo> slots;     // one per slot, 0-9
};

// Menu bar and option windows, drawn with Dear ImGui (OpenGL 3) on top of the game.
class Ui {
public:
    Ui(SDL_Window* window, SDL_GLContext context, Settings& settings, Input& input);
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // Feeds the event to ImGui. Returns true when the UI used it up (a binding being
    // assigned), so the emulator should ignore it.
    bool handleEvent(const SDL_Event& e);
    // True while the keyboard belongs to the UI rather than the game.
    bool keyboardBusy() const;
    // True while the mouse is over a menu or window.
    bool mouseBusy() const;

    // Builds this frame's interface. Call once per frame before drawing the game.
    UiActions frame(const UiState& state);
    // Pixels at the top of the window covered by the menu bar.
    int menuHeight() const { return menuHeight_; }
    // Draws the interface over whatever is already on screen.
    void render();

    void openRom() { openRequested_ = true; }
    // The states menu: a list of the slots that the pad (or the keyboard) can drive.
    void toggleStateMenu();
    bool stateMenuOpen() const { return stateMenu_.open; }
    void notify(std::string message);

private:
    struct Capture {
        bool active = false;
        int row = 0;          // 0..kButtons-1 console buttons, then hotkeys
        int column = 0;       // 0 keyboard player 1, 1 keyboard player 2, 2 gamepad
        bool append = false;  // add to the existing bindings instead of replacing them
    };

    void drawMenuBar(const UiState& state, UiActions& out);
    void drawControls(UiActions& out);
    void drawVideo(const UiState& state, UiActions& out);
    void drawAbout();
    void drawStateMenu(const UiState& state);
    bool handleStateMenu(const SDL_Event& e);
    void drawOverlays(const UiState& state);
    void finishCapture(int keyOrButton);

    SDL_Window* window_;
    Settings& settings_;
    Input& input_;
    FileBrowser browser_;

    bool showControls_ = false;
    bool showVideo_ = false;
    bool showAbout_ = false;
    bool openRequested_ = false;
    bool pendingSave_ = false;  // a binding changed outside frame()
    bool peekMenu_ = false;  // menu bar hidden but shown while the mouse is at the top
    int menuHeight_ = 0;
    Capture capture_;
    struct StateMenu {
        bool open = false;
        int selected = 0;
        int slots = 0;
        bool scroll = false;  // bring the selected row into view on the next frame
        int load = -1, save = -1;  // chosen this frame, handed over by frame()
    } stateMenu_;
    std::string toast_;
    Uint32 toastUntil_ = 0;
};

}  // namespace plumsg
