# Compatibilidad

Primera pasada, hecha el 20-10-2026 con la versión 0.0.31-alpha (`main` en 33e7b6a), con
`plumsg-headless` y las 129 ROM de la colección de Ignacio: 75 de Mega Drive, 14 de 32X,
7 de Mega CD, 23 de Master System y 10 de Game Gear (un juego en dos regiones cuenta dos
veces).

**Resultado: arrancan los 129.** Darxide y Tempo (32X) no arrancaban en la 0.0.31; ya
están arreglados (ver abajo).

## Cómo se ha probado

Cada juego corre 90 segundos (5400 fotogramas) sin ventana, con un guion de mando que
pulsa Start, A y C unas cuantas veces durante el primer medio minuto y luego anda a la
derecha saltando y atacando. Se guarda una captura cada 10 segundos, el sonido y el hash
de cada fotograma. Los discos de Mega CD usan las BIOS de la carpeta `bios`.

Después se miraron las capturas una a una. Los que parecían parados se repitieron con
otro guion: casi todos estaban en pausa (el guion pulsa Start ya dentro del juego) o en
un menú que pide otro botón (en Master System, Start es Pausa y hay que pulsar 1 o 2).

Lo que esta prueba **no** mira: si el sonido suena bien (solo que no está callado), los
fallos gráficos pequeños, lo que pasa después del primer nivel, los guardados ni el 3D.
Para eso hace falta jugar.

## Leyenda

- **Juego**: llega a jugar (o a la demo del juego).
- **Menús**: llega al título, la introducción o los menús, y responde al mando; el guion
  no pasó de ahí, pero no se vio ningún fallo.
- **No arranca**: pantalla negra desde el principio (ahora ninguno).

## Mega Drive

| Juego | Estado | Notas |
|---|---|---|
| Adventures of Batman & Robin, The | Juego | |
| After Burner II | Juego | |
| Aladdin | Juego | |
| Alex Kidd in the Enchanted Castle | Juego | |
| Alien Storm | Juego | |
| Altered Beast | Juego | |
| Beyond Oasis | Juego | |
| Castle of Illusion | Juego | |
| Castlevania: Bloodlines | Juego | Demo del juego |
| Columns | Juego | |
| Columns III | Juego | |
| Combat Cars | Juego | |
| Comix Zone | Juego | |
| Contra: Hard Corps (USA y Japón) | Menús | Llega a la introducción |
| Dr. Robotnik's Mean Bean Machine | Juego | |
| Dune: The Battle for Arrakis | Juego | |
| Dynamite Headdy | Juego | |
| Earthworm Jim | Juego | |
| Earthworm Jim 2 | Juego | |
| Ecco the Dolphin | Juego | |
| Flashback | Juego | |
| Ghouls'n Ghosts | Juego | |
| Gods | Juego | |
| Golden Axe | Juego | |
| Golden Axe II | Juego | |
| Golden Axe III | Juego | |
| Gunstar Heroes | Juego | |
| Herzog Zwei | Juego | |
| James Pond II: Robocod | Juego | |
| King of the Monsters 2 | Juego | |
| Landstalker | Juego | |
| Lion King, The | Juego | |
| Mega Bomberman | Juego | |
| Mega Man: The Wily Wars | Menús | Guarda en EEPROM |
| Michael Jackson's Moonwalker | Juego | |
| Micro Machines: Turbo Tournament 96 | Menús | Cartucho J-Cart |
| Monster World IV | Juego | |
| NHL 96 | Menús | |
| Phantasy Star IV | Juego | |
| Psy-O-Blade (traducción) | Menús | Introducción |
| QuackShot | Juego | |
| Revenge of Shinobi, The | Juego | |
| Ristar | Juego | |
| Rocket Knight Adventures | Juego | |
| Shadowrun | Juego | |
| Shining Force | Menús | Introducción |
| Shining Force II | Menús | Nombre del personaje |
| Shinobi III | Juego | |
| Soleil | Juego | |
| Sonic & Knuckles | Juego | |
| Sonic & Knuckles + Sonic 1 | Juego | Sale "No Way?" y Blue Spheres, como en la consola |
| Sonic & Knuckles + Sonic 2 | Juego | |
| Sonic & Knuckles + Sonic 3 | Menús | Introducción |
| Sonic The Hedgehog | Juego | |
| Sonic The Hedgehog 2 | Juego | |
| Sonic The Hedgehog 3 | Menús | Introducción |
| Sonic Spinball | Juego | |
| Story of Thor, The | Juego | |
| Streets of Rage | Juego | |
| Streets of Rage 2 | Juego | |
| Streets of Rage 3 | Juego | |
| Strider | Juego | |
| Super Street Fighter II | Juego | Mapper de SSF2 |
| Sword of Vermilion | Menús | Introducción |
| Taz in Escape from Mars (Europa y USA) | Juego | |
| TMNT: The Hyperstone Heist | Juego | |
| Thunder Force II | Juego | |
| Thunder Force IV | Juego | |
| ToeJam & Earl | Juego | |
| ToeJam & Earl in Panic on Funkotron | Juego | |
| Vectorman | Juego | |
| Wani Wani World | Juego | |
| Wonder Boy in Monster World | Juego | Guarda en EEPROM |

