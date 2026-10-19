<p align="center"><img src="src/frontend/assets/icon.png" alt="La ciruela de plumsg" width="128"><br><img src="docs/marquesina.png" alt="PLUMSG" width="640"></p>

# plumsg

Emulador de **Sega Mega Drive / Genesis**, **32X** y **Mega CD** para Windows (y Linux) con un efecto
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
- **32X** (`.32x`, o un `.bin` con su cabecera): las dos CPU SH-2 con sus módulos
  (división, DMA, temporizadores, puerto serie entre ellas), el VDP de la 32X (los modos de
  píxeles empaquetados, color directo y por tramos, con su paleta y la prioridad frente a
  la Mega Drive), el sonido PWM, los registros de comunicación y la FIFO del 68000. No
  hace falta BIOS: plumsg hace lo que hacen las ROM de arranque. En el 3D, la imagen de la
  32X va plana al fondo y el efecto sigue en los planos de la Mega Drive.
- **Mega CD** (`.cue` con sus `.bin` o `.wav`, `.iso`, `.chd`, o un `.bin` de una sola
  pista de datos): la segunda CPU 68000 a 12,5 MHz con su memoria de programa, la memoria
  de palabras compartida (modos de 2 y 1 Mbit, con las vistas por celdas y por puntos), la
  memoria de guardado (a `.sav` junto al disco), el controlador de CD (con DMA), la unidad
  emulada a nivel de órdenes, el chip gráfico que gira y escala sellos, el temporizador,
  el chip PCM RF5C164 y el audio de CD, todo mezclado con el FM y el PSG. Necesita la BIOS
  de la región del disco (ver abajo).
- **MSU-MD**: cartuchos parcheados por la comunidad para tocar su música desde pistas de
  CD. Basta con dejar junto al cartucho un `.cue` con su mismo nombre (`Sonic.md` y
  `Sonic.cue`, con las pistas que nombre): el juego arranca con el Mega CD conectado, como
  en una consola de verdad (modo 1). Vale la BIOS del Mega CD de cualquier región; sin
  ninguna el juego va solo, sin música en CD, y plumsg lo dice. En la biblioteca ese `.cue`
  no sale como un disco aparte, y la partida se guarda en el cartucho.
- **Mandos** de 3 y 6 botones, dos jugadores, región automática o forzada.
- **Estados**: 10 ranuras por juego con fecha y miniatura.
- **Pseudo-3D**: cinco planos (cielo, plano B, plano A, sprites y tiles con prioridad),
  en estilo de capas o de vóxeles, con extrusión de los tiles macizos del plano A.
  Iluminación lateral con sombras suaves entre planos, cámara viva que se balancea
  siguiendo la acción, y la inclinación de la cámara se puede quitar para mirar de frente.
- **Perfiles 3D por juego**: cada ROM puede guardar sus propios ajustes del 3D.
- **Rebobinado, avance rápido y cámara lenta**.
- **Run-ahead**: quita 1 o más fotogramas de retardo a los mandos, como en RetroArch.
- **Filtros de imagen** para la vista 2D: scanlines o CRT completo.
- **Trucos** Game Genie y Action Replay, guardados por juego, con las listas de trucos de
  cada juego de libretro-database para elegirlos de un menú.
- **Biblioteca de juegos** con carátulas, que se descargan solas de libretro-thumbnails,
  con varias carpetas y una pestaña por consola (Mega Drive, 32X y Mega CD).
- **BIOS del Mega CD**: se indica dónde está la de cada región (no viene con plumsg), y
  cada disco arranca con la de su región.
- **Juego en red** a dobles, con *rollback* como GGPO: cada uno en su ordenador, por
  internet o en casa (ver abajo).
- **RetroAchievements**: los logros, retos y clasificaciones de retroachievements.org,
  con tu cuenta de allí y modo hardcore (ver abajo).

No hay ROMs comerciales en el repositorio ni en CI: las pruebas usan una ROM generada por el
propio proyecto (`tests/test_rom.h`), un programa de 32X propio (`tests/mars/`) y otro de
Mega CD con un disco hecho en memoria (`tests/megacd/`). Tampoco las BIOS del Mega CD.

## Uso

