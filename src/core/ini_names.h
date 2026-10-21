#pragma once

#include <string>
#include <unordered_map>

namespace plumsg {

// The .ini files (plumsg.ini, the per-game ones in profiles/) use English section,
// key and value names since 0.0.32. Up to 0.0.31 they were Spanish: readers pass every
// name through iniName() so those files keep working, and the next save writes them in
// English. Names already in English, and anything not listed, come back unchanged.
inline const std::string& iniName(const std::string& name) {
    static const std::unordered_map<std::string, std::string> kSpanish = {
        // sections
        {"interfaz", "interface"}, {"consola", "console"}, {"velocidad", "speed"}, {"estados", "states"},
        {"biblioteca", "library"}, {"red", "network"}, {"logros", "achievements"}, {"recientes", "recent"},
        {"teclado", "keyboard"}, {"teclado2", "keyboard2"}, {"mando", "gamepad"}, {"perfil_3d", "profile_3d"},
        // [interface] [video] [audio]
        {"idioma", "language"}, {"vista", "view"}, {"escalado", "scale"}, {"suavizado", "smooth"},
        {"filtro", "filter"}, {"intensidad_filtro", "filter_strength"}, {"reescalado", "upscaling"},
        {"pixeles_cuadrados", "square_pixels"}, {"sin_limite_sprites", "no_sprite_limit"},
        {"pantalla_ancha", "widescreen"}, {"pantalla_ancha_izquierda", "widescreen_left"},
        {"pantalla_ancha_derecha", "widescreen_right"}, {"tamano_ventana", "window_scale"},
        {"pantalla_completa", "fullscreen"}, {"barra_menus", "menu_bar"}, {"volumen", "volume"},
        {"silencio", "mute"}, {"modo", "mode"}, {"mezcla_chip", "chip_mix"}, {"ecualizador", "equalizer"},
        {"efectos", "effects"}, {"comparar_sonido", "compare_sound"},
        // [console] [speed] [states] [library] [bios] [network] [achievements] [recent]
        {"mando1", "pad1"}, {"mando2", "pad2"}, {"mandos", "pads"},
        {"rebobinado_segundos", "rewind_seconds"}, {"avance_rapido", "fast_forward"},
        {"camara_lenta", "slow_motion"}, {"contador_fps", "fps_counter"}, {"contador_fps_esquina", "fps_counter_corner"},
        {"ranura", "slot"}, {"carpeta", "folder"}, {"abrir_al_empezar", "open_at_start"}, {"detalles", "details"},
        {"orden", "sort"}, {"sistema", "system"}, {"megacd_japon", "megacd_japan"}, {"megacd_europa", "megacd_europe"},
        {"puerto", "port"}, {"direccion", "address"}, {"retardo", "input_delay"}, {"activado", "enabled"},
        {"usuario", "user"}, {"ultima_carpeta", "last_folder"},
        // 3D
        {"efecto_3d", "pseudo_3d"}, {"estilo_3d", "style_3d"}, {"profundidad_3d", "depth_3d"},
        {"inclinacion_3d", "tilt_3d"}, {"giro_3d", "yaw_3d"}, {"grosor_3d", "thickness_3d"},
        {"sombras_3d", "shadows_3d"}, {"inclinar_camara", "tilt_camera"}, {"iluminacion_3d", "lighting_3d"},
        {"camara_viva", "live_camera"}, {"b_alta_delante", "high_b_in_front"}, {"plano_b_plano", "flat_b"},
        // controls
        {"arriba", "up"}, {"abajo", "down"}, {"izquierda", "left"}, {"derecha", "right"},
        {"guardar_estado", "save_state"}, {"cargar_estado", "load_state"}, {"ranura_anterior", "previous_slot"},
        {"ranura_siguiente", "next_slot"}, {"menu_estados", "states_menu"}, {"rebobinar", "rewind"},
        {"parar", "stop"}, {"stick_como_cruceta", "stick_as_dpad"}, {"zona_muerta", "deadzone"},
        // values
        {"si", "yes"}, {"sí", "yes"}, {"entero", "integer"}, {"ajustar", "fit"}, {"estirar", "stretch"},
        {"ninguno", "none"}, {"japon", "japan"}, {"europa", "europe"}, {"sintetizador", "synthesizer"},
        {"titulo", "title"}, {"año", "year"}, {"genero", "genre"}, {"desarrolladora", "developer"},
        {"todos", "all"}, {"lista", "list"}, {"cuadricula", "grid"}, {"capas", "layers"}, {"voxeles", "voxels"},
    };
    const auto it = kSpanish.find(name);
    return it == kSpanish.end() ? name : it->second;
}

}  // namespace plumsg
