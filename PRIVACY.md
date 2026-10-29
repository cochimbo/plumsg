# Privacy Policy

_Last updated: 28 October 2026_

plumsg is a free, open-source emulator that runs entirely on your computer. It has no
accounts of its own, no servers of its own, no telemetry, no analytics, no advertising
and no automatic update checks. The plumsg project does not collect, receive or store
any personal data.

## Data stored on your computer

Everything plumsg keeps stays in its settings folder on your machine (next to
`plumsg.ini`) and is never sent anywhere by plumsg:

- Settings, controls, save states, cartridge saves, recordings and screenshots.
- Downloaded covers, game descriptions and cheat files (as a cache).
- Your RetroAchievements user name and login token, if you sign in (your password is
  not stored).
- Your ScreenScraper user name and password, if you enter them (ScreenScraper has no
  tokens: the password goes with every request, so it is kept in `plumsg.ini` in plain
  text), and the media downloaded from it.
- Crash reports (`crash-reports/`): when plumsg crashes it writes a text report and, on
  Windows, a minidump. They are only written locally; plumsg never uploads them. It is
  up to you whether to share one, for example by attaching it to a GitHub issue.

You can delete any of this at any time by deleting the files or the settings folder.

## Network connections

plumsg only connects to the internet for the features below. Each request goes directly
from your computer to the third-party service named; the plumsg project does not
operate any of them and never sees the traffic. Like any internet request, these
services can see your IP address and the request itself, and their own privacy
policies apply.

| Feature | Service | What is sent |
|---|---|---|
| Box art covers for the library and "Find cover" | thumbnails.libretro.com, raw.githubusercontent.com, api.github.com | The game's name (to look up the image file) |
| Game details, descriptions and screenshots | raw.githubusercontent.com (libretro-database), Wikipedia (`<language>.wikipedia.org`), libretro-thumbnails | The game's name / title and your interface language |
| Cheats from the internet | raw.githubusercontent.com, api.github.com (libretro-database) | The game's name (to look up the cheat file) |
| ScreenScraper media (optional, only when you ask for them) | api.screenscraper.fr and its media servers | plumsg's developer account, your ScreenScraper user name and password if you entered them, the game's file name, size and CRC32 (or its title when searching) |
| RetroAchievements (optional, off unless you sign in) | retroachievements.org and its media servers | Your user name and password (at sign-in) or token, a hash identifying the loaded game, and the achievements, leaderboard entries and rich presence you earn while playing |
| Netplay (optional, only when you host or join) | The other player's computer, directly over UDP | Controller inputs and a save state of the game; both players see each other's IP address. There is no relay or matchmaking server |

ROM files, BIOS files and your personal files are never uploaded.

## Children

plumsg does not knowingly collect information from anyone, including children.

## Changes

Changes to this policy are published in this file in the
[plumsg repository](https://github.com/cochimbo/plumsg), where its history is public.

## Contact

Questions about privacy can be raised as an issue at
<https://github.com/cochimbo/plumsg/issues>.
