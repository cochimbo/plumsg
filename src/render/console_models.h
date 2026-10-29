#pragma once

#include <vector>

#include "../core/system.h"

namespace plumsg {

// The consoles the library's 3D view puts the cartridges into, built from their real
// measurements with 13.5 cm (the width of a Mega Drive case) as 1. Each model stands on
// the floor with the middle of its footprint at the origin and its front towards +z.
enum class ConsoleModel {
    MegaDrive,     // the first model: 278 x 215 x 57 mm, with its round top and the slot in it
    Mega32X,       // the 32X alone, to stand in a Mega Drive's slot: 210 x 100 x 110 mm
    MegaCD,        // the first Mega-CD alone, to sit under a Mega Drive, without its tray
    MegaCDTray,    // its disc tray, with the origin at the middle of its top
    MasterSystem,  // the first model: 365 x 170 x 70 mm, black with red lines
    GameGear,      // 210 x 113 x 38 mm, standing up facing the viewer
    Count
};

// Where things are on the models, for placing the cartridges and the moving parts.
struct ConsoleLayout {
    // Mega Drive: the middle of the slot's mouth.
    static constexpr float kMdSlotX = 0.12f, kMdSlotY = 0.404f, kMdSlotZ = -0.08f;
    // 32X, from where it stands in the Mega Drive's slot: the top of its own slot.
    static constexpr float k32xSlotY = 0.75f;
    // Mega-CD: its height (the Mega Drive sits on it, moved by kCdMdX / kCdMdZ), and the
    // tray's top, closed, and how far it comes out.
    static constexpr float kCdHeight = 0.5f, kCdMdX = -0.06f, kCdMdZ = -0.06f;
    static constexpr float kTrayX = -0.12f, kTrayY = 0.33f, kTrayZ = 0.27f, kTrayTravel = 1.1f;
    // Master System: the slot on its raised back.
    static constexpr float kSmsSlotX = 0.55f, kSmsSlotY = 0.521f, kSmsSlotZ = -0.34f;
    // Game Gear: the slot along its top, at the back; and the screen, on its front.
    static constexpr float kGgSlotY = 0.842f, kGgSlotZ = -0.05f;
    static constexpr float kGgScreenY = 0.5f, kGgScreenZ = 0.146f, kGgScreenW = 0.48f, kGgScreenH = 0.36f;
    // Power lights (x, y, z of their middle; the Game Gear's is on its front).
    static constexpr float kMdLed[3] = {-0.55f, 0.301f, 0.56f};
    static constexpr float kCdLed[3] = {0.62f, 0.33f, 0.876f};
    static constexpr float kSmsLed[3] = {1.12f, 0.362f, 0.2f};
    static constexpr float kGgLed[3] = {-0.36f, 0.72f, 0.143f};
};

// Appends the model's triangles: per vertex its position, normal, texture coordinates
// (unused, 0), face (0) and colour, kConsoleVertexFloats floats.
constexpr int kConsoleVertexFloats = 12;
void buildConsoleModel(ConsoleModel model, std::vector<float>& out);

// A console's cartridge, from measured ones (ConsoleMods' table of cartridge sizes):
// its size, where its label is on the front (from its middle), how deep it goes into
// the slot and the slot's width. The Mega CD has a disc instead (12 cm).
struct CartridgeShape {
    float w, h, d;
    float labelW, labelH, labelY;
    float insert;
    float slotW;
};
CartridgeShape cartridgeShape(ConsoleSystem s);
// Appends the cartridge's shell, board and contacts (not its label), centred on the
// origin with the label's side towards +z, in the same layout as buildConsoleModel.
void buildCartridgeModel(ConsoleSystem s, std::vector<float>& out);

}  // namespace plumsg
