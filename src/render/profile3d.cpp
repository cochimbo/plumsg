#include "profile3d.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

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

float percent(const std::string& v) { return std::clamp(std::atoi(v.c_str()), 0, 100) / 100.0f; }

float number(const std::string& v, float lo, float hi) {
    return std::clamp(static_cast<float>(std::atof(v.c_str())), lo, hi);
}

}  // namespace

void copy3D(const RenderOptions& from, RenderOptions& to) {
    to.pseudo3d = from.pseudo3d;
    to.style = from.style;
    to.depth = from.depth;
    to.tilt = from.tilt;
    to.yaw = from.yaw;
    to.zoom = from.zoom;
    to.extrusion = from.extrusion;
    to.thickness = from.thickness;
    to.shadows = from.shadows;
    to.tiltCamera = from.tiltCamera;
    to.lighting = from.lighting;
    to.liveCamera = from.liveCamera;
    to.highBInFront = from.highBInFront;
    to.flatB = from.flatB;
}

bool same3D(const RenderOptions& a, const RenderOptions& b) {
    return a.pseudo3d == b.pseudo3d && a.style == b.style && a.depth == b.depth && a.tilt == b.tilt &&
           a.yaw == b.yaw && a.zoom == b.zoom && a.extrusion == b.extrusion && a.thickness == b.thickness &&
           a.shadows == b.shadows &&
           a.tiltCamera == b.tiltCamera && a.lighting == b.lighting && a.liveCamera == b.liveCamera && a.highBInFront == b.highBInFront && a.flatB == b.flatB;
}

bool read3DKey(const std::string& key, const std::string& value, RenderOptions& r) {
    if (key == "efecto_3d") r.pseudo3d = yes(value);
    else if (key == "estilo_3d") r.style = lower(value) == "capas" ? Style3D::Layers : Style3D::Voxels;
    else if (key == "profundidad_3d") r.depth = percent(value);
    else if (key == "inclinacion_3d") r.tilt = number(value, kMinTilt, kMaxTilt);
    else if (key == "giro_3d") r.yaw = number(value, -kMaxYaw, kMaxYaw);
    else if (key == "zoom_3d") r.zoom = number(value, kMinZoom, kMaxZoom);
    else if (key == "extrusion_3d") r.extrusion = percent(value);
    else if (key == "grosor_3d") r.thickness = percent(value);
    else if (key == "sombras_3d") r.shadows = yes(value);
    else if (key == "inclinar_camara") r.tiltCamera = yes(value);
    else if (key == "iluminacion_3d") r.lighting = yes(value);
    else if (key == "camara_viva") r.liveCamera = yes(value);
    else if (key == "b_alta_delante") r.highBInFront = yes(value);
    else if (key == "plano_b_plano") r.flatB = yes(value);
    else return false;
    return true;
}

void write3D(std::ostream& out, const RenderOptions& r) {
    auto flag = [](bool b) { return b ? "si" : "no"; };
    out << "efecto_3d = " << flag(r.pseudo3d) << "\n"
        << "# estilo_3d: voxeles o capas. profundidad_3d, grosor_3d y extrusion_3d van de 0 a 100.\n"
        << "# Cámara: inclinacion_3d de -10 a 80 grados, giro_3d de -75 a 75, zoom_3d de 0.5 a 3.\n"
        << "estilo_3d = " << (r.style == Style3D::Layers ? "capas" : "voxeles") << "\n"
        << "profundidad_3d = " << std::lround(r.depth * 100) << "\n"
        << "inclinacion_3d = " << std::lround(r.tilt) << "\n"
        << "giro_3d = " << std::lround(r.yaw) << "\n"
        << "zoom_3d = " << std::lround(r.zoom * 100) / 100.0 << "\n"
        << "extrusion_3d = " << std::lround(r.extrusion * 100) << "\n"
        << "grosor_3d = " << std::lround(r.thickness * 100) << "\n"
        << "sombras_3d = " << flag(r.shadows) << "\n"
        << "# inclinar_camara: no mirar de frente (usa inclinacion_3d y giro_3d). iluminacion_3d: luz lateral\n"
        << "# y sombras suaves de cada plano. camara_viva: la cámara se balancea siguiendo la acción.\n"
        << "inclinar_camara = " << flag(r.tiltCamera) << "\n"
        << "iluminacion_3d = " << flag(r.lighting) << "\n"
        << "camara_viva = " << flag(r.liveCamera) << "\n"
        << "# b_alta_delante: los tiles del plano B con prioridad van delante, con los de A.\n"
        << "b_alta_delante = " << flag(r.highBInFront) << "\n"
        << "# plano_b_plano: el plano B se queda plano, como parte del cielo.\n"
        << "plano_b_plano = " << flag(r.flatB) << "\n";
}

std::string formatProfile(const RenderOptions& r, const std::string& game) {
    std::ostringstream out;
    out << "# Perfil 3D de " << game << " para plumsg. Mientras este juego está cargado,\n"
        << "# estos ajustes sustituyen a los del 3D de [video] en plumsg.ini.\n\n[perfil_3d]\n";
    write3D(out, r);
    return out.str();
}

bool parseProfile(const std::string& text, RenderOptions& r) {
    std::istringstream in(text);
    std::string line, section;
    bool found = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            found |= section == "perfil_3d";
            continue;
        }
        const size_t eq = line.find('=');
        if (section != "perfil_3d" || eq == std::string::npos) continue;
        read3DKey(lower(trim(line.substr(0, eq))), trim(line.substr(eq + 1)), r);
    }
    return found;
}

}  // namespace plumsg
