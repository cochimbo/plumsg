# plumsg

Emulador de **Sega Mega Drive / Genesis** para Windows (y Linux) con un efecto
**pseudo-3D** que separa las capas de la consola y las pone a distintas profundidades, y
con **estados guardados** (savestates). Parte de [plumgg](https://github.com/cochimbo/plumgg),
el emulador de Game Gear, del que hereda el Z80, el PSG, el frontend y el efecto 3D.

| 2D | Pseudo-3D (vóxeles) |
|---|---|
| ![ROM de prueba en 2D](docs/captura-2d.png) | ![La misma imagen en pseudo-3D](docs/captura-3d.png) |

Las capturas son de la ROM de prueba del propio proyecto, ejecutada en Linux con Mesa.

## Estado

Primera versión. Lo que hay:

- **CPU 68000** completa, con tiempos por instrucción: pasa 1.000.020 de los 1.000.060
  tests de un solo paso de Tom Harte (los 40 que fallan son casos raros de *address error*).
- **VDP**: modos H32/H40, V28/V30 (PAL), planos A y B con scroll por pantalla, por fila de
  tiles y por línea, scroll vertical por columnas, ventana, sprites (enlaces, límites por
  línea, enmascarado, colisión), prioridades, sombra/resalte, entrelazado, DMA (68000,
  relleno y copia), interrupciones de línea y de cuadro, contador HV.
- **Z80** con su mapa de memoria, BUSREQ/RESET y banco hacia el 68000.
- **Sonido**: YM2612 (con [ymfm](https://github.com/aaronsgiles/ymfm)) y PSG, mezclados y
  remuestreados a la salida.
- **Cartuchos** `.bin`, `.md`, `.gen`, `.smd` (entrelazado) y dentro de `.zip`; SRAM a
  `.sav` junto a la ROM; mapper de Super Street Fighter II.
- **Mandos** de 3 y 6 botones, dos jugadores, región automática o forzada.
- **Estados**: 10 ranuras por juego con fecha y miniatura.
- **Pseudo-3D**: cinco planos (cielo, plano B, plano A, sprites y tiles con prioridad),
  en estilo de capas o de vóxeles, con extrusión de los tiles macizos del plano A.
  Iluminación lateral con sombras suaves entre planos, cámara viva que se balancea
  siguiendo la acción, y la inclinación de la cámara se puede quitar para mirar de frente.
- **Perfiles 3D por juego**: cada ROM puede guardar sus propios ajustes del 3D.
- **Rebobinado, avance rápido y cámara lenta**.
- **Filtros de imagen** para la vista 2D: scanlines o CRT completo.
- **Trucos** Game Genie y Action Replay, guardados por juego.
- **Biblioteca de juegos** con carátulas, que se descargan solas de libretro-thumbnails.

No hay ROMs comerciales en el repositorio ni en CI: las pruebas usan una ROM generada por el
propio proyecto (`tests/test_rom.h`).

## Uso

Arrastra una ROM a la ventana, usa **Archivo > Abrir ROM** o abre la **biblioteca** (F10).

| Tecla | Acción |
|---|---|
| Flechas | Cruceta |
| A, S, D | Botones A, B, C |
| Q, W, E | Botones X, Y, Z |
| Intro | Start |
| Mayús derecha | Mode |
| F3 | Efecto 3D |
| F5 / F8 | Guardar / cargar estado en la ranura actual |
| F4 (mando: clic del stick izquierdo) | Menú de estados |
| F10 o Ctrl+L | Biblioteca de juegos |
| F6 / F7 | Ranura anterior / siguiente |
| Retroceso (mando: gatillo izquierdo), mantener | Rebobinar |
| Tab (mando: gatillo derecho), mantener | Avance rápido |
| F9 | Cámara lenta (activar / desactivar) |
| F1 | Reiniciar |
| F2 | Cambiar de vista (2D / capas de depuración) |
| P | Pausa |
| M | Silenciar |
| F11 o Alt+Intro | Pantalla completa |
| Esc | Mostrar u ocultar la barra de menús |

Todo se puede cambiar en **Opciones > Controles**. El primer mando conectado es el
jugador 1 y el segundo, el jugador 2; el teclado del jugador 2 está vacío por defecto.

- **Estado**: guardar, cargar y elegir ranura; al pasar el ratón sobre una ranura se ve su
  miniatura. El **menú de estados** (F4, o el clic del stick izquierdo del mando) pausa el
  juego y muestra las 10 ranuras con su miniatura: arriba y abajo para elegir, A (o Intro)
  para cargar, Y (o Espacio) para guardar y B (o Esc) para cerrar. Los estados están en `%APPDATA%\plumsg\plumsg\states\` (o en `states\` junto
  al ejecutable si hay un `plumsg.ini` a su lado, modo portátil). Un estado de otra ROM o
  dañado se rechaza sin tocar la partida. Cargar un estado restaura también la SRAM en
  memoria; el `.sav` se sigue escribiendo al salir o al cambiar de ROM.
- **Juego > Velocidad**: cámara lenta y cuántos segundos se pueden rebobinar (20 por
  defecto, hasta 60; 0 lo desactiva), a qué velocidad va el avance rápido (4x) y la cámara
  lenta (50 %). El sonido acompaña: más agudo al acelerar y más grave a cámara lenta. Al
  rebobinar el juego va hacia atrás fotograma a fotograma, en silencio, y al soltar sigue
  desde ahí.
- **Consola**: región (automática según la cabecera, Japón, EE. UU. o Europa/PAL) y tipo
  de mando (3 o 6 botones) de cada puerto. Cambiar la región reinicia el juego.
- **Opciones > Vídeo**: escalado, 4:3 o píxeles cuadrados, y las opciones del 3D
  (estilo, profundidad, inclinación, giro, zoom, grosor, extrusión, sombras, plano B con
  prioridad delante, plano B plano para juegos que pintan el cielo en él). La cámara del
  3D se gira arrastrando con el ratón o con el stick derecho, y se centra con doble clic.
  Tres casillas cambian cómo se nota la profundidad: **Inclinar la cámara** (sin marcar,
  mira de frente y la imagen coincide con la 2D), **Iluminación** (luz lateral sobre los
  bloques y sombras suaves de cada plano sobre los de detrás) y **Cámara viva** (un
  balanceo de 3 a 4 grados que sigue a los sprites y al scroll).
  **Guardar el 3D solo para este juego** crea un perfil con todos esos ajustes (también
  el efecto 3D y la cámara) que se recupera al abrir esa ROM; está en `perfiles\` junto a
  `states\`. Al desmarcarlo, el juego vuelve a los ajustes generales.

- **Archivo > Biblioteca** (F10): todas las ROMs de una carpeta y sus subcarpetas, con su
  carátula, en una cuadrícula que se maneja con el ratón, el teclado o el mando (cruceta
  para elegir, A para jugar, B para cerrar, Y para buscar). La primera vez pide la carpeta.
  **Descargar carátulas que faltan** las busca por el nombre de cada ROM (los nombres de
  No-Intro, como `Sonic The Hedgehog (USA, Europe).md`, aciertan casi siempre) en
  [libretro-thumbnails](https://github.com/libretro-thumbnails/Sega_-_Mega_Drive_-_Genesis)
  y las guarda en `caratulas\` junto a `states\`. También vale una imagen propia: un `.png`
  o `.jpg` con el nombre de la ROM a su lado, o en una carpeta `caratulas` o `covers` de
  la carpeta de ROMs. Si hay carpeta elegida, la biblioteca se abre al arrancar sin juego.
- **Juego > Trucos**: códigos Game Genie (`ABCD-EFGH`, cambian la ROM) y Action Replay
  (`FF0A12:0009`, o `FF0A12:09` para un byte; fijan ese valor de la RAM en cada cuadro).
  Cada uno se activa o desactiva con su casilla y se guarda para ese juego en `trucos\`.
- **Opciones > Vídeo > Filtro 2D**: **Scanlines** (las líneas oscuras entre líneas de la
  tele, más finas donde la imagen es brillante) o **CRT** (además, la pantalla un poco
  curva, la máscara de colores y las esquinas oscuras), con su intensidad. Se notan más
  cuanto más grande es la ventana, mejor a pantalla completa.
- A pantalla completa, **Abrir ROM** usa el explorador propio de plumsg (también con el
  mando) en vez del diálogo de Windows, que se quedaba escondido detrás del juego.

La configuración se guarda en `plumsg.ini`.

## Compilar

Hace falta CMake 3.16+, un compilador de C++17 y SDL2 (si no está instalado, CMake lo
descarga). Dear ImGui y ymfm se descargan solos.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

En Windows, el workflow `windows-installer.yml` genera el instalador con Inno Setup (ver
[installer/README.md](installer/README.md)).

## Pruebas

- `core_tests`: cartucho, mandos, PSG, VDP, la ROM de prueba (imagen y sonido),
  savestates (determinismo, ida y vuelta, archivos dañados), el rebobinado (volver atrás y
  repetir da los mismos fotogramas), el ritmo del avance rápido y la cámara lenta, la
  separación en planos del 3D (con la cámara recta, el 3D tiene que verse igual que el 2D),
  la cámara viva, los perfiles 3D, los códigos de trucos (Game Genie descodificado como
  Genesis Plus GX) y los nombres de la biblioteca.
- `m68k_json_runner --cycles archivos.json.gz`: los tests de un solo paso del 68000
  ([SingleStepTests/680x0](https://github.com/SingleStepTests/680x0)).
- `rom_runner rom.md --frames 300 --png captura.png`: ejecuta una ROM sin ventana y guarda la
  imagen (con `--layers prefijo` también cada capa del VDP, y `--wav` el sonido).
  `rom_runner --write-test-rom prueba.md` escribe la ROM de prueba a un archivo.
- `zex_runner zexdoc.com`: el ejercitador del Z80.
- El emulador también hace capturas desde la línea de comandos:
  `plumsg rom.md --3d --inclinacion 30 --cuadros 120 --captura 3d.png`, con
  `--filtro crt` o `--filtro scanlines`, o de la biblioteca con
  `plumsg --biblioteca carpeta --descargar-caratulas --captura biblioteca.png`.

## Licencias

Código propio bajo la licencia del repositorio. Bibliotecas: SDL2 (zlib), Dear ImGui (MIT),
miniz (MIT, en `third_party/miniz`), stb_image (dominio público o MIT, en `third_party/stb`),
ymfm (BSD-3-Clause). Las carátulas son de libretro-thumbnails y no van en el repositorio.
