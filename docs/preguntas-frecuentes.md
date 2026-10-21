# Preguntas frecuentes

## Instalar y empezar

### Windows dice que plumsg es una aplicación desconocida

El instalador aún no está firmado digitalmente, así que SmartScreen no lo conoce. Pulsa **Más información > Ejecutar de todas
formas**. El instalador lo genera el CI de GitHub a partir del código del repositorio.

### ¿Dónde están mis partidas, estados y ajustes?

En `%APPDATA%\plumsg\plumsg\` (escribe eso en la barra del Explorador): `plumsg.ini`,
`states\` (estados), `perfiles\`, `trucos\`, `caratulas\`, `grabaciones\`, `sonido\` y
`fallos\`. Las partidas guardadas en el cartucho (`.sav`) van junto a la ROM.

Si quieres llevarlo todo en un pendrive (**modo portátil**), crea un `plumsg.ini` vacío
junto a `plumsg.exe`: desde entonces guarda todo en esa carpeta.

### ¿Se pierden mis estados al actualizar?

No. Los estados de versiones anteriores se cargan en las nuevas, y el CI lo comprueba en
cada cambio con estados guardados por las versiones publicadas.

## Juegos

### ¿De dónde saco los juegos?

plumsg no trae ninguno. Usa copias de los tuyos. Con los nombres de No-Intro o Redump
(como `Sonic The Hedgehog (USA, Europe).md`) la biblioteca acierta casi siempre con la
carátula y la ficha.

### Un juego no arranca o se ve mal

Mira la [lista de compatibilidad](compatibilidad.md). Si no está o está como que va bien,
[avisa del fallo](https://github.com/cochimbo/plumsg/issues/new/choose) con el nombre
exacto del archivo y qué pasa. Antes prueba:

- **Consola > Región**: algunos juegos solo arrancan en su región (y los europeos van a
  50 Hz).
- Quitar los trucos, el overclock y el run-ahead del juego (**Juego > Opciones de este
  juego**).

### Los juegos de Mega CD no arrancan

Necesitan la BIOS de su región (Japón, EE. UU. o Europa), que no viene con plumsg porque
es de Sega. Son archivos de 128 KB, como `bios_CD_U.bin`. Indícala en **Consola > BIOS
del Mega CD**; **Buscar en las carpetas de la biblioteca** las encuentra solas si están
ahí. Los juegos de 32X **no** necesitan BIOS.

### Un disco de Mega CD arranca sin música

La música va en pistas de audio. Abre el `.cue` (no el `.bin`) y comprueba que las
pistas que nombra están en la misma carpeta. Los `.chd` las llevan dentro.

### ¿Qué es "experimental"?

Funciones que van pero aún no en todos los juegos o no del todo pulidas. Se pueden usar
sin riesgo para la partida; si algo sale raro, se desactiva.

## Mandos

### Mi mando no responde

- Se puede conectar en cualquier momento, también con el juego en marcha.
- Valen los mandos que conoce SDL2: Xbox, PlayStation, Switch Pro, 8BitDo y casi todos
  los XInput. Uno que SDL2 no conoce (algunos genéricos baratos) no aparece; muchos de
  esos tienen un modo XInput (a menudo manteniendo un botón al conectarlos).
- Si responde pero con los botones cambiados, asígnalos en **Opciones > Controles**.
- El primer mando es el jugador 1 y el segundo el jugador 2.

### Quiero 6 botones (Street Fighter II, Mortal Kombat)

En el menú **Consola**, bajo **Mando 1** (y **Mando 2**), elige **6 botones**. X, Y y Z son Q, W y E en el teclado. Hay
juegos antiguos que fallan con un mando de 6 botones; para esos, vuelve a 3.

### Botones distintos para un juego

**Juego > Opciones de este juego**, pestaña **Controles**, marca *Botones propios de este
juego*.

## Jugar en red

### No conecta

- El anfitrión pulsa *Alojar* y el otro escribe su dirección. Los dos tienen que tener
  abierto **el mismo juego** (el mismo archivo).
- En casa vale la dirección que enseña la ventana. Por internet, el anfitrión tiene que
  abrir en su router el **puerto UDP 7845** hacia su ordenador y dar su IP pública; más
  fácil: usad los dos Tailscale o ZeroTier y no hay que abrir nada.
- La primera vez Windows pregunta si plumsg puede usar la red: hay que permitirlo. Si se
  dijo que no, cámbialo en **Firewall de Windows > Permitir una aplicación**.

### Va a saltos

Con más de unos 150 ms de ping se nota. Sube el **retardo de los mandos** (2 por defecto)
en la ventana de red: menos saltos a cambio de un poco de retraso al pulsar.

## Rendimiento

### Va lento o a tirones

- Prueba primero en 2D (F3): el 3D pide más a la tarjeta gráfica.
- Quita el reescalado (xBR o Anime4K), el run-ahead y el filtro CRT.
- **F** enseña los FPS y cuánto tarda cada fotograma: si el juego está en 60 y la ventana
  no, es la gráfica; si el juego baja de 60, es el procesador.
- Los juegos de 32X son los que más procesador piden.

## Fallos

### plumsg se cerró de golpe

Deja un informe en `fallos\` (dentro de la carpeta de ajustes) y al abrirlo otra vez te
dice dónde. [Avisa del fallo](https://github.com/cochimbo/plumsg/issues/new/choose)
adjuntando el `.txt` y, en Windows, el `.dmp` del mismo nombre.
