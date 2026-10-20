#include "settings.h"

#include "../core/system.h"
#include "../core/video_frame_md.h"
#include "../render/profile3d.h"

#include <algorithm>
#include <iterator>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

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

bool yes(const std::string& v) {
    const std::string l = lower(v);
    return l == "1" || l == "si" || l == "sí" || l == "true";
}

constexpr const char* kScaleNames[] = {"entero", "ajustar", "estirar"};
constexpr const char* kFilterNames[] = {"ninguno", "scanlines", "crt"};
constexpr const char* kRegionNames[] = {"auto", "japon", "usa", "europa"};
constexpr const char* kSystemKeys[] = {"megadrive", "32x", "megacd", "mastersystem", "gamegear"};
constexpr const char* kLibrarySortKeys[] = {"titulo", "consola", "año", "genero", "desarrolladora"};
static_assert(std::size(kLibrarySortKeys) == Settings::kLibrarySorts, "one key per order");
static_assert(std::size(kSystemKeys) == kSystemCount, "one key per console");
constexpr const char* kBiosKeys[] = {"megacd_japon", "megacd_usa", "megacd_europa"};

}  // namespace

void Settings::addRecent(const std::string& path) {
    removeRecent(path);
    recent.insert(recent.begin(), path);
    if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) lastFolder = path.substr(0, slash);
}

void Settings::removeRecent(const std::string& path) {
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
}

Settings Settings::load(const std::string& path) {
    Settings s;
    std::ifstream in(path);
    parse(in, s);
    return s;
}

