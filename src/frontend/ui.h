#pragma once

#include <SDL.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../core/cheats.h"
#include "../core/system.h"
#include "achievements_ui.h"
#include "cheat_database.h"
#include "file_dialog.h"
#include "input.h"
#include "instrument_editor.h"
#include "library.h"
#include "settings.h"

namespace plumsg {

// What the user asked for this frame; main() carries it out.
struct UiActions {
    std::string load;            // ROM to open
    bool reset = false;
    bool stop = false;           // close the game and go back to the start screen
    bool togglePause = false;
    bool quit = false;
    int windowScale = 0;         // resize the window to this many times 320x224
    int saveState = -1;          // slot to save to / load from, 0-9 (-1 none)
    int loadState = -1;
    bool consoleChanged = false;  // region or pad types changed
    bool fullscreenChanged = false;
    bool settingsChanged = false;  // something in Settings or the controls changed: save the .ini
    bool toggleSlowMotion = false;
    bool rewindChanged = false;    // the rewind length changed
    int profile = -1;              // 1 give the game its own 3D profile, 0 drop it, -1 nothing
    int wideProfile = -1;          // the same for its own widescreen
    bool cheatsChanged = false;    // the cheat list was edited: apply it and save it
    bool netHost = false;          // host a netplay game on settings.netPort
    bool netJoin = false;          // join the host at settings.netAddress
    bool netLeave = false;         // leave the netplay game
    std::string cheatsFor;         // open the Trucos window for this game
    std::string optionsFor;        // open the Opciones de este juego window for this game
    bool gameOptionsChanged = false;  // a game's own options were edited: save them (and apply if loaded)
    bool soundChanged = false;        // the Roland sound was switched or retuned
    bool equalizerChanged = false;    // settings.equalizer moved: heard at once, saved with settingsChanged
    bool openEditor = false;          // open the instrument editor for editorFor (empty: no game)
    std::string editorFor;
    bool startRecording = false;      // the editor's requests: record the game's music,
    bool stopRecording = false;       // stop and hand it over,
    bool assignmentsChanged = false;  // and the game's instruments changed
    bool achievementsChanged = false; // RetroAchievements switched on or off, or hardcore
    int record = 0;                   // start or stop recording: 1 a GIF, 2 a video
    bool openRecordings = false;      // show the folder the recordings go to
};

// What the Estado menu shows for each savestate slot.
struct SlotInfo {
    bool used = false;
    std::string when;          // date and time it was saved
    unsigned int texture = 0;  // OpenGL texture with its thumbnail, 0 if none
    int width = 0, height = 0;
};

// What the Opciones de este juego window edits: which options the game has its own
// value of, and the values (the game's where it has its own, the general ones elsewhere).
struct GameOptionsView {
    std::string game;                     // file name, empty when no game was chosen
    Settings* values = nullptr;
    std::set<std::string>* own = nullptr;
    const Settings* general = nullptr;    // to go back to when an option stops being the game's
    InputConfig* controls = nullptr;      // its own console buttons (the general ones are Input's)
    bool loaded = false;                  // the game is the one running: changes apply at once
};

struct UiState {
    bool romLoaded = false;
    bool disc = false;  // a Mega CD disc or an MSU-MD cartridge: its music is CD audio, never Roland
    std::string romPath;
    bool paused = false;
    bool slowMotion = false;
    std::string speed;     // badge in the corner while not at normal speed ("<< REBOBINANDO", "x4"...)
    bool hasProfile = false;  // the loaded game has its own 3D profile
    bool hasWideProfile = false;  // the loaded game has its own widescreen
    std::string gameName;     // the ROM's file name, for the profile option
    std::vector<const char*> views;  // Renderer::name() of each available view
    std::vector<SlotInfo> slots;     // one per slot, 0-9
    std::vector<Cheat>* cheats = nullptr;  // the cheats of the Trucos window's game, edited in place
    std::string cheatsGame;                // that game's file name
    std::string cheatsPath;                // and its path
    bool cheatsSms = false;                // and it takes Master System and Game Gear codes
    GameOptionsView gameOptions;
    // Netplay: a session exists (hosting, joining or playing), and what to say about it.
    bool netSession = false;
    bool netPlaying = false;  // connected: the options that would make the consoles differ are off
    std::string netStatus;
    float netProgress = -1;   // 0..1 while the game is sent, else -1
    // RetroAchievements' hardcore mode is on: no rewinding, slow motion, cheats or loading states.
    bool hardcore = false;
    int recording = 0;         // being recorded: 1 a GIF, 2 a video
    int recordingSeconds = 0;  // and for how long
    EditorView editor;
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
    // The game library: a grid of covers that the pad (or the keyboard) can drive.
    void toggleLibrary();
    bool libraryOpen() const { return showLibrary_; }
    void openBios() { showBios_ = true; }
    // "Buscar carátula..." for the library's game `game`.
    void openCoverSearch(size_t game);
    bool coverSearchBusy() const { return showCoverSearch_ && (coverSearch_.autoPreview || library_.coverSearch().busy); }
    // The Trucos window, for the game main() handed it.
    void openCheats() { showCheats_ = true; }
    // Its lists from internet are still being looked for (or not asked for yet).
    bool cheatsBusy() const {
        return showCheats_ && (cheatDb_.view().busy || cheatDb_.view().game.empty() || cheatsRefit_ > 0);
    }
    void openNetplay() { showNetplay_ = true; }
    // `controls`: on its Controles tab.
    void openGameOptions(bool controls = false) {
        showGameOptions_ = true;
        gameControlsTab_ = controls;
    }
    bool browserOpen() const { return browser_.isOpen(); }
    Library& library() { return library_; }
    // The instrument editor's window, drawn with the rest (not owned).
    void setEditor(InstrumentEditor* editor) { editor_ = editor; }
    // RetroAchievements' menu, windows and notices (not owned).
    void setAchievements(Achievements* achievements);
    void openAchievements() {
        if (achievements_) achievements_->openAccount();
    }
    void showAchievement(const Achievements::Notice& notice) {
        if (achievements_) achievements_->show(notice);
    }
    // Where the SoundFonts to choose from are: the one that comes with plumsg, and the
    // user's folder (created if missing), listed in Sonido > SoundFont.
    void setSoundfonts(std::string included, std::string folder);
    // The states menu: a list of the slots that the pad (or the keyboard) can drive.
    void toggleStateMenu();
    bool stateMenuOpen() const { return stateMenu_.open; }
    void notify(std::string message);
    // A message the user has to acknowledge, drawn inside the window so that it is seen
    // in full screen too.
    void showError(std::string message) { error_ = std::move(message); }

private:
    struct Capture {
        bool active = false;
        int row = 0;          // 0..kButtons-1 console buttons, then hotkeys
        int column = 0;       // 0 keyboard player 1, 1 keyboard player 2, 2 gamepad
        bool append = false;  // add to the existing bindings instead of replacing them
        bool game = false;    // for the Opciones de este juego window's buttons, not the general ones
    };

