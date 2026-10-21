# FAQ

## Installing and getting started

### Windows says plumsg is an unrecognized app

The installer is not digitally signed yet, so SmartScreen does not know it. Click **More
info > Run anyway**. The installer is built by GitHub's CI from the code in this
repository.

### Where are my saves, states and settings?

In `%APPDATA%\plumsg\plumsg\` (type that into the Explorer address bar): `plumsg.ini`,
`states\`, `perfiles\` (per-game settings), `trucos\` (cheats), `caratulas\` (covers),
`grabaciones\` (recordings), `sonido\` (sound) and `fallos\` (crash reports). Cartridge
saves (`.sav`) go next to the ROM.

To carry everything on a USB stick (**portable mode**), create an empty `plumsg.ini` next
to `plumsg.exe`: from then on everything is saved in that folder.

### Will updating break my save states?

No. States saved by earlier versions load in newer ones, and CI checks this on every
change with states saved by the released versions.

## Games

### Where do I get games?

plumsg does not include any. Use copies of your own. With No-Intro or Redump names (like
`Sonic The Hedgehog (USA, Europe).md`) the library almost always finds the right cover
and game details.

### A game does not start or looks wrong

Check the [compatibility list](compatibility.md). If it is not there, or it is listed as
working, [report the bug](https://github.com/cochimbo/plumsg/issues/new/choose) with the
exact file name and what happens. First try:

- **Console > Region**: some games only start in their own region (and European ones run
  at 50 Hz).
- Turning off the game's cheats, overclock and run-ahead (**Game > Options for this
  game**).

### Mega CD games do not start

They need the BIOS for their region (Japan, USA or Europe), which does not come with
plumsg because it belongs to Sega. They are 128 KB files, like `bios_CD_U.bin`. Set them
in **Console > Mega CD BIOS**; **Search the library's folders** finds them by itself if
they are there. 32X games do **not** need a BIOS.

### A Mega CD disc plays without music

The music is on audio tracks. Open the `.cue` (not the `.bin`) and check that the tracks
it names are in the same folder. `.chd` files have them inside.

### What does "experimental" mean?

Features that work, but not yet in every game or not fully polished. They are safe to use
and cannot harm your game; if something looks odd, turn them off.

## Gamepads

### My gamepad does not respond

- It can be plugged in at any time, even while a game is running.
- Gamepads SDL2 knows work: Xbox, PlayStation, Switch Pro, 8BitDo and almost any XInput
  pad. One SDL2 does not know (some cheap generic ones) does not show up; many of those
  have an XInput mode (often by holding a button while plugging them in).
- If it responds but with the buttons mixed up, map them in **Options > Controls**.
- The first gamepad is player 1 and the second is player 2.

### I want 6 buttons (Street Fighter II, Mortal Kombat)

In the **Console** menu, under **Pad 1** (and **Pad 2**), choose **6 buttons**. X, Y and
Z are Q, W and E on the keyboard. Some older games misbehave with a 6-button pad; switch
those back to 3.

### Different buttons for one game

**Game > Options for this game**, **Controls** tab, tick *This game's own buttons*.

## Netplay

### It does not connect

- The host clicks *Host* and the other player types the host's address. Both must have
  **the same game** open (the same file).
- At home, the address the window shows is enough. Over the internet, the host has to
  forward **UDP port 7845** on their router to their computer and give the other player
  their public IP; easier: both use Tailscale or ZeroTier and nothing needs opening.
- The first time, Windows asks whether plumsg may use the network: allow it. If it was
  denied, change it in **Windows Firewall > Allow an app**.

### It stutters

Above about 150 ms of ping it shows. Raise the **input delay** (2 by default) in the
netplay window: fewer jumps in exchange for a little delay when pressing.

## Performance

### It runs slowly or stutters

- Try 2D first (F3): 3D asks more of the graphics card.
- Turn off upscaling (xBR or Anime4K), run-ahead and the CRT filter.
- **F** shows the FPS and how long each frame takes: if the game is at 60 and the window
  is not, it is the graphics card; if the game drops below 60, it is the processor.
- 32X games are the ones that need the most processor.

## Crashes

### plumsg closed suddenly

It leaves a report in `fallos\` (in the settings folder) and tells you where it is the
next time you open it. [Report the bug](https://github.com/cochimbo/plumsg/issues/new/choose)
and attach the `.txt` and, on Windows, the `.dmp` with the same name.
