#pragma once

#include <array>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "../core/cartridge.h"
#include "../core/io.h"
#include "../render/renderer.h"
#include "input.h"

namespace plumsg {

// Everything the menus change apart from the controls. Lives in plumsg.ini next to
// the [teclado] / [mando] sections, under [interfaz], [video], [audio], [consola],
// [velocidad], [estados], [biblioteca], [bios], [red], [logros] and [recientes].
struct Settings {
    static constexpr size_t kMaxRecent = 10;
    static constexpr int kMaxRewindSeconds = 60;

    std::string language;  // "es", "en", "fr" or "pt"; empty: the system's, decided at start
    std::string view = "2D";  // Renderer::name() of the active view
    RenderOptions render;
    // Draw every sprite on a line, without the console's limit that makes them flicker.
    bool unlimitedSprites = false;
    int windowScale = 3;
    bool fullscreen = false;
    bool showMenu = true;
    int volume = 80;  // 0-100
    bool mute = false;
    // The Roland sound: the FM and PSG music played as General MIDI on a SoundFont.
    bool roland = false;
    std::string soundfont;  // empty: the one that comes with plumsg
    int chipMix = 0;        // how much of the chips' own music stays, 0-100
    int reverb = 40;        // 0-100
    int chorus = 20;        // 0-100
    bool effectsOriginal = true;  // the game's sound effects keep the chips' sound
    Region region = Region::Auto;
    std::array<PadType, 2> padType{PadType::SixButton, PadType::SixButton};
    int stateSlot = 0;  // 0-9
    int rewindSeconds = 20;  // how far back rewinding reaches, 0 = off
    int fastForward = 4;     // fast-forward speed, times normal (2-10)
    int slowMotion = 50;     // slow-motion speed, percent of normal (10-90)
    // Run-ahead: frames of input lag taken away by showing the picture that many frames
    // ahead (0 off, up to kMaxRunAhead).
    static constexpr int kMaxRunAhead = 4;
    int runAhead = 0;
    std::vector<std::string> recent;  // most recent first
    std::string lastFolder;
    std::vector<std::string> libraryFolders;  // where the game library looks for games
    bool libraryAtStart = true;   // open the library when plumsg starts without a ROM
    int librarySystem = -1;       // the library's tab: -1 every console, else a ConsoleSystem
    // The Mega CD's BIOS for each region (Japan, USA, Europe); plumsg does not include them.
    std::array<std::string, 3> cdBios;
    // Netplay: the UDP port to host on (and to join when the address has none), the
    // last host joined and the input delay in frames.
    static constexpr int kDefaultNetPort = 7845;
    int netPort = kDefaultNetPort;
    std::string netAddress;
    int netInputDelay = 2;
    // RetroAchievements: on or off, the account (its token, never the password) and
    // hardcore mode.
    bool achievements = false;
    std::string raUser;
    std::string raToken;
    bool raHardcore = false;

    void addRecent(const std::string& path);
    void removeRecent(const std::string& path);

    // Reads the sections it knows from `path`, ignoring the rest.
    static Settings load(const std::string& path);
    // Applies the sections it knows from `in` onto `s`, leaving the rest of `s` as it is.
    static void parse(std::istream& in, Settings& s);
    // Rewrites the whole file: the controls from `input` plus these settings.
    bool save(const std::string& path, const InputConfig& input) const;
    // Writes every section of these settings (not the controls), with comments.
    void write(std::ostream& out) const;
};

}  // namespace plumsg
