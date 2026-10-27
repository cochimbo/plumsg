// plumsg's look: the Mega Drive as an object. Black ribbed plastic, the red of the
// "16-BIT" on the console's top, chrome italic lettering and the round A B C buttons of
// the pad, on top of Dear ImGui's widgets.
#pragma once

#include "imgui.h"

namespace theme {

// The typefaces, Chakra Petch (OFL, built into the executable). Null until addFonts.
struct Fonts {
    ImFont* body = nullptr;   // everything
    ImFont* title = nullptr;  // bold italic: window titles, a game's name, the main button
};
const Fonts& fonts();

// Add each font to the atlas at `size` pixels, the characters in `ranges`; the
// system's fonts for other scripts can be merged into it right after.
void addBodyFont(float size, const ImWchar* ranges);
void addTitleFont(float size, const ImWchar* ranges);

// The colours and shapes, once after ImGui::CreateContext.
void apply();

// Repaints the title bars of the windows drawn this frame (ribs, red line, chrome
// title). Between the last End and ImGui::Render.
void decorateWindows();

// The one action a window is for ("Jugar"): a slanted red button, `width` wide
// (-1 for all the room left).
bool primaryButton(const char* label, float width = -1.0f);

// A two-way switch drawn as one plate: `on` lit red with `onLabel`, off dark with
// `offLabel`. Clicking it flips `*on` and returns true.
bool switchButton(const char* id, bool* on, const char* onLabel, const char* offLabel);

// The white grid on black of the Master System's boxes, over `min`-`max` of the current
// window, moving with its scroll. Before the items drawn on it.
void boxGrid(ImVec2 min, ImVec2 max);

// A line of controls help ("A / Intro: jugar   B / Esc: cerrar"), dim, with the pad's
// buttons drawn as buttons. Several lines with '\n'.
void hint(const char* text);

}  // namespace theme