Arrastra una ROM o un disco a la ventana, usa **Archivo > Abrir ROM** o abre la **biblioteca** (F10).

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
| F12 | Parar la emulación: cierra el juego y vuelve a la pantalla de inicio (o a la biblioteca) |
| F2 | Cambiar de vista (2D / capas de depuración) |
| P | Pausa |
| M | Silenciar |
| F11 o Alt+Intro | Pantalla completa |
| Esc | Mostrar u ocultar la barra de menús |

Todo se puede cambiar en **Opciones > Controles**. El primer mando conectado es el
jugador 1 y el segundo, el jugador 2; el teclado del jugador 2 está vacío por defecto.
Cada juego puede tener además **sus propios botones**: en **Juego > Opciones de este
juego**, pestaña **Controles**, marca *Botones propios de este juego* y asigna las teclas y
los botones del mando que quieras; mientras ese juego está abierto sustituyen a los
generales (los atajos como guardar estado o rebobinar siguen siendo los generales).
**Volver a los generales** los quita. Se guardan en el `.ini` del juego en `perfiles\`.

- **Estado**: guardar, cargar y elegir ranura; al pasar el ratón sobre una ranura se ve su
  miniatura. El **menú de estados** (F4, o el clic del stick izquierdo del mando) pausa el
  juego y muestra las 10 ranuras con su miniatura: arriba y abajo para elegir, A (o Intro)
  para cargar, Y (o Espacio) para guardar y B (o Esc) para cerrar. Los estados están en `%APPDATA%\plumsg\plumsg\states\` (o en `states\` junto
  al ejecutable si hay un `plumsg.ini` a su lado, modo portátil). Un estado de otra ROM o
  dañado se rechaza sin tocar la partida. Cargar un estado restaura también la SRAM en
  memoria; el `.sav` se sigue escribiendo al salir, al cambiar de ROM o al parar la emulación.
- **Juego > Parar emulación** (F12): cierra el juego sin cerrar plumsg. Guarda su partida de
  cartucho, sale de la partida en red si la hay y vuelve a la pantalla de inicio, o a la
  biblioteca si tiene carpetas.
- **Juego > Velocidad**: cámara lenta y cuántos segundos se pueden rebobinar (20 por
  defecto, hasta 60; 0 lo desactiva), a qué velocidad va el avance rápido (4x) y la cámara
  lenta (50 %). El sonido acompaña: más agudo al acelerar y más grave a cámara lenta. Al
  rebobinar el juego va hacia atrás fotograma a fotograma, en silencio, y al soltar sigue
  desde ahí.
- **Juego > Latencia > Run-ahead** (desactivado por defecto, de 1 a 4 fotogramas): en cada
  fotograma el juego corre por delante ese número de fotogramas, sin sonido, se ve su
  imagen y vuelve atrás, así lo que pulsas se ve antes. Con 1 suele bastar; cada fotograma
  más pide más al ordenador. También se puede fijar por juego en Opciones de este juego.
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

- **Archivo > Biblioteca** (F10): los juegos de una o varias carpetas y sus subcarpetas,
  con su carátula, en una cuadrícula que se maneja con el ratón, el teclado o el mando
  (cruceta para elegir, A para jugar, B para cerrar, Y para buscar). La primera vez pide la
  carpeta; con **Añadir carpeta...** se suman más y con **Quitar** se sacan. Cada juego
  sabe de qué consola es por su extensión: `.md`, `.gen`, `.smd` y `.bin` son de Mega
  Drive (un `.bin` con la cabecera de la 32X o de un disco se reconoce también), `.32x` de
  32X, y `.cue`, `.chd` e `.iso` de Mega CD; las pistas `.bin` de un `.cue` no salen
  aparte. Las pestañas **Todos**, **Mega Drive**, **32X** y **Mega CD** filtran por
  consola (con el mando, los botones L y R; con el teclado, RePág y AvPág).
  **Descargar carátulas que faltan** las busca por el nombre de cada juego (los nombres de
  No-Intro y Redump, como `Sonic The Hedgehog (USA, Europe).md`, aciertan casi siempre) en
  libretro-thumbnails, en el juego de carátulas de cada consola, y las guarda en
  `caratulas\` junto a `states\` (las de 32X en `caratulas\32X\` y las de Mega CD en
  `caratulas\MegaCD\`). También vale una imagen propia: un `.png` o `.jpg` con el nombre
  del juego a su lado, o en una carpeta `caratulas` o `covers` de la carpeta de juegos.
  Si una no sale, o sale la que no es, **Buscar carátula...** en el menú del juego (botón
  derecho, X en el mando o la tecla de menú) deja cambiar el nombre que se busca y lista
  las carátulas de nombre parecido (vale con parte del nombre, una errata o `2` por `II`);
  al pulsar una se ve, y **Usar esta carátula** la guarda como la del juego. Si
  hay carpetas elegidas, la biblioteca se abre al arrancar sin juego.
- **Consola > BIOS del Mega CD** (o el botón de la biblioteca): el archivo de la BIOS de
  cada región (Japón, EE. UU. y Europa, 128 KB, como `bios_CD_U.bin`). plumsg dice si el
  archivo parece una BIOS y de qué región es, y **Buscar en las carpetas de la
  biblioteca** las encuentra solas. Cada disco usará la BIOS de su región.
- **Juego > Trucos**: códigos Game Genie (`ABCD-EFGH`, cambian la ROM) y Action Replay
  (`FF0A12:0009`, o `FF0A12:09` para un byte; fijan ese valor de la RAM en cada cuadro).
  Cada uno se activa o desactiva con su casilla y se guarda para ese juego en `trucos\`.
  Un código puede ser varios unidos con `+` (`AJ0T-CA9Y+FFA3C5:0006`).
  En **Trucos de internet** salen los de la lista del juego en
  [libretro-database](https://github.com/libretro/libretro-database) (la base de trucos de
  RetroArch, casi 2.100 listas de Mega Drive, más las de 32X y Mega CD), que se descarga al
  abrir la ventana: la del nombre de la ROM primero y en **Lista** otras de nombre parecido
  (las de Action Replay, otras regiones). Se filtran por nombre (vienen en inglés) y
  **Añadir** pone el truco en la lista del juego, activado. Si el juego comprueba la ROM
  (Sonic se queda en rojo), se añade también su **Master Code**.
- **Opciones > Vídeo > Filtro 2D**: **Scanlines** (las líneas oscuras entre líneas de la
  tele, más finas donde la imagen es brillante) o **CRT** (además, la pantalla un poco
  curva, la máscara de colores y las esquinas oscuras), con su intensidad. Se notan más
  cuanto más grande es la ventana, mejor a pantalla completa.
- **Sonido**: volumen, silenciar y el modo de la música. **Original** es el sonido de los
  chips. **Sintetizador** toca la música del FM y del PSG como notas General MIDI sobre un
  SoundFont, como sonaría en un módulo como el Roland SC-55; las voces y la batería digitalizada (el DAC)
  siguen sonando como en la consola. El control **Sonido original** deja oír también el FM y el PSG
  por debajo, y **Reverb** y **Chorus** añaden ambiente. Cada instrumento del juego recibe
  un instrumento General MIDI con unas reglas automáticas, que se cambian a mano en el
  **Editor de instrumentos**. Todo esto se puede cambiar solo para un juego desde sus opciones.

  plumsg trae **GeneralUser GS** en `soundfonts\` junto al ejecutable. Los `.sf2` que se
  dejen en `sonido\soundfonts\` (**Abrir la carpeta de SoundFonts** la abre) salen en el
  desplegable **SoundFont** para elegir uno con un clic, y **Elegir otro (.sf2)...** abre
  uno de cualquier sitio:
  - Formato **SoundFont 2** (`.sf2`). No valen los `.sf3` (comprimidos), `.sfz` ni `.dls`;
    un `.sf3` se puede pasar a `.sf2` con Polyphone.
  - Tiene que estar ordenado como **General MIDI**: los 128 instrumentos en el banco 0 y la
    batería en el banco 128. Los SoundFonts GM, GS y XG lo cumplen; uno de un solo
    instrumento sonará igual en todos los canales o en silencio.
  - Se carga entero en memoria y ocupa unas dos veces lo que pesa: uno de 30 MB va al
    instante, uno de 500 MB tarda unos segundos y necesita 1 GB libre.
  - Si el archivo no se puede leer o no es un SoundFont, plumsg avisa con un mensaje.

  Algunos que suenan bien: GeneralUser GS (el incluido), FluidR3 GM y los que imitan el
  SC-55 o el SC-88.
- **Sonido > Editor de instrumentos** (o **Editor de instrumentos...** en el menú de un juego
  de la biblioteca): para elegir con qué suena cada instrumento sin tener que jugar a la vez.
  - **Grabar del juego** graba la música mientras juegas, hasta **Parar la grabación**. La
    grabación se guarda en `sonido\pistas\` (junto a `states\`) como `.vgz`.
  - **Sacar canciones** busca las canciones en la ROM del juego y las graba una a una en
    `sonido\pistas\<juego>\`, sin nombre ("Canción 1", "Canción 2"...), cada una con una
    vuelta y su punto de vuelta para que siga sonando. Arranca el juego por su cuenta y
    localiza su driver de sonido (el código que toca el chip FM, o el Z80) y cómo le pide el
    juego la música: un byte que el juego escribe y el driver vacía (el SMPS de Sega y
    parecidos), una rutina a la que el juego llama con el número en un registro (como
    Streets of Rage 2) o en la pila (los juegos de EA, como NHL 96, que tienen dos rutinas,
    cada una con sus canciones), o un anillo de órdenes en la memoria del Z80 donde el juego
    deja mensajes (el GEMS de los juegos de Sega of America, como Sonic Spinball). Luego le
    va pidiendo cada número. Probado con Sonic 1, 2 y 3, Gunstar Heroes, Ristar, Shinobi
    III, Golden Axe, Streets of Rage 2, Sonic Spinball, Earthworm Jim, Taz in Escape from
    Mars, NHL 96, Castle of Illusion, QuackShot, Streets of Rage 3, Beyond Oasis y Shadowrun
    (estos tres con el driver en el 68000, que le pasa las notas al Z80). Lo que suena en
    uno o dos canales, aunque dure (un motor, una alarma), cuenta como efecto y no como
    canción. Tarda unos minutos, con una barra de progreso, y se puede parar.
  - De paso aprende cuáles son los **efectos de sonido** del juego (anillos, saltos,
    golpes...): los instrumentos FM que suenan en los sonidos cortos y en ninguna canción.
    Con **Sonido > Efectos de sonido originales** (la opción de serie) suenan como en la
    consola y solo la música pasa por el SoundFont. Se guardan en `sonido\<juego>.efectos`.
  - Los discos de **Mega CD** y los juegos **MSU-MD** suenan siempre originales: su música
    son pistas de audio del CD, y el FM y el PSG solo hacen efectos.
  - La ROM no trae sus canciones con nombre (cada juego guarda su música a su manera), pero
    [vgmrips](https://vgmrips.net) tiene casi todas: su `.zip` de un juego, o sus `.vgm` y
    `.vgz`, se abren con **Abrir .vgm...** y se guardan en `sonido\pistas\<juego>\`
    (**Carpeta** la abre). Desde entonces, la lista **Pista** de ese juego trae cada canción
    por su nombre, como "Green Hill Zone", junto a las grabaciones. También vale el sound
    test del juego: se pone a grabar y se van tocando sus canciones.
  - **Tocar** la reproduce con el juego en pausa. **Original** y **Sintetizador** cambian entre
    los dos sonidos en el mismo punto, para comparar, y la lista **SoundFont** de debajo (la
    misma de **Sonido**) cambia de SoundFont sin salir del editor. Cada canal (FM 1-6, DAC,
    PSG 1-3 y ruido) tiene **Solo** y **Silencio**.
  - La lista enseña los instrumentos de la pista, con sus canales y sus notas. Los que suenan
    en ese momento salen en verde. A cada uno se le puede dar un **Instrumento** General
    MIDI (con octava y volumen), una pieza de la **Batería** GM, o dejarle su **Sonido
    original**. **Escuchar solo este** calla todo lo demás, y **Volver a la sugerencia**
    deshace lo elegido.
  - Lo elegido se oye al momento, también en el juego, y se guarda en
    `sonido\<juego>.ini`. Con **Usar también en otros juegos** (marcado de entrada) va
    además a `sonido\cache.ini`, que comparten todos los juegos: el mismo sonido FM en otro
    juego saldrá ya asignado. Lo de cada juego manda sobre lo común.
- A pantalla completa, **Abrir ROM** usa el explorador propio de plumsg (también con el
  mando) en vez del diálogo de Windows, que se quedaba escondido detrás del juego.

La configuración se guarda en `plumsg.ini`.

### Jugar en red

Menú **Red > Jugar en red...**. Los dos abren antes el mismo juego (la misma ROM).

- Quien **aloja** (jugador 1) pulsa *Alojar*. Mientras espera puede seguir jugando: la
  partida en red empieza en el punto en que esté, con su partida guardada del cartucho,
  su región y sus mandos.
- Quien **se une** (jugador 2) escribe la dirección del anfitrión (`IP` o `IP:puerto`) y
  pulsa *Unirse*. Recibe la partida del anfitrión en un momento y empiezan los dos a la vez.
- Cada uno juega con **sus controles del jugador 1** (teclado o mando).
- En casa basta la dirección que muestra la ventana (la de la red local). Por internet,
  el anfitrión abre (redirige) en su router el **puerto UDP 7845** (se cambia en la
  ventana) hacia su ordenador y le da al otro su IP pública; o los dos usan una red
  privada como Tailscale o ZeroTier y no hay que abrir nada. La primera vez Windows
  pregunta si plumsg puede usar la red: hay que permitirlo.

Cómo funciona: cada consola ejecuta el juego por su cuenta y solo se envían los botones.
Lo que pulsa el otro se adivina (sigue con lo último que pulsó) hasta que llega; si se
adivinó mal, plumsg vuelve atrás unos fotogramas con un estado guardado y los repite con
lo bueno, sin que se note más que un pequeño salto. Así nadie espera a la red mientras el
ping no pase de unos 150 ms. El **retardo de los mandos** (2 fotogramas por defecto)
reduce esos saltos a cambio de un poco de retraso al pulsar. Cada segundo las dos
consolas comparan su memoria; si alguna vez dejaran de coincidir, la ventana lo avisa.

Durante la partida en red no se puede pausar, rebobinar, cambiar la velocidad, cargar
estados, reiniciar, cambiar la región ni usar trucos (harían que las dos consolas
dejaran de ver lo mismo). Guardar estados sí. Al que se une no se le toca su partida
guardada del cartucho: la que usa es la del anfitrión.

### Logros (RetroAchievements)

Menú **Logros**. Activa *RetroAchievements* e inicia sesión en **Logros > Cuenta...** con tu
cuenta de [retroachievements.org](https://retroachievements.org) (es gratis). La contraseña
no se guarda: plumsg guarda en `plumsg.ini` la sesión que da el servidor, y la próxima vez
entra solo.

- Al abrir un juego se busca en RetroAchievements por su hash (el mismo que usan RetroArch
  y los demás emuladores; en el Mega CD, el de la cabecera del disco). Si lo conoce, sale
  un aviso con cuántos logros tienes de ese juego.
- Cada logro conseguido sale en la esquina de abajo a la derecha con su insignia, también
  a pantalla completa y con la barra de menús escondida. Arriba a la izquierda salen los
  retos en marcha, el progreso de un logro que se está consiguiendo y los contadores de
  las clasificaciones.
- **Logros > Logros de este juego...** los lista todos, conseguidos o no, con su progreso.
- **Modo hardcore**: los logros cuentan para la puntuación hardcore y las clasificaciones, pero
  no se puede rebobinar, ir a cámara lenta, usar trucos ni cargar estados (guardarlos sí).
  Al activarlo con un juego abierto, el juego vuelve a empezar.
- Sin conexión, los logros se guardan y se envían cuando vuelve.
- Cada estado guardado lleva al lado (`.logros`) cómo iban los logros en ese momento.

### Desde la línea de comandos (LaunchBox, Playnite, muebles arcade)

```
plumsg.exe "C:\Juegos\Sonic.md" --pantalla-completa --3d
plumsg.exe "C:\Juegos\Sonic CD.cue" --region eu --estado 1
```

| Opción | Qué hace |
|---|---|
| `juego` | el cartucho o disco que se abre |
| `--pantalla-completa` / `--ventana` | a pantalla completa o en ventana (también `--fullscreen` / `--windowed`) |
| `--3d` / `--2d` | con o sin la vista pseudo-3D |
| `--filtro f` | `ninguno`, `scanlines` o `crt` |
| `--region r` | `auto`, `jp`, `us` o `eu` |
| `--estado N` | carga el estado guardado N (0-9) del juego al abrirlo |
| `--biblioteca carpeta` | abre la biblioteca de esa carpeta |
| `--alojar` | aloja una partida en red con el juego (en el puerto del menú Red) |
| `--unirse direccion` | se une a la partida en red de ese anfitrión (`IP` o `IP:puerto`) |
| `--ayuda` | muestra las opciones |

La pantalla completa y la región que se dan así valen solo para esa vez: `plumsg.ini`
sigue como estaba. En LaunchBox, por ejemplo, se añade plumsg como emulador con la ruta
de `plumsg.exe` y `--pantalla-completa` en los parámetros de la línea de comandos.

### Idioma

plumsg está en español, inglés, francés y portugués. La primera vez usa el idioma del
sistema (o inglés si no es ninguno de esos), y se cambia en **Opciones > Idioma**. Cómo
añadir textos o idiomas: [src/frontend/lang/README.md](src/frontend/lang/README.md).

## Compilar

Hace falta CMake 3.16+, un compilador de C++17 y SDL2 (si no está instalado, CMake lo
descarga). Dear ImGui, ymfm y libchdr se descargan solos.

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
  Genesis Plus GX), los nombres de la biblioteca y cómo se reconoce la consola de cada
  juego, las pistas de un `.cue` y las BIOS del Mega CD. De la 32X: instrucciones de la
  SH-2 y sus módulos, el arranque sin BIOS, el dibujo sobre la imagen de la Mega Drive y
  los estados, y un programa en C (`tests/mars/program.c`) que las dos SH-2 ejecutan y
  tiene que dar exactamente lo mismo que el PC (división de libgcc, aritmética de 64 bits
  y de coma flotante...), más interrupciones, paleta y sonido PWM. Se recompila con
  `tests/mars/build.sh` (hace falta `gcc-sh-elf`); el resultado va en el repositorio.
  Del Mega CD: hojas `.cue` (varios archivos, `INDEX 00`, `PREGAP`), un `.chd` hecho con
  chdman (`tests/megacd/disco.chd`), y un programa en ensamblador para las dos CPU
  (`tests/megacd/main.s` hace de BIOS y carga `sub.s` en la CPU secundaria) que prueba
  la comunicación entre ellas, interrupciones, la memoria de guardado, las órdenes a la
  unidad, la lectura de un sector por el controlador y por DMA, el chip gráfico, el PCM,
  el audio de CD y los estados; el mismo programa como cartucho prueba el modo 1 de los
  juegos MSU-MD (el Mega CD en 0x400000) y cómo se encuentra su `.cue`. Se recompila con `tests/megacd/build.sh` (hace falta
  `binutils-m68k-linux-gnu`).
  Del juego en red: dos sesiones conectadas por una red simulada que retrasa, desordena,
  duplica y pierde paquetes tienen que ver los mismos fotogramas entre sí y que una sola
  consola con los dos mandos, aunque vuelvan atrás muchas veces.
- `frontend_tests` (solo con la interfaz, necesita SDL2): las opciones propias de cada
  juego y sus botones, leídos y escritos en su archivo de `perfiles\`.
- `m68k_json_runner --cycles archivos.json.gz`: los tests de un solo paso del 68000
  ([SingleStepTests/680x0](https://github.com/SingleStepTests/680x0)).
- `rom_runner rom.md --frames 300 --png captura.png`: ejecuta una ROM sin ventana y guarda la
  imagen (con `--layers prefijo` también cada capa del VDP, y `--wav` el sonido). Con un
  disco de Mega CD, `--bios archivo` dice qué BIOS usar.
  `rom_runner --write-test-rom prueba.md` escribe la ROM de prueba a un archivo.
- `zex_runner zexdoc.com`: el ejercitador del Z80.
- El emulador también hace capturas desde la línea de comandos:
  `plumsg rom.md --3d --inclinacion 30 --cuadros 120 --captura 3d.png`, con
  `--filtro crt` o `--filtro scanlines`, o de la biblioteca con
  `plumsg --biblioteca carpeta --descargar-caratulas --captura biblioteca.png`
  (`--biblioteca` se puede repetir, `--bios` abre la ventana de las BIOS y
  `--buscar-caratula` la búsqueda de carátula del primer juego de la biblioteca;
  `plumsg rom.md --trucos --captura trucos.png` abre los trucos del juego con su lista y
  `--opciones-juego` sus opciones propias, o `--opciones-juego controles` sus botones).

## Sin ventana: `plumsg-headless`

`plumsg-headless` es el emulador sin ventana, sin sonido y sin tarjeta gráfica, para
pruebas, CI y reproducir fallos. Ejecuta un cartucho o un disco durante N fotogramas,
pulsa los botones que diga un guion y guarda lo que se le pida: capturas PNG, el sonido
en WAV, un hash por fotograma, estados y volcados de la RAM. También comprueba
resultados y dice en qué fotograma una ejecución empieza a ser distinta.

Para compilar solo esto (sin SDL2, ImGui ni OpenGL):

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPLUMSG_BUILD_FRONTEND=OFF
cmake --build build --target plumsg-headless
```

