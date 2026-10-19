#include "game_options.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "../core/i18n.h"
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
        {"vista", N_("Vista"), N_("Vídeo"), [](const Settings& f, Settings& t) { t.view = f.view; }},
        {"escalado", N_("Escalado"), N_("Vídeo"), [](const Settings& f, Settings& t) { t.render.scale = f.render.scale; }},
        {"suavizado", N_("Suavizado"), N_("Vídeo"), [](const Settings& f, Settings& t) { t.render.smooth = f.render.smooth; }},
        {"filtro", N_("Filtro 2D"), N_("Vídeo"), [](const Settings& f, Settings& t) { t.render.filter = f.render.filter; }},
        {"intensidad_filtro", N_("Intensidad del filtro"), N_("Vídeo"),
         [](const Settings& f, Settings& t) { t.render.filterStrength = f.render.filterStrength; }},
        {"pixeles_cuadrados", N_("Píxeles cuadrados"), N_("Vídeo"),
         [](const Settings& f, Settings& t) { t.render.squarePixels = f.render.squarePixels; }},
        {"sin_limite_sprites", N_("Sin límite de sprites"), N_("Vídeo"),
         [](const Settings& f, Settings& t) { t.unlimitedSprites = f.unlimitedSprites; }},
        {"pantalla_ancha", N_("Pantalla ancha"), N_("Vídeo"), [](const Settings& f, Settings& t) { t.widescreen = f.widescreen; }},
        {"perfil_3d", N_("Todos los ajustes del 3D"), "3D", [](const Settings& f, Settings& t) { copy3D(f.render, t.render); }},
        {"volumen", N_("Volumen"), N_("Sonido"), [](const Settings& f, Settings& t) { t.volume = f.volume; }},
        {"silencio", N_("Silencio"), N_("Sonido"), [](const Settings& f, Settings& t) { t.mute = f.mute; }},
        {"modo", N_("Sintetizador"), N_("Sonido"), [](const Settings& f, Settings& t) { t.roland = f.roland; }},
        {"soundfont", N_("SoundFont"), N_("Sonido"), [](const Settings& f, Settings& t) { t.soundfont = f.soundfont; }},
        {"mezcla_chip", N_("Sonido original mezclado"), N_("Sonido"), [](const Settings& f, Settings& t) { t.chipMix = f.chipMix; }},
        {"reverb", N_("Reverb"), N_("Sonido"), [](const Settings& f, Settings& t) { t.reverb = f.reverb; }},
        {"chorus", N_("Chorus"), N_("Sonido"), [](const Settings& f, Settings& t) { t.chorus = f.chorus; }},
        {"efectos", N_("Efectos de sonido originales"), N_("Sonido"),
         [](const Settings& f, Settings& t) { t.effectsOriginal = f.effectsOriginal; }},
        {"region", N_("Región"), N_("Consola"), [](const Settings& f, Settings& t) { t.region = f.region; }},
        {"mandos", N_("Tipo de mando"), N_("Consola"), [](const Settings& f, Settings& t) { t.padType = f.padType; }},
        {"overclock", N_("Overclock"), N_("Consola"), [](const Settings& f, Settings& t) { t.overclock = f.overclock; }},
        {"rebobinado_segundos", N_("Segundos de rebobinado"), N_("Velocidad"),
         [](const Settings& f, Settings& t) { t.rewindSeconds = f.rewindSeconds; }},
        {"avance_rapido", N_("Avance rápido"), N_("Velocidad"), [](const Settings& f, Settings& t) { t.fastForward = f.fastForward; }},
        {"camara_lenta", N_("Cámara lenta"), N_("Velocidad"), [](const Settings& f, Settings& t) { t.slowMotion = f.slowMotion; }},
        {"run_ahead", N_("Run-ahead (menos latencia)"), N_("Velocidad"), [](const Settings& f, Settings& t) { t.runAhead = f.runAhead; }},
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

GameOverrides parseGameOverrides(const std::string& text, const Settings& general, const InputConfig& generalInput) {
    GameOverrides o;
    o.values = general;
    o.controls = generalInput;
    std::istringstream controls(text);
    if (InputConfig::parse(controls, o.controls)) o.own.insert(kGameControls);
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
        // The controls' sections share key names (avance_rapido...) with the options.
        if (eq == std::string::npos || section == "perfil_3d" || section == "teclado" || section == "teclado2" ||
            section == "mando")
            continue;
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
    if (o.has(kGameControls)) {
        out << "\n";
        o.controls.writeButtons(out);
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