    void drawMenuBar(const UiState& state, UiActions& out);
    void drawSoundMenu(const UiState& state, UiActions& out);
    void chooseSoundfont(UiActions& out);
    void chooseTrack();
    void drawControls(UiActions& out);
    // The table of bindings of `c`: the console buttons, and the hotkeys too when not `game`.
    // Returns whether a binding was cleared (a new one arrives later, in finishCapture).
    bool drawBindings(InputConfig& c, bool game);
    void drawVideo(const UiState& state, UiActions& out);
    void drawAbout();
    void drawError();
    void drawCheats(const UiState& state, UiActions& out);
    void drawCheatDatabase(const UiState& state, UiActions& out);
    void drawGameOptions(const UiState& state, UiActions& out);
    void drawGameSettings(const UiState& state, UiActions& out);
    void drawGameControls(const GameOptionsView& v, UiActions& out);
    void drawLibrary(UiActions& out);
    void drawLibraryFolders(UiActions& out);
    void drawCoverSearch();
    void startCoverSearch();
    void drawBios(UiActions& out);
    void drawNetplay(const UiState& state, UiActions& out);
    void rescanLibrary();
    bool handleLibrary(const SDL_Event& e);
    std::vector<size_t> visibleGames();
    void drawStateMenu(const UiState& state, UiActions& out);
    void drawStateSlots(const UiState& state);
    // The options of the states menu's tab `tab` (1 on): the ones that change while playing.
    void drawStateOptions(int tab, const UiState& state, UiActions& out);
    bool handleStateMenu(const SDL_Event& e);
    void drawOverlays(const UiState& state);
    // Hides the mouse pointer while a game runs and the mouse has been still a while.
    void hideIdleCursor(const UiState& state);
    void finishCapture(int keyOrButton);
    void loadLogo();

