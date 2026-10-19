// Frontend tests that need SDL's key names but no window: a game's own options and
// controls, read from and written to its file in the perfiles folder.
#define SDL_MAIN_HANDLED
#include <cstdio>
#include <cstdlib>
#include <sstream>
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

void testRunAheadOption() {
    const Settings general;
    const InputConfig generalInput = InputConfig::defaults();
    check(general.runAhead == 0, "run-ahead: desactivado si no se dice");

    // Its own value for a game, and back from its file.
    const GameOverrides o = parseGameOverrides("[velocidad]\nrun_ahead = 1\n", general, generalInput);
    check(o.has("run_ahead") && o.values.runAhead == 1, "run-ahead: un juego con su propio valor");
    const GameOverrides back = parseGameOverrides(formatGameOverrides(o, "juego.md"), general, generalInput);
    check(back.has("run_ahead") && back.values.runAhead == 1, "run-ahead: se guarda y se lee igual en el juego");

    // plumsg.ini keeps it, within 0 and kMaxRunAhead.
    Settings s;
    std::istringstream in("[velocidad]\nrun_ahead = 9\n");
    Settings::parse(in, s);
    check(s.runAhead == Settings::kMaxRunAhead, "run-ahead: no pasa del máximo");
    s.runAhead = 2;
    std::ostringstream out;
    s.write(out);
    Settings again;
    std::istringstream text(out.str());
    Settings::parse(text, again);
    check(again.runAhead == 2, "run-ahead: plumsg.ini lo guarda");
}

void testSpriteLimitOption() {
    const Settings general;
    const InputConfig generalInput = InputConfig::defaults();
    check(!general.unlimitedSprites, "sin límite de sprites: desactivado si no se dice");
    const GameOverrides o = parseGameOverrides("[video]\nsin_limite_sprites = si\n", general, generalInput);
    check(o.has("sin_limite_sprites") && o.values.unlimitedSprites, "sin límite de sprites: un juego con su propio valor");
    const GameOverrides back = parseGameOverrides(formatGameOverrides(o, "juego.md"), general, generalInput);
    check(back.has("sin_limite_sprites") && back.values.unlimitedSprites,
          "sin límite de sprites: se guarda y se lee igual en el juego");
    Settings s;
    s.unlimitedSprites = true;
    std::ostringstream out;
    s.write(out);
    Settings again;
    std::istringstream text(out.str());
    Settings::parse(text, again);
    check(again.unlimitedSprites, "sin límite de sprites: plumsg.ini lo guarda");
}

void testWidescreenOption() {
    const Settings general;
    const InputConfig generalInput = InputConfig::defaults();
    check(general.widescreen == 0, "pantalla ancha: desactivada si no se dice");
    const GameOverrides o = parseGameOverrides("[video]\npantalla_ancha = 16:9\n", general, generalInput);
    check(o.has("pantalla_ancha") && o.values.widescreen == 2, "pantalla ancha: un juego con su propio valor");
    const GameOverrides back = parseGameOverrides(formatGameOverrides(o, "juego.md"), general, generalInput);
    check(back.has("pantalla_ancha") && back.values.widescreen == 2, "pantalla ancha: se guarda y se lee igual en el juego");
    Settings s;
    s.widescreen = 1;
    std::ostringstream out;
    s.write(out);
    Settings again;
    std::istringstream text(out.str());
    Settings::parse(text, again);
    check(again.widescreen == 1, "pantalla ancha: plumsg.ini lo guarda");
}

}  // namespace

int main() {
    testGameControls();
    testRunAheadOption();
    testSpriteLimitOption();
    testWidescreenOption();
    std::printf(failures ? "%d fallos\n" : "todo bien\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
