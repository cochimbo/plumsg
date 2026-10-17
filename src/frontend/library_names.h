#pragma once

#include <string>
#include <vector>

namespace plumsg {

// Names for the game library, worked out from ROM file names in the No-Intro style
// ("Lion King, The (World).md"). No SDL here, so the tests can use it.

// The name to show: tags in brackets dropped and a trailing ", The" moved to the front
// ("The Lion King").
std::string gameTitle(const std::string& fileStem);
// The tags in brackets, e.g. "USA, Europe · Rev A"; empty if there are none.
std::string gameTags(const std::string& fileStem);
// For searching: lower case letters and digits only, words separated by one space.
std::string searchKey(const std::string& text);

// File names (without ".png") to try in the libretro-thumbnails boxart sets, best
// first: the whole name, then with fewer trailing tags, then the bare title with the
// usual regions. Characters the set cannot use in file names (&*/:`<>?\|") become '_'.
std::vector<std::string> coverNames(const std::string& fileStem);
// Percent-encodes everything but letters, digits and -._~ (spaces become %20).
std::string urlEncode(const std::string& text);

}  // namespace plumsg
