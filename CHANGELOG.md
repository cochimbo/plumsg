# Changelog

What each version brings, newest first. Installers are on the
[releases](https://github.com/cochimbo/plumsg/releases) page.

## Unreleased

- Danny Sullivan's Indy Heat (prototype) no longer stays on a black screen
  ([#100](https://github.com/cochimbo/plumsg/issues/100)).

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
