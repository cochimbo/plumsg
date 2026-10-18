#include "game_options.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "../render/profile3d.h"

namespace plumsg {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The option a key of the file belongs to: both pads are one option.
std::string optionFor(const std::string& key) { return key == "mando1" || key == "mando2" ? "mandos" : key; }

}  // namespace

const std::vector<GameOption>& gameOptions() {
    static const std::vector<GameOption> options = {
        {"vista", "Vista", "Vídeo", [](const Settings& f, Settings& t) { t.view = f.view; }},
        {"escalado", "Escalado", "Vídeo", [](const Settings& f, Settings& t) { t.render.scale = f.render.scale; }},
        {"suavizado", "Suavizado", "Vídeo", [](const Settings& f, Settings& t) { t.render.smooth = f.render.smooth; }},
        {"filtro", "Filtro 2D", "Vídeo", [](const Settings& f, Settings& t) { t.render.filter = f.render.filter; }},
        {"intensidad_filtro", "Intensidad del filtro", "Vídeo",
         [](const Settings& f, Settings& t) { t.render.filterStrength = f.render.filterStrength; }},
        {"pixeles_cuadrados", "Píxeles cuadrados", "Vídeo",
         [](const Settings& f, Settings& t) { t.render.squarePixels = f.render.squarePixels; }},
        {"perfil_3d", "Todos los ajustes del 3D", "3D", [](const Settings& f, Settings& t) { copy3D(f.render, t.render); }},
        {"volumen", "Volumen", "Sonido", [](const Settings& f, Settings& t) { t.volume = f.volume; }},
        {"silencio", "Silencio", "Sonido", [](const Settings& f, Settings& t) { t.mute = f.mute; }},
        {"modo", "Sonido Roland", "Sonido", [](const Settings& f, Settings& t) { t.roland = f.roland; }},
        {"soundfont", "SoundFont", "Sonido", [](const Settings& f, Settings& t) { t.soundfont = f.soundfont; }},
        {"mezcla_chip", "Sonido original mezclado", "Sonido", [](const Settings& f, Settings& t) { t.chipMix = f.chipMix; }},
        {"reverb", "Reverb", "Sonido", [](const Settings& f, Settings& t) { t.reverb = f.reverb; }},
        {"chorus", "Chorus", "Sonido", [](const Settings& f, Settings& t) { t.chorus = f.chorus; }},
        {"region", "Región", "Consola", [](const Settings& f, Settings& t) { t.region = f.region; }},
        {"mandos", "Tipo de mando", "Consola", [](const Settings& f, Settings& t) { t.padType = f.padType; }},
        {"rebobinado_segundos", "Segundos de rebobinado", "Velocidad",
         [](const Settings& f, Settings& t) { t.rewindSeconds = f.rewindSeconds; }},
        {"avance_rapido", "Avance rápido", "Velocidad", [](const Settings& f, Settings& t) { t.fastForward = f.fastForward; }},
        {"camara_lenta", "Cámara lenta", "Velocidad", [](const Settings& f, Settings& t) { t.slowMotion = f.slowMotion; }},
    };
    return options;
}

const GameOption* findGameOption(const std::string& key) {
    for (const GameOption& o : gameOptions())
        if (key == o.key) return &o;
    return nullptr;
}

void GameOverrides::applyTo(Settings& to) const {
    for (const GameOption& o : gameOptions())
        if (has(o.key)) o.copy(values, to);
}

void GameOverrides::restore(const Settings& general, Settings& to) const {
    for (const GameOption& o : gameOptions())
        if (has(o.key)) o.copy(general, to);
}

GameOverrides parseGameOverrides(const std::string& text, const Settings& general) {
    GameOverrides o;
    o.values = general;
    if (parseProfile(text, o.values.render)) o.own.insert("perfil_3d");
    // Settings::parse reads the values; which options the file has comes from its keys.
    std::istringstream keys(text);
    std::string line, section;
    while (std::getline(keys, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || section == "perfil_3d") continue;
        const std::string key = optionFor(lower(trim(line.substr(0, eq))));
        if (key != "perfil_3d" && findGameOption(key)) o.own.insert(key);
    }
    Settings read = o.values;
    std::istringstream in(text);
    Settings::parse(in, read);
    for (const GameOption& opt : gameOptions())
        if (o.has(opt.key) && std::string(opt.key) != "perfil_3d") opt.copy(read, o.values);
    return o;
}

std::string formatGameOverrides(const GameOverrides& o, const std::string& game) {
    if (o.own.empty()) return {};
    std::ostringstream out;
    out << "# Opciones propias de " << game << " para plumsg. Mientras este juego está cargado,\n"
        << "# estos ajustes sustituyen a los de plumsg.ini.\n";
    if (o.has("perfil_3d")) {
        out << "\n[perfil_3d]\n";
        write3D(out, o.values.render);
    }
    // The rest as plumsg.ini writes them, keeping only this game's keys and their sections.
    std::ostringstream all;
    o.values.write(all);
    std::istringstream in(all.str());
    std::string line, header;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[') {
            header = t;
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = optionFor(lower(trim(t.substr(0, eq))));
        if (key == "perfil_3d" || !o.has(key) || !findGameOption(key)) continue;
        if (!header.empty()) {
            out << "\n" << header << "\n";
            header.clear();
        }
        out << t << "\n";
    }
    return out.str();
}

}  // namespace plumsg