Ejemplos (`plumsg-headless --help` da todas las opciones):

```
# 600 fotogramas y una captura del último
plumsg-headless juego.md --frames 600 --png final.png

# un disco de Mega CD con las BIOS de una carpeta, una captura cada 60 fotogramas
plumsg-headless disco.cue --bios-dir bios/ --frames 1800 --png-every 60 --out-dir capturas

# con un guion de mando, y guardando el hash de cada fotograma
plumsg-headless juego.md --input partida.txt --hash-log hashes.txt

# después de un cambio en el core: ¿en qué fotograma deja de verse igual?
plumsg-headless juego.md --input partida.txt --expect-log hashes.txt
```

El guion tiene una orden por línea: los fotogramas (`N`, `N-M` o `N+K`, contados desde
0) y los botones que se mantienen pulsados en ellos (`up down left right a b c x y z
start mode`, y `p2.start` etc. para el segundo mando), o una orden: `screenshot
[archivo]`, `savestate archivo`, `loadstate archivo`, `dump-ram archivo`, `hash`,
`reset`, `power` o `quit`. Con `--do "120+4 start"` se da una línea desde la línea de
órdenes.

```
120+4    start          # pulsar Start en la pantalla de título
300-420  right
330+10   right c        # saltar mientras anda
420      screenshot nivel.png
600      quit
```