void Settings::parse(std::istream& in, Settings& s) {
    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));

        if (section == "interfaz") {
            if (key == "idioma") s.language = lower(value);
        } else if (section == "video") {
            if (key == "vista") s.view = value;
            else if (key == "escalado") {
                for (int i = 0; i < 3; ++i)
                    if (lower(value) == kScaleNames[i]) s.render.scale = static_cast<ScaleMode>(i);
            } else if (key == "suavizado") s.render.smooth = yes(value);
            else if (key == "filtro") {
                for (int i = 0; i < 3; ++i)
                    if (lower(value) == kFilterNames[i]) s.render.filter = static_cast<Filter2D>(i);
            } else if (key == "intensidad_filtro") s.render.filterStrength = std::clamp(std::atoi(value.c_str()), 0, 100) / 100.0f;
            else if (read3DKey(key, value, s.render)) continue;
            else if (key == "pixeles_cuadrados") s.render.squarePixels = yes(value);
            else if (key == "sin_limite_sprites") s.unlimitedSprites = yes(value);
            else if (key == "pantalla_ancha") {  // as the first version wrote it: no, 16:10 or 16:9
                s.wideLeft = s.wideRight = 0;
                for (const Widescreen& w : kWidescreens)
                    if (lower(value) == w.name) s.wideLeft = s.wideRight = w.columns;
            } else if (key == "pantalla_ancha_izquierda") s.wideLeft = std::clamp(std::atoi(value.c_str()), 0, VideoFrameMd::kMaxWideSide);
            else if (key == "pantalla_ancha_derecha") s.wideRight = std::clamp(std::atoi(value.c_str()), 0, VideoFrameMd::kMaxWideSide);
            else if (key == "tamano_ventana") s.windowScale = std::clamp(std::atoi(value.c_str()), 1, 8);
            else if (key == "pantalla_completa") s.fullscreen = yes(value);
            else if (key == "barra_menus") s.showMenu = yes(value);
        } else if (section == "audio") {
            if (key == "volumen") s.volume = std::clamp(std::atoi(value.c_str()), 0, 100);
            else if (key == "silencio") s.mute = yes(value);
            else if (key == "modo") s.roland = lower(value) == "sintetizador" || lower(value) == "roland";
            else if (key == "soundfont") s.soundfont = value;
            else if (key == "mezcla_chip") s.chipMix = std::clamp(std::atoi(value.c_str()), 0, 100);
            else if (key == "reverb") s.reverb = std::clamp(std::atoi(value.c_str()), 0, 100);
            else if (key == "chorus") s.chorus = std::clamp(std::atoi(value.c_str()), 0, 100);
            else if (key == "ecualizador") {
                std::array<int, 5> bands{};
                if (std::sscanf(value.c_str(), "%d , %d , %d , %d , %d", &bands[0], &bands[1], &bands[2], &bands[3],
                                &bands[4]) == 5)
                    for (size_t i = 0; i < bands.size(); ++i) s.equalizer[i] = std::clamp(bands[i], -12, 12);
            }
            else if (key == "efectos") s.effectsOriginal = lower(value) == "original";
        } else if (section == "consola") {
            if (key == "region") {
                for (int i = 0; i < 4; ++i)
                    if (lower(value) == kRegionNames[i]) s.region = static_cast<Region>(i);
            } else if (key == "mando1" || key == "mando2") {
                s.padType[key == "mando1" ? 0 : 1] = value == "3" ? PadType::ThreeButton : PadType::SixButton;
            } else if (key == "fm_master_system") {
                s.smsFm = yes(value);
            } else if (key == "overclock") {
                s.overclock = std::clamp(std::atoi(value.c_str()), 100, kMaxOverclock);
            }
        } else if (section == "velocidad") {
            if (key == "rebobinado_segundos") s.rewindSeconds = std::clamp(std::atoi(value.c_str()), 0, kMaxRewindSeconds);
            else if (key == "avance_rapido") s.fastForward = std::clamp(std::atoi(value.c_str()), 2, 10);
            else if (key == "camara_lenta") s.slowMotion = std::clamp(std::atoi(value.c_str()), 10, 90);
            else if (key == "run_ahead") s.runAhead = std::clamp(std::atoi(value.c_str()), 0, kMaxRunAhead);
        } else if (section == "estados") {
            if (key == "ranura") s.stateSlot = std::clamp(std::atoi(value.c_str()), 0, 9);
        } else if (section == "biblioteca") {
            if (key == "carpeta") {
                if (!value.empty() && std::find(s.libraryFolders.begin(), s.libraryFolders.end(), value) == s.libraryFolders.end())
                    s.libraryFolders.push_back(value);
            } else if (key == "abrir_al_empezar") s.libraryAtStart = yes(value);
            else if (key == "vista") s.libraryList = lower(value) == "lista";
            else if (key == "detalles") s.libraryDetails = yes(value);
            else if (key == "orden") {
                s.librarySort = 0;
                for (int i = 0; i < Settings::kLibrarySorts; ++i)
                    if (lower(value) == kLibrarySortKeys[i]) s.librarySort = i;
            } else if (key == "sistema") {
                s.librarySystem = -1;
                for (int i = 0; i < kSystemCount; ++i)
                    if (lower(value) == kSystemKeys[i]) s.librarySystem = i;
            }
        } else if (section == "bios") {
            for (int i = 0; i < 3; ++i)
                if (key == kBiosKeys[i]) s.cdBios[static_cast<size_t>(i)] = value;
        } else if (section == "red") {
            if (key == "puerto") s.netPort = std::clamp(std::atoi(value.c_str()), 1, 65535);
            else if (key == "direccion") s.netAddress = value;
            else if (key == "retardo") s.netInputDelay = std::clamp(std::atoi(value.c_str()), 0, 6);
        } else if (section == "logros") {
            if (key == "activado") s.achievements = yes(value);
            else if (key == "usuario") s.raUser = value;
            else if (key == "token") s.raToken = value;
            else if (key == "hardcore") s.raHardcore = yes(value);
        } else if (section == "recientes") {
            if (key == "rom" && !value.empty() && s.recent.size() < kMaxRecent) s.recent.push_back(value);
            else if (key == "ultima_carpeta") s.lastFolder = value;
        }
    }
}

