# Changelog

What each version brings, newest first. Installers are on the
[releases](https://github.com/cochimbo/plumsg/releases) page.

## Unreleased

- Master System: the games made only for European 50 Hz consoles (Back to the Future
  Part III, Sonic 2, Sensible Soccer, RoboCop 3, Home Alone and others) start on a PAL
  console instead of a black screen
  ([#89](https://github.com/cochimbo/plumsg/issues/89)).
- Paprium no longer runs at half speed when enemies are on screen: the copies of the
  sprites' graphics from its chip no longer wait for the VDP's timed DMA
  ([#68](https://github.com/cochimbo/plumsg/issues/68)).

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
