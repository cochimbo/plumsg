# Changelog

What each version brings, newest first. Installers are on the
[releases](https://github.com/cochimbo/plumsg/releases) page.

## Unreleased

- **Mega CD 32X** (#96): the six Mega CD 32X discs (Corpse Killer, Night Trap,
  Fahrenheit, Slam City, Supreme Warrior, Surgical Strike) run with the Mega CD and the
  32X together, the 32X with no cartridge: its boot ROMs take the SH-2s' program from
  the frame buffer, as the real ones do on a Mega CD, and the 32X's PWM is mixed with the
  Mega CD's PCM and CD audio. Corpse Killer only reads pad 1 with **Sin mando** (or the
  Menacer) in port 2.
- **Mode 1 for 32X cartridges** (#96): a 32X cartridge with a `.cue` of its name next to
  it (the MSU-32X patches) starts with the Mega CD attached and plays its CD music, as
  MSU-MD cartridges already did.
- **Light guns** (#102): Consola > Mando 2 (or a game's options) plugs in Konami's
  Justifier (Lethal Enforcers I and II, Snatcher), with its second gun for player 2, or
  Sega's Menacer (T2: The Arcade Game, Menacer 6-Game Cartridge) instead of pad 2. A
  crosshair on the game aims each gun, moved with the mouse or with the pad's left stick
  (or d-pad); the left click or A pulls the trigger, the right click or C shoots off the
  screen to reload, and the middle click or Start is Start.
- The 3D shelf's Master System is a real 3D model too, with its two pads in front, plugged in, and
  its own cartridge wearing each game's label: "Sega Master System" by dark_igorek
  (CC BY 4.0).
- Game Gear VDP timing (#76): against its H counter a Game Gear's line starts later than
  a Master System's, so its V counter changes a cycle later and the latched H counter is
  2 cycles ahead. VDPTEST's "GG 2 ASIC" test now passes all of its data, misc and sprite
  tests (it failed 6).

## 0.1.0-beta.8

- **The library's 3D shelf** ("Cajas 3D", next to Cuadrícula and Lista): the games'
  cases stand on a shelf with their spines out; the wheel zooms out to see it all and
  dragging moves along it. A click (or A) takes a box out, and dragging turns it to see
  its back; a second click opens its lid and brings out the cartridge (the disc on the
  Mega CD), which dragging turns too; a third puts it into the console, which rises from
  the floor, its power light comes on and the game starts. A click anywhere else puts
  things back a step. The cartridges and consoles are built from their real
  measurements: the Mega Drive and Master System of the first model, the Game Gear, the
  32X in the Mega Drive's slot and the first Mega-CD under it, with its tray. The Mega
  Drive is a real 3D model, "Sega Mega Drive" by synthetic worlds (Jon Blenky, CC BY
  4.0), and so is its cartridge, "Sonic 2 Mega Drive Cartridge" by Naser (CC BY 4.0),
  wearing each game's label; a .glb of your own with the same name in the "models"
  folder next to plumsg.ini takes its place.
- "Etiqueta del cartucho..." in a game's menu puts a scan of its cartridge label on the
  3D cartridge; labels in an "etiquetas" (or "labels", "Named_Labels") folder next to
  the ROMs are found by the game's name.
- "Portada completa..." in a game's menu takes a whole sleeve (back, spine and front side
  by side, as The Cover Project has them): the 3D box wears all of it, and the grid and
  the details panel show its front.
- **ScreenScraper** (screenscraper.fr): "Medios de ScreenScraper..." in a game's menu (or
  in the details panel) identifies the game by its ROM (CRC32, name and size; a disc by
  its name, or any game by its title if that fails) and downloads what ScreenScraper has
  of it: front cover, whole sleeve, back cover, 3D box, cartridge label, cartridge,
  screenshot, title screen, logo, marquee and fan art, and on request the video and the
  manual. Its window shows everything downloaded (the cursor over one enlarges it, a
  click opens it) with ScreenScraper's data of the game; if it is the wrong game,
  **Buscar otro juego** searches by name in any console, shows each result's cover and
  **Usar este juego** downloads that one's instead. The sleeve becomes the game's cover
  (the 3D box wears it) and the label goes on its cartridge. **Descargar de ScreenScraper**
  in the library does every game that has not been done yet, one by one, keeping the
  covers already there unless ScreenScraper has the whole sleeve. ScreenScraper asks for
  a developer account of the program as well as (optionally) the user's free account,
  both in the window's **Cuenta** tab; builds can carry the developer one
  (`PLUMSG_SCREENSCRAPER_DEVID` and `PLUMSG_SCREENSCRAPER_DEVPASSWORD` in CMake, from the
  repository's secrets in the installer's build).

## 0.1.0-beta.7

- **A look of its own**: the interface takes its style from the console, black ribbed
  plastic with the red of its "16-BIT" and chrome italic lettering, in the Chakra Petch
  font. Window title bars, the "Jugar" button, the pad's buttons in the controls help
  and the grid behind the library's covers match it.
- "Opciones de este juego": each option has a General / Propio switch, and the on/off
  ones a "Sí" / "No" list, instead of two boxes on a row.
- The library's grid has a "Tamaño" slider to make the covers smaller or bigger.
- Game Gear: World Series Baseball saves to the EEPROM on its cartridge and no longer
  stays black ([#77](https://github.com/cochimbo/plumsg/issues/77)); Primal Rage gets
  past its title screen, as the link port now reads back what the game writes to it
  ([#78](https://github.com/cochimbo/plumsg/issues/78)); and the last line of the
  pictures in Madou Monogatari I is no longer garbled: a change of sprite size at the
  end of a line waits a line, as on the console
  ([#82](https://github.com/cochimbo/plumsg/issues/82)).

## 0.1.0-beta.6

- **Virtua Racing** runs: the SVP chip on its cartridge (the SSP1601 DSP and its memory
  controller) is emulated.
- Master System: a BIOS dump loaded as a game ("[BIOS] Alex Kidd in Miracle World...")
  no longer stays on the Sega logo: it finds the slots empty and runs the game it has
  inside ([#136](https://github.com/cochimbo/plumsg/issues/136)).
- 32X: the reset button resets only the 68000, as on the console, and the SH-2s go back
  to their boot ROM; BC Racers starts again after a reset. testpico passes all its 57
  tests ([#69](https://github.com/cochimbo/plumsg/issues/69)).
- misc_test passes 23 of its 28 checks: the 68000's prefetch and DRAM
  refresh, the Z80's reset, bus request and waits during DMA, and the VDP's sprite
  table cache ([#137](https://github.com/cochimbo/plumsg/issues/137)). Sega's softcheck
  says OK to every test ([#117](https://github.com/cochimbo/plumsg/issues/117)), and
  m68k_opcode_sizes gets both checksums right
  ([#111](https://github.com/cochimbo/plumsg/issues/111)).
- Another World (Europe) starts on a PAL console: the top of the picture no longer goes
  blank when it flips pages.
- Honoo no Toukyuuji Dodge Danpei finds its EEPROM and gets past the SEGA logo
  ([#115](https://github.com/cochimbo/plumsg/issues/115)).
- Pad 2 can be unplugged ("Sin mando"), for games such as ESPN Sunday Night NFL that make
  player 2 the computer only with the second port empty
  ([#93](https://github.com/cochimbo/plumsg/issues/93)).

## 0.1.0-beta.5

- Master System: the games made only for European 50 Hz consoles (Back to the Future
  Part III, Sonic 2, Sensible Soccer, RoboCop 3, Home Alone and others) start on a PAL
  console instead of a black screen
  ([#89](https://github.com/cochimbo/plumsg/issues/89)).
- Paprium no longer runs at half speed when enemies are on screen: the copies of the
  sprites' graphics from its chip no longer wait for the VDP's timed DMA
  ([#68](https://github.com/cochimbo/plumsg/issues/68)).
- Master System: the Codemasters games (Micro Machines, Cosmic Spacehead, the Dizzy
  games) start on a PAL console too, as they were made for one: the top of the
  picture of Micro Machines no longer flashes
  ([#74](https://github.com/cochimbo/plumsg/issues/74)).
- The "Opciones de este juego" tab draws each option's control again, instead of
  an empty box next to its name.

## 0.1.0-beta.4

- The VDP writes through its 4-word FIFO in its access slots, with timed DMA:
  VDPFIFOTesting passes all its 122 tests ([#90](https://github.com/cochimbo/plumsg/issues/90)),
  and testpico 55 of its 57 ([#69](https://github.com/cochimbo/plumsg/issues/69)).
- Master System: the VDP's counters, interrupts, latched H counter and sprite
  collisions match the hardware, and SMSVDPTest passes everything
  ([#72](https://github.com/cochimbo/plumsg/issues/72)).
- "Direct colour" DMA pictures (colours written to the backdrop with the display off)
  show, steady ([#95](https://github.com/cochimbo/plumsg/issues/95)).
- Sprite masking after a line that ran out of sprite pixels, as on the console: the
  Sprite Masking Test ROM passes all 9 tests in H32 and H40
  ([#101](https://github.com/cochimbo/plumsg/issues/101)).
- Mega CD: KRIKzz's mcd-verificator runs, loaded as a cartridge in place of the BIOS
  (with a .cue of the same name in the tray), and passes 16 of its 19 groups; the CD
  controller's flags, DMA and interrupts behave as it measures
  ([#80](https://github.com/cochimbo/plumsg/issues/80)).
- Paprium runs: the chip on its cartridge is emulated (it unpacks the graphics, builds
  the sprites, scales the pictures in the menus, keeps the saves and plays the sound
  effects), so the game gets past its "Vapor Ware" decoy to the NX terminal, the intro,
  the menus and the levels ([#68](https://github.com/cochimbo/plumsg/issues/68)). Its
  music is not in the ROM: it plays from the album's tracks (.ogg, .mp3 or .wav) put in
  a folder with the ROM's name next to it.
- The Score's figures no longer change as it scrolls: they are written from a bar line
  before the view, and each staff keeps its clef.

## 0.1.0-beta.3

- **Score** (Sound > Score): the music's notes on staves as it plays, one track per
  instrument like GarageBand, written as a score: in 4/4 at the tempo guessed from the
  music (or set by hand), with whole to sixteenth notes, dots, ties, stems, flags and
  beams, rests, bar lines, clefs, sharps and naturals. Play/pause, stop,
  x0.5/x1/x2 speed, a playhead to drag along the instrument editor's tracks, and a switch
  to mute each instrument. It works with the original sound too. The instrument editor
  gets a Hear column to switch instruments off as well.

## 0.1.0-beta.2

- Danny Sullivan's Indy Heat (prototype) no longer stays on a black screen
  ([#100](https://github.com/cochimbo/plumsg/issues/100)).
- Trouble Shooter reads the pad right: Start and A work and C no longer pauses
  ([#91](https://github.com/cochimbo/plumsg/issues/91)).
- QuackShot (Rev A), Triple Play - Gold Edition, Tyrants - Fight Through Time and Sesame
  Street Counting Cafe no longer stay on a black screen
  ([#97](https://github.com/cochimbo/plumsg/issues/97),
  [#92](https://github.com/cochimbo/plumsg/issues/92),
  [#99](https://github.com/cochimbo/plumsg/issues/99),
  [#75](https://github.com/cochimbo/plumsg/issues/75)).
- Marko's Magic Football (Mega CD) draws its sky, its level map and its levels right
  ([#79](https://github.com/cochimbo/plumsg/issues/79)).

## 0.1.0-beta.1

- **English names on disk and on the command line.** The settings folders (`profiles`,
  `cheats`, `covers`, `game-info`, `sound`, `recordings`, `crash-reports`), the keys and
  values in `plumsg.ini` and the per-game files, the command-line options and the crash
  reports are now in English. Folders and files from earlier versions are renamed the
  first time plumsg opens, old `.ini` files keep reading (and are saved in English), and
  the Spanish options (`--pantalla-completa`, `--biblioteca`...) still work.
- Compatibility list: [129 games tested](docs/compatibility.md), all of them start.
- 32X: Darxide (PAL) and Tempo now start.
- If plumsg closes suddenly, it leaves a report in `crash-reports\` and says so the next time it
  opens.
- About shows the version.
- Library: a game's cover can be set from an image of your own (PNG or JPEG), with "Cover
  from image..." in the game's menu or "Choose image from computer..." in Find cover, for
  when it is not found online.
- Error messages say what to do: a file that is not a game, a state that does not load or
  save, no internet for covers or cheats.
- Opening a folder as if it were a game no longer closes plumsg.
- Get songs is marked experimental on the Master System and Game Gear, where it does not
  find the music of many games yet.
- Quick start guide, FAQ, this changelog and a bug report template; all documentation is
  now in English.
- Tests: save states from earlier versions keep loading, and the 240p Test Suite checks 35
  video screens on every change.

## 0.0.31 (2026-10-20)

- Get songs: the final count no longer stops at the first song.

## 0.0.30 (2026-10-20)

- NBA Jam TE on 32X starts (cartridge serial EEPROM).
- States menu: System tab (pause, reset, stop emulation).
- Roland sound: better instrument suggestions, FM drums, and choosing the instrument by
  comparing the sound.
- Game details: Japanese and other scripts instead of `?????`.
- Achievements: the challenge indicator no longer stays on top of the library.

## 0.0.29 (2026-10-20)

- 32X: SH-2 cache (Blackthorne goes from 5 to 30 fps) and fixed timers.

## 0.0.28 (2026-10-20)

- FPS counter with a graph (F).
- Faster 32X.
- Experimental upscaling of the 2D view: xBR and Anime4K.
- `.chd` files can no longer overflow the stack.

## 0.0.27 (2026-10-20)

- Library: details panel and a sortable list view.

## 0.0.26 (2026-10-20)

- States menu with option tabs.

## 0.0.25 (2026-10-19)

- F11 and Alt+Enter: full screen also with the library or another window open.

## 0.0.24 (2026-10-19)

- Get songs: Ecco the Dolphin.

## 0.0.23 (2026-10-19)

- Instrument editor: Sonic 1, aligned tables, equalizer and "All back to suggestions".

## 0.0.22 (2026-10-19)

- Widescreen: each side adjustable live and savable per game.

## 0.0.21 (2026-10-19)

- **Master System and Game Gear**: VDP, Sega and Codemasters mappers, FM (YM2413), TMS9918
  modes, the Game Gear's screen and palette, cheats, Roland sound and Get songs.
- Run-ahead, no sprite limit, overclock and widescreen.
- Record video (`.avi`) or GIF.

## 0.0.20 (2026-10-19)

- **RetroAchievements**: achievements, leaderboards and hardcore mode.
- plumsg in English, French and Portuguese.
- Get songs: Streets of Rage 3, Beyond Oasis, Castle of Illusion, QuackShot, NHL 96 and
  other EA games.

## 0.0.19 (2026-10-18)

- **MSU-MD**: cartridges with their music on CD tracks.
- Each game's own buttons.
- Get songs: GEMS games (Sonic Spinball, Earthworm Jim, Taz).

## 0.0.18 (2026-10-18)

- Cheats from the internet: each game's lists from libretro-database.
- Get songs finds how the game asks for its music (Streets of Rage 2 too).

## 0.0.17 (2026-10-18)

- Library: "Find cover..." in the game's menu.

## 0.0.16 (2026-10-18)

- Original sound effects with the Roland sound.
- Plum icon and **Game > Stop emulation** (F12).

## 0.0.14 (2026-10-18)

- **Roland sound**: FM and PSG music played on a SoundFont, with its instrument editor.
- Get songs from the ROM.

## 0.0.9.1 (2026-10-18)

- Mega CD: Silpheed no longer freezes as stage 1 starts.
- Per-game options and a right-click menu in the library.

## 0.0.9 (2026-10-17)

- PLUMSG marquee; the mouse pointer hides while playing.

## 0.0.8 (2026-10-17)

- Two-player **netplay** with rollback.

## 0.0.7.1 (2026-10-17)

- `plumsg-headless`, the emulator without a window, for tests.
- Opening games from the command line (LaunchBox, Playnite).
- Mega CD: sound, region and Snatcher.

## 0.0.7 (2026-10-17)

- **Mega CD**: `.cue`, `.iso` and `.chd`, the second CPU, PCM, graphics and CD audio.

## 0.0.6 (2026-10-17)

- **32X**, without a BIOS.

## 0.0.5 (2026-10-17)

- Library with several folders and a tab per console; Mega CD BIOS.

## 0.0.4 (2026-10-17)

- Library with cover art, cheats and CRT filters.

## 0.0.3 (2026-10-17)

- Rewind, fast forward, slow motion, per-game 3D profiles and lighting.

## 0.0.2 (2026-10-17)

- States menu usable with a gamepad.
- VDP: Contra Hard Corps and horizontal blanking.
- 3D: the background no longer shows through the floor.

## 0.0.1 (2026-10-17)

- First version: Mega Drive with pseudo-3D and save states.
