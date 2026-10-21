# Guía rápida

Lo justo para jugar en cinco minutos. Todo lo demás está en el [README](../README.md) y
las dudas de siempre en las [preguntas frecuentes](preguntas-frecuentes.md).

## 1. Instalar

Baja `plumsg-<versión>-setup.exe` de la última
[release](https://github.com/cochimbo/plumsg/releases) y ábrelo. No pide permisos de
administrador. Windows puede avisar de que es una aplicación desconocida: pulsa **Más
información > Ejecutar de todas formas** (ver [por qué](preguntas-frecuentes.md#windows-dice-que-plumsg-es-una-aplicación-desconocida)).

En Linux, compílalo (ver [Compilar](../README.md#compilar)).

## 2. Abrir un juego

plumsg no trae juegos: usa los tuyos. Tres formas:

- Arrastra el archivo a la ventana.
- **Archivo > Abrir ROM**.
- **Archivo > Biblioteca** (F10): elige la carpeta donde tienes los juegos y salen todos
  con su carátula, separados por consola.

Valen cartuchos de Mega Drive, 32X, Master System y Game Gear (`.md`, `.bin`, `.gen`,
`.smd`, `.32x`, `.sms`, `.gg`, también dentro de un `.zip`) y discos de Mega CD (`.cue`,
`.chd`, `.iso`). **Los discos de Mega CD necesitan la BIOS** de su región: indícala en
**Consola > BIOS del Mega CD**.

## 3. Jugar

| Tecla | Mando de la consola |
|---|---|
| Flechas | Cruceta |
| A, S, D | A, B, C |
| Q, W, E | X, Y, Z |
| Intro | Start |

Un mando de PC conectado funciona solo como jugador 1 (el segundo, como jugador 2). Todo
se cambia en **Opciones > Controles**.

Las teclas que más se usan:

| Tecla | Qué hace |
|---|---|
| F5 / F8 | Guardar / cargar estado |
| F4 | Menú de estados (con miniaturas; también con el clic del stick izquierdo) |
| Retroceso, mantener | Rebobinar |
| Tab, mantener | Avance rápido |
| F3 | Efecto 3D |
| F11 | Pantalla completa |
| Esc | Mostrar u ocultar los menús |
| F12 | Cerrar el juego |

## 4. Si algo va mal

- Mira las [preguntas frecuentes](preguntas-frecuentes.md).
- Si un juego no va, mira si está en la [lista de compatibilidad](compatibilidad.md).
- Si plumsg se cierra de golpe, deja un informe: al abrirlo otra vez te dice dónde.
  Adjúntalo al [avisar del fallo](https://github.com/cochimbo/plumsg/issues/new/choose).