## 32X

| Juego | Estado | Notas |
|---|---|---|
| After Burner Complete | Juego | |
| Blackthorne | Juego | |
| Knuckles' Chaotix | Juego | |
| Darxide | Juego | Arreglado: antes negro desde el principio (ver abajo) |
| Doom | Juego | |
| Kolibri | Menús | |
| Mortal Kombat II | Juego | |
| NBA Jam: Tournament Edition | Menús | Guarda en EEPROM. Sin tocar el mando, la demo acaba en negro al rato |
| Pitfall: The Mayan Adventure | Juego | |
| Space Harrier | Juego | |
| Star Wars Arcade | Juego | |
| Tempo | Juego | Arreglado: antes negro desde el principio (ver abajo) |
| Virtua Fighter | Juego | |
| Virtua Racing Deluxe | Juego | |

## Mega CD

| Juego | Estado | Notas |
|---|---|---|
| Final Fight CD | Menús | Introducción |
| Lunar: The Silver Star | Juego | |
| Road Avenger | Menús | Demo del juego |
| Shining Force CD | Menús | Nombre del personaje |
| Silpheed | Menús | Introducción |
| Snatcher | Menús | |
| Sonic CD | Juego | |

## Master System

| Juego | Estado | Notas |
|---|---|---|
| Alex Kidd in Miracle World | Juego | |
| Alex Kidd in Shinobi World | Juego | |
| Alien 3 | Juego | |
| Double Dragon | Juego | |
| Fantasy Zone | Juego | |
| Golvellius | Juego | |
| New Zealand Story, The | Juego | |
| Ninja Gaiden | Juego | |
| Ninja, The | Juego | |
| Power Strike II | Juego | |
| Renegade | Menús | |
| Shinobi | Juego | |
| Sonic Chaos | Juego | |
| Sonic The Hedgehog | Juego | |
| Sonic The Hedgehog 2 | Juego | |
| Streets of Rage | Juego | |
| Streets of Rage II | Menús | |
| Taito Chase H.Q. | Juego | |
| Wonder Boy | Juego | |
| Wonder Boy III: The Dragon's Trap | Menús | |
| Wonder Boy in Monster Land | Juego | |
| Wonder Boy in Monster World | Juego | |
| Zillion | Juego | |

## Game Gear

| Juego | Estado | Notas |
|---|---|---|
| Arena | Juego | |
| Ax Battler | Juego | |
| Crystal Warriors | Juego | |
| Defenders of Oasis | Juego | |
| Shining Force II: The Sword of Hajya | Menús | |
| Shinobi | Juego | |
| Shinobi II: The Silent Fury | Juego | |
| Sonic Chaos | Juego | |
| Sonic The Hedgehog: Triple Trouble | Juego | |
| Tails Adventures | Juego | |

## Darxide y Tempo

Los dos se quedaban en negro porque el código de arranque del cartucho (el mismo en
todos los juegos de 32X) daba error, y el juego se paraba:

- **Darxide** solo salió en Europa, así que va en PAL. La 32X decía en su registro de
  modo que era NTSC, y el código de arranque comprueba que coincida con la Mega Drive.
  Ahora la 32X dice PAL cuando la consola es PAL.
- **Tempo** pone a trabajar las SH-2 enseguida, y la segunda escribía en los registros
  de comunicación antes de que el 68000 leyera allí la suma de comprobación. Ahora las
  dos SH-2 esperan a que el 68000 la lea y la borre antes de saltar al juego, como con
  la BIOS de verdad, que tarda en calcularla.

## Pendiente

- Mirar si NBA Jam TE (32X) debe acabar la demo en negro sin tocar el mando.
- Jugar a mano los que se quedan en **Menús** y la lista de la sección 9.5 del plan
  (sonido, efectos raster, guardados, 3D).
- Probar más juegos: esta colección no tiene juegos PAL a 50 Hz ni de 6 botones
  (SF2 Special Champion Edition).
