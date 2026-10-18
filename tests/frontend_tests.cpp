// Frontend tests that need SDL's key names but no window: a game's own options and
// controls, read from and written to its file in the perfiles folder.
#define SDL_MAIN_HANDLED
#include <cstdio>
#include <cstdlib>
#include <string>

#include "frontend/game_options.h"

using namespace plumsg;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

constexpr int kA = static_cast<int>(MdButton::A);
constexpr int kB = static_cast<int>(MdButton::B);
constexpr int kStart = static_cast<int>(MdButton::Start);

void testGameControls() {
    const Settings general;
    const InputConfig generalInput = InputConfig::defaults();

    // [teclado] shares avance_rapido with [velocidad]: a hotkey there is not the option.
    const GameOverrides o = parseGameOverrides(
        "[video]\nfiltro = crt\n[teclado]\na = Z\navance_rapido = F1\n[mando]\nb = y\n", general, generalInput);
    check(o.has(kGameControls) && o.has("filtro") && o.own.size() == 2, "juego: botones propios y filtro");
    check(!o.has("avance_rapido"), "juego: un atajo de [teclado] no es la opción avance_rapido");
    check(o.controls.keys[kA] == std::vector<SDL_Scancode>{SDL_SCANCODE_Z}, "juego: A en la tecla Z");
    check(o.controls.pad[kB] == std::vector<SDL_GameControllerButton>{SDL_CONTROLLER_BUTTON_Y}, "juego: B en el botón Y");
    check(o.controls.keys[kStart] == generalInput.keys[kStart], "juego: los botones que no tiene siguen a los generales");

    const GameOverrides back = parseGameOverrides(formatGameOverrides(o, "juego.md"), general, generalInput);
    check(back.own == o.own && back.controls.keys == o.controls.keys && back.controls.keys2 == o.controls.keys2 &&
              back.controls.pad == o.controls.pad,
          "juego: los botones propios se guardan y se leen igual");

    const GameOverrides plain = parseGameOverrides("[velocidad]\navance_rapido = 6\n", general, generalInput);
    check(plain.has("avance_rapido") && !plain.has(kGameControls), "juego: sin [teclado] ni [mando] no hay botones propios");
    check(formatGameOverrides(plain, "juego.md").find("[teclado]") == std::string::npos,
          "juego: sin botones propios no se escribe [teclado]");
}

}  // namespace

int main() {
    testGameControls();
    std::printf(failures ? "%d fallos\n" : "todo bien\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
