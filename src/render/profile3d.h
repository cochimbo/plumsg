#pragma once

#include <ostream>
#include <string>

#include "renderer.h"

namespace plumsg {

// The pseudo-3D options: on or off, style, depth, camera, thickness, extrusion and the
// plane B tweaks. They live in [video] of plumsg.ini and, for a game with its own
// profile, in a small .ini of that game that takes their place while it is loaded.

// Copies only the pseudo-3D options from `from` into `to`.
void copy3D(const RenderOptions& from, RenderOptions& to);
bool same3D(const RenderOptions& a, const RenderOptions& b);

// Reads one key=value of the pseudo-3D options (key in lower case); false if the key
// is not one of them. Values out of range are clamped.
bool read3DKey(const std::string& key, const std::string& value, RenderOptions& r);
// Writes them as key = value lines, with comments.
void write3D(std::ostream& out, const RenderOptions& r);

// A game profile file: a [perfil_3d] section with the keys above.
std::string formatProfile(const RenderOptions& r, const std::string& game);
// Applies the profile's keys onto `r`; false if `text` has no [perfil_3d] section.
bool parseProfile(const std::string& text, RenderOptions& r);

}  // namespace plumsg
