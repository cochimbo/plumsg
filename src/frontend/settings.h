#pragma once

#include <array>
#include <string>
#include <vector>

#include "../core/cartridge.h"
#include "../core/io.h"
#include "../render/renderer.h"
#include "input.h"

namespace plumsg {

// Everything the menus change apart from the controls. Lives in plumsg.ini next to
// the [teclado] / [mando] sections, under [video], [audio], [consola], [velocidad],
// [estados], [biblioteca], [bios] and [recientes].
struct Settings {
    static constexpr size_t kMaxRecent = 10;
    static constexpr int kMaxRewindSeconds = 60;

    std::string view = "2D";  // Renderer::name() of the active view
    RenderOptions render;
    int windowScale = 3;
    bool fullscreen = false;
    bool showMenu = true;
    int volume = 80;  // 0-100
    bool mute = false;
    Region region = Region::Auto;
    std::array<PadType, 2> padType{PadType::SixButton, PadType::SixButton};
    int stateSlot = 0;  // 0-9
    int rewindSeconds = 20;  // how far back rewinding reaches, 0 = off
    int fastForward = 4;     // fast-forward speed, times normal (2-10)
    int slowMotion = 50;     // slow-motion speed, percent of normal (10-90)
    std::vector<std::string> recent;  // most recent first
    std::string lastFolder;
    std::vector<std::string> libraryFolders;  // where the game library looks for games
    bool libraryAtStart = true;   // open the library when plumsg starts without a ROM
    int librarySystem = -1;       // the library's tab: -1 every console, else a ConsoleSystem
    // The Mega CD's BIOS for each region (Japan, USA, Europe); plumsg does not include them.
    std::array<std::string, 3> cdBios;

    void addRecent(const std::string& path);
    void removeRecent(const std::string& path);

    // Reads the sections it knows from `path`, ignoring the rest.
    static Settings load(const std::string& path);
    // Rewrites the whole file: the controls from `input` plus these settings.
    bool save(const std::string& path, const InputConfig& input) const;
};

}  // namespace plumsg
