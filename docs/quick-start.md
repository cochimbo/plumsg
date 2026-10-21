# Quick start

Just enough to be playing in five minutes. Everything else is in the
[README](../README.md), and the usual questions are in the [FAQ](faq.md).

## 1. Install

Download `plumsg-<version>-setup.exe` from the latest
[release](https://github.com/cochimbo/plumsg/releases) and run it. It does not need
administrator rights. Windows may warn that it is an unrecognized app: click **More info >
Run anyway** (see [why](faq.md#windows-says-plumsg-is-an-unrecognized-app)).

On Linux, build it yourself (see [Building](../README.md#building)).

plumsg starts in your system's language if it is English, Spanish, French or Portuguese
(English otherwise); change it in **Options > Language**.

## 2. Open a game

plumsg comes with no games: use your own. Three ways:

- Drag the file onto the window.
- **File > Open ROM...**
- **File > Library** (F10): pick the folder where your games are and they all show up
  with their cover art, sorted by console.

It plays Mega Drive / Genesis, 32X, Master System and Game Gear cartridges (`.md`,
`.bin`, `.gen`, `.smd`, `.32x`, `.sms`, `.gg`, also inside a `.zip`) and Mega CD / Sega
CD discs (`.cue`, `.chd`, `.iso`). **Mega CD discs need the BIOS** for their region: set
it in **Console > Mega CD BIOS**.

## 3. Play

| Key | Console pad |
|---|---|
| Arrows | D-pad |
| A, S, D | A, B, C |
| Q, W, E | X, Y, Z |
| Enter | Start |

A PC gamepad works as player 1 as soon as it is plugged in (a second one is player 2).
Everything can be changed in **Options > Controls**.

The keys you will use most:

| Key | What it does |
|---|---|
| F5 / F8 | Save / load state |
| F4 | States menu (with thumbnails; also the left stick click) |
| Backspace, hold | Rewind |
| Tab, hold | Fast forward |
| F3 | 3D effect |
| F11 | Full screen |
| Esc | Show or hide the menus |
| F12 | Close the game |

## 4. If something goes wrong

- Check the [FAQ](faq.md).
- If a game does not work, see whether it is on the [compatibility list](compatibility.md).
- If plumsg closes suddenly, it leaves a report and tells you where it is the next time
  you open it. Attach it when you [report the bug](https://github.com/cochimbo/plumsg/issues/new/choose).