bool Settings::save(const std::string& path, const InputConfig& input) const {
    std::ofstream out(path);
    if (!out) return false;
    input.write(out);
    write(out);
    return static_cast<bool>(out);
}

void Settings::write(std::ostream& out) const {
    auto flag = [](bool b) { return b ? "si" : "no"; };
    out << "\n[interfaz]\n"
        << "# idioma: es (español), en (English), fr (français) o pt (português); vacío: el del sistema.\n"
        << "idioma = " << language << "\n"
        << "\n[video]\n"
        << "# vista: 2D o capas (depuración, no son los colores del juego). escalado: entero, ajustar o estirar.\n"
        << "vista = " << view << "\n"
        << "escalado = " << kScaleNames[static_cast<int>(render.scale)] << "\n"
        << "suavizado = " << flag(render.smooth) << "\n"
        << "# filtro de la vista 2D: ninguno, scanlines o crt. intensidad_filtro: de 0 a 100.\n"
        << "filtro = " << kFilterNames[static_cast<int>(render.filter)] << "\n"
        << "intensidad_filtro = " << static_cast<int>(render.filterStrength * 100.0f + 0.5f) << "\n";
    write3D(out, render);
    out << "# pixeles_cuadrados: no estirar la imagen a 4:3 como la tele.\n"
        << "pixeles_cuadrados = " << flag(render.squarePixels) << "\n"
        << "# sin_limite_sprites: dibujar todos los sprites de cada línea, sin el límite de la consola\n"
        << "# que los hace parpadear o desaparecer. El juego no lo nota.\n"
        << "sin_limite_sprites = " << flag(unlimitedSprites) << "\n"
        << "# pantalla_ancha_izquierda/derecha: columnas de más a cada lado (0 a 56; 53 y 53 es 16:9).\n"
        << "# Enseñan lo que hay más allá de los bordes; en algunos juegos se ven cosas que no tendrían que verse.\n"
        << "pantalla_ancha_izquierda = " << wideLeft << "\n"
        << "pantalla_ancha_derecha = " << wideRight << "\n"
        << "tamano_ventana = " << windowScale << "\n"
        << "pantalla_completa = " << flag(fullscreen) << "\n"
        << "barra_menus = " << flag(showMenu) << "\n"
        << "\n[audio]\n"
        << "# volumen: de 0 a 100.\n"
        << "volumen = " << volume << "\n"
        << "silencio = " << flag(mute) << "\n"
        << "# modo: original (el YM2612 y el PSG) o sintetizador (su música con un SoundFont, como una SC-55).\n"
        << "# soundfont: un .sf2 propio (vacío: el que viene con plumsg). mezcla_chip: cuánto se oye\n"
        << "# el sonido original además del sintetizador, de 0 a 100. reverb y chorus: de 0 a 100.\n"
        << "modo = " << (roland ? "sintetizador" : "original") << "\n"
        << "soundfont = " << soundfont << "\n"
        << "mezcla_chip = " << chipMix << "\n"
        << "reverb = " << reverb << "\n"
        << "chorus = " << chorus << "\n"
        << "# ecualizador: los dB (de -12 a 12) de graves, medios graves, medios, medios agudos y agudos.\n"
        << "ecualizador = " << equalizer[0] << ", " << equalizer[1] << ", " << equalizer[2] << ", " << equalizer[3]
        << ", " << equalizer[4] << "\n"
        << "# efectos: original (los efectos de sonido suenan como en la consola) o sintetizador. Hace falta\n"
        << "# haber usado Sacar canciones en el juego para saber cuáles son sus efectos.\n"
        << "efectos = " << (effectsOriginal ? "original" : "sintetizador") << "\n"
        << "\n[consola]\n"
        << "# region: auto (la de la ROM), japon, usa o europa. mando1/mando2: 3 o 6 botones.\n"
        << "region = " << kRegionNames[static_cast<int>(region)] << "\n"
        << "mando1 = " << (padType[0] == PadType::ThreeButton ? 3 : 6) << "\n"
        << "mando2 = " << (padType[1] == PadType::ThreeButton ? 3 : 6) << "\n"
        << "# fm_master_system: si, la unidad FM de la Master System también fuera de Japón.\n"
        << "fm_master_system = " << (smsFm ? "si" : "no") << "\n"
        << "# overclock: velocidad del procesador (68000) en porcentaje, de 100 (la de la consola) a 400.\n"
        << "# Quita las ralentizaciones de algunos juegos; alguno puede ir demasiado rápido o fallar.\n"
        << "overclock = " << overclock << "\n"
        << "\n[velocidad]\n"
        << "# rebobinado_segundos: cuántos segundos se pueden rebobinar (0 lo desactiva, hasta 60).\n"
        << "# avance_rapido: veces más rápido, de 2 a 10. camara_lenta: porcentaje de la velocidad, de 10 a 90.\n"
        << "# run_ahead: fotogramas de retardo de los mandos que se quitan, de 0 (desactivado) a 4.\n"
        << "rebobinado_segundos = " << rewindSeconds << "\n"
        << "avance_rapido = " << fastForward << "\n"
        << "camara_lenta = " << slowMotion << "\n"
        << "run_ahead = " << runAhead << "\n"
        << "\n[estados]\n"
        << "# ranura: la ranura de estado activa, de 0 a 9.\n"
        << "ranura = " << stateSlot << "\n"
        << "\n[biblioteca]\n"
        << "# carpeta: donde están tus juegos (una línea por carpeta). abrir_al_empezar: mostrar la biblioteca\n"
        << "# al abrir plumsg sin juego. sistema: pestaña elegida (todos, megadrive, 32x, megacd, mastersystem o gamegear).\n"
        << "# vista: cuadricula o lista. detalles: el panel de la derecha con los datos del juego elegido.\n"
        << "# orden: titulo, consola, año, genero o desarrolladora.\n";
    for (const auto& f : libraryFolders) out << "carpeta = " << f << "\n";
    out << "abrir_al_empezar = " << flag(libraryAtStart) << "\n"
        << "sistema = " << (librarySystem >= 0 && librarySystem < kSystemCount ? kSystemKeys[librarySystem] : "todos") << "\n"
        << "vista = " << (libraryList ? "lista" : "cuadricula") << "\n"
        << "detalles = " << flag(libraryDetails) << "\n"
        << "orden = " << kLibrarySortKeys[std::clamp(librarySort, 0, kLibrarySorts - 1)] << "\n"
        << "\n[bios]\n"
        << "# Las BIOS del Mega CD de cada región (no vienen con plumsg).\n";
    for (int i = 0; i < 3; ++i) out << kBiosKeys[i] << " = " << cdBios[static_cast<size_t>(i)] << "\n";
    out << "\n[red]\n"
        << "# puerto: el puerto UDP para alojar partidas. direccion: el último anfitrión al que te uniste.\n"
        << "# retardo: fotogramas entre pulsar un botón y que haga efecto en una partida en red (0 a 6).\n"
        << "puerto = " << netPort << "\n"
        << "direccion = " << netAddress << "\n"
        << "retardo = " << netInputDelay << "\n"
        << "\n[logros]\n"
        << "# RetroAchievements. activado: si o no. usuario y token: la cuenta (el token sustituye a la\n"
        << "# contraseña, que no se guarda). hardcore: sin rebobinar, cámara lenta, trucos ni cargar estados.\n"
        << "activado = " << flag(achievements) << "\n"
        << "usuario = " << raUser << "\n"
        << "token = " << raToken << "\n"
        << "hardcore = " << flag(raHardcore) << "\n"
        << "\n[recientes]\n";
    if (!lastFolder.empty()) out << "ultima_carpeta = " << lastFolder << "\n";
    for (const auto& r : recent) out << "rom = " << r << "\n";
}

}  // namespace plumsg