    SDL_Window* window_;
    Settings& settings_;
    Input& input_;
    FileBrowser browser_;
    Library library_;
    CheatDatabase cheatDb_;
    InstrumentEditor* editor_ = nullptr;
    std::unique_ptr<AchievementsUi> achievements_;
    std::string includedSoundfont_, soundfontFolder_;
    void drawSoundfontMenu(UiActions& out);
    void drawSoundfontCombo(UiActions& out, bool labelled = true);
    void drawEqualizer(UiActions& out);
    unsigned int logoTexture_ = 0;  // the PLUMSG marquee, 0 if it could not be decoded
    int logoWidth_ = 0, logoHeight_ = 0;
    unsigned int iconTexture_ = 0;  // the plum, 0 if it could not be decoded

    bool showControls_ = false;
    bool showVideo_ = false;
    bool showAbout_ = false;
    bool showCheats_ = false;
    bool showLibrary_ = false;
    bool showBios_ = false;
    bool showNetplay_ = false;
    bool showGameOptions_ = false;
    bool gameControlsTab_ = false;  // bring the Opciones de este juego window's Controles tab forward
    char netAddress_[128] = "";
    std::vector<std::string> localAddresses_;  // this computer's, for the host to share
    bool showFolders_ = false;  // the library's list of folders, unfolded
    // What the file browser (or the system dialog) is choosing a file or folder for.
    enum class BrowseFor { Game, LibraryFolder, CdBios, Soundfont, Track };
    BrowseFor browseFor_ = BrowseFor::Game;
    int biosSlot_ = 0;  // the region whose BIOS is being chosen
    std::string biosStatus_;
    std::map<std::string, BiosInfo> biosChecked_;  // what each chosen file looks like, read once
    bool openRequested_ = false;
    bool pendingSave_ = false;  // a binding changed outside frame()
    bool pendingGameOptions_ = false;  // one of a game's own buttons changed outside frame()
    InputConfig* gameControls_ = nullptr;  // the buttons the Opciones de este juego window shows
    bool peekMenu_ = false;  // menu bar hidden but shown while the mouse is at the top
    int menuHeight_ = 0;
    Capture capture_;
    struct StateMenu {
        static constexpr int kTabs = 5;  // Estados, Imagen, 3D, Sonido, Velocidad
        bool open = false;
        int selected = 0;
        int slots = 0;
        bool scroll = false;  // bring the selected row into view on the next frame
        int load = -1, save = -1;  // chosen this frame, handed over by frame()
        int tab = 0;               // 0 the slots, then the tabs of options (L1 / R1)
        bool tabChanged = false;   // changed with the pad or the keyboard: select it in the tab bar
        int option = 0;            // the option chosen on the tabs of options
        int options = 0;           // how many the tab had, as last drawn
        std::vector<int> changes;  // to the chosen option: -1 left, +1 right, 0 A; applied when drawn
        int held = 0;              // the pad's left (-1) or right (+1) held down, to repeat it
        Uint32 heldSince = 0, repeatAt = 0;
    } stateMenu_;
    struct LibraryView {
        int selected = 0;      // in the filtered list
        int columns = 1;       // of the grid, as last drawn
        bool scroll = false;
        bool focusSearch = false;
        int load = -1;         // index into the filtered list chosen this frame
        bool tabChanged = false;  // the console tab was changed with the pad or the keyboard
        // The menu of one game (right click, X on the pad or the menu key): its index in
        // the filtered list, the item chosen with the pad or the keys, and a request to open it.
        static constexpr int kItems = 5;  // Jugar, Trucos, Opciones de este juego, Editor de instrumentos, Buscar carátula
        int menuGame = -1;
        int menuItem = 0;
        bool openMenu = false;
        int menuAction = -1;   // item picked with the pad or the keys this frame
        char search[96] = "";
    } libraryView_;
    // "Buscar carátula..." for one game of the library.
    bool showCoverSearch_ = false;
    struct CoverSearchView {
        size_t game = 0;   // index into the library's games
        std::string path;  // of that game, in case the library is scanned again
        char query[128] = "";
        int selected = -1;  // in the list of names
        bool focus = false;
        bool autoPreview = false;  // show the closest cover once the names arrive
    } coverSearch_;
    char cheatCode_[64] = "";
    char cheatName_[96] = "";
    std::string cheatError_;
    char cheatFilter_[64] = "";  // over the downloaded list
    int cheatsRefit_ = 0;        // frames left to fit the Trucos window to its new height
    float cheatsWidth_ = 560;
    std::string error_;  // showError(), until acknowledged
    std::string toast_;
    Uint32 toastUntil_ = 0;
    Uint32 lastMouseMove_ = 0;  // SDL_GetTicks() of the last mouse motion, click or wheel
};

}  // namespace plumsg