Código de salida: 0 bien, 1 no se pudo leer o escribir algo, 2 opciones o guion mal
escritos, 3 falló una comprobación (`--expect-hash`, `--expect-audio`, `--expect-log`).
Con `--json archivo` (o `-` para la salida estándar) el resumen sale en JSON. Trae tres
ROM propias: `--test-rom` (la de las pruebas), `--pad-test-rom`, que pinta el fondo
según los botones del mando 1, y `--two-pad-test-rom`, en la que cada fotograma depende
de todo lo que pulsaron los dos mandos antes (para probar el juego en red).

También juega en red como la ventana, para probarlo: `--netplay-host PUERTO` aloja y
`--netplay-join IP:PUERTO` se une, cada uno con su guion (el anfitrión usa los botones
del mando 1 y el invitado los de `p2.`). `--net-lag MS` y `--net-loss PORCENTAJE` simulan
una conexión lenta o que pierde paquetes. El `--hash-log` de cada uno tiene que ser igual
al de una partida sin red con los dos guiones juntos:

```
plumsg-headless juego.md --frames 3000 --netplay-host 7845 --net-lag 60 --input p1.txt --hash-log red1.txt &
plumsg-headless juego.md --frames 3000 --netplay-join 127.0.0.1 --net-lag 60 --input p2.txt --hash-log red2.txt
cat p1.txt p2.txt > juntos.txt
plumsg-headless juego.md --frames 3000 --input juntos.txt --expect-log red1.txt
```

Las pruebas de `ctest` lo usan
(`tests/headless_tests.cmake`) y el trabajo `headless` de CI lo compila sin SDL2 y guarda
sus capturas, sonido y hashes como artefacto.

## Licencias

Código propio bajo la licencia del repositorio. Bibliotecas: SDL2 (zlib), Dear ImGui (MIT),
miniz (MIT, en `third_party/miniz`), stb_image (dominio público o MIT, en `third_party/stb`),
ymfm (BSD-3-Clause), libchdr (BSD-3-Clause, para los `.chd`), TinySoundFont (MIT, en
`third_party/tsf`), rcheevos (MIT, para RetroAchievements). El SoundFont incluido es GeneralUser GS de S. Christian Collins, con su
propia licencia libre (`soundfonts\GeneralUser-GS-LICENSE.txt` en la instalación). La 32X y el Mega CD se han escrito con la documentación de Sega y el código de
[ares](https://github.com/ares-emulator/ares) (ISC) como referencia. Las carátulas son de libretro-thumbnails y las listas de trucos de libretro-database
(CC BY-SA 4.0); se descargan al usarlas y no van en el repositorio.
