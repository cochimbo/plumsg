#include "library_names.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace plumsg {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// Where the first " (" or " [" tag starts, or the end of the name.
size_t tagsStart(const std::string& stem) {
    size_t i = 0;
    while ((i = stem.find_first_of("([", i)) != std::string::npos) {
        if (i > 0) return i;
        ++i;
    }
    return stem.size();
}

// The tags one by one, without their brackets.
std::vector<std::string> tags(const std::string& stem) {
    std::vector<std::string> out;
    size_t i = tagsStart(stem);
    while (i < stem.size()) {
        const char open = stem[i];
        if (open != '(' && open != '[') {
            ++i;
            continue;
        }
        const size_t close = stem.find(open == '(' ? ')' : ']', i);
        if (close == std::string::npos) break;
        out.push_back(stem.substr(i + 1, close - i - 1));
        i = close + 1;
    }
    return out;
}

std::string bareName(const std::string& stem) { return trim(stem.substr(0, tagsStart(stem))); }

std::string libretroSafe(std::string name) {
    for (char& c : name)
        if (std::strchr("&*/:`<>?\\|\"", c)) c = '_';
    return name;
}

}  // namespace

std::string gameTitle(const std::string& fileStem) {
    std::string title = bareName(fileStem);
    // "Lion King, The" and "Adventures of Batman & Robin, The - Subtitle".
    for (const char* article : {", The", ", A", ", An"}) {
        const size_t at = title.find(article);
        if (at == std::string::npos) continue;
        const size_t end = at + std::strlen(article);
        if (end != title.size() && title.compare(end, 3, " - ") != 0) continue;
        title = std::string(article + 2) + " " + title.substr(0, at) + title.substr(end);
        break;
    }
    return title.empty() ? fileStem : title;
}

std::string gameTags(const std::string& fileStem) {
    std::string out;
    for (const auto& t : tags(fileStem)) out += (out.empty() ? "" : " · ") + t;
    return out;
}

std::string searchKey(const std::string& text) {
    std::string out;
    bool space = false;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 0x80) {
            if (space && !out.empty()) out += ' ';
            out += static_cast<char>(std::tolower(c));
            space = false;
        } else {
            space = true;
        }
    }
    return out;
}

std::vector<std::string> coverNames(const std::string& fileStem) {
    std::vector<std::string> out;
    auto add = [&](const std::string& n) {
        const std::string safe = libretroSafe(trim(n));
        if (!safe.empty() && std::find(out.begin(), out.end(), safe) == out.end()) out.push_back(safe);
    };
    const std::string bare = bareName(fileStem);
    const std::vector<std::string> t = tags(fileStem);
    // Whole name, then dropping tags from the end: "(USA) (Sega Smash Pack)" -> "(USA)".
    for (size_t keep = t.size() + 1; keep-- > 0;) {
        std::string n = bare;
        for (size_t i = 0; i < keep; ++i) n += " (" + t[i] + ")";
        if (keep > 0) add(n);
    }
    for (const char* region : {"USA", "USA, Europe", "World", "Europe", "Japan, USA", "Japan", "USA, Europe, Korea"})
        add(bare + " (" + region + ")");
    return out;
}

std::string urlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

}  // namespace plumsg
