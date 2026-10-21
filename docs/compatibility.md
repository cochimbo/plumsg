# Compatibility

First pass, done on 20 Oct 2026 with version 0.0.31-alpha (`main` at 33e7b6a), using
`plumsg-headless` and the 129 ROMs in Ignacio's collection: 75 Mega Drive, 14 32X, 7 Mega
CD, 23 Master System and 10 Game Gear (a game in two regions counts twice).

**Result: all 129 start.** Darxide and Tempo (32X) did not start in 0.0.31; they are fixed
now (see below).

## How they were tested

Each game runs for 90 seconds (5400 frames) without a window, with an input script that
presses Start, A and C a few times during the first half minute and then walks right,
jumping and attacking. It saves a screenshot every 10 seconds, the sound and the hash of
every frame. Mega CD discs use the BIOS files in the `bios` folder.

Then every screenshot was looked at by hand. The games that looked stuck were run again
with another script: almost all were paused (the script presses Start once in game) or on
a menu that wants another button (on the Master System, Start is Pause and you have to
press 1 or 2).

What this test does **not** check: whether the sound is right (only that it is not
silent), small graphics glitches, anything past the first level, saving, or the 3D. That
needs playing.

## Legend

- **Playable**: gets to gameplay (or to the game's demo).
- **Menus**: gets to the title, intro or menus, and responds to the pad; the script did
  not get further, but no fault was seen.
- **Does not start**: black screen from the start (none now).

## Mega Drive

| Game | Status | Notes |
|---|---|---|
| Adventures of Batman & Robin, The | Playable | |
| After Burner II | Playable | |
| Aladdin | Playable | |
| Alex Kidd in the Enchanted Castle | Playable | |
| Alien Storm | Playable | |
| Altered Beast | Playable | |
| Beyond Oasis | Playable | |
| Castle of Illusion | Playable | |
| Castlevania: Bloodlines | Playable | Gameplay demo |
| Columns | Playable | |
| Columns III | Playable | |
| Combat Cars | Playable | |
| Comix Zone | Playable | |
| Contra: Hard Corps (USA and Japan) | Menus | Reaches the intro |
| Dr. Robotnik's Mean Bean Machine | Playable | |
| Dune: The Battle for Arrakis | Playable | |
| Dynamite Headdy | Playable | |
| Earthworm Jim | Playable | |
| Earthworm Jim 2 | Playable | |
| Ecco the Dolphin | Playable | |
| Flashback | Playable | |
| Ghouls'n Ghosts | Playable | |
| Gods | Playable | |
| Golden Axe | Playable | |
| Golden Axe II | Playable | |
| Golden Axe III | Playable | |
| Gunstar Heroes | Playable | |
| Herzog Zwei | Playable | |
| James Pond II: Robocod | Playable | |
| King of the Monsters 2 | Playable | |
| Landstalker | Playable | |
| Lion King, The | Playable | |
| Mega Bomberman | Playable | |
| Mega Man: The Wily Wars | Menus | Saves to EEPROM |
| Michael Jackson's Moonwalker | Playable | |
| Micro Machines: Turbo Tournament 96 | Menus | J-Cart cartridge |
| Monster World IV | Playable | |
| NHL 96 | Menus | |
| Phantasy Star IV | Playable | |
| Psy-O-Blade (fan translation) | Menus | Intro |
| QuackShot | Playable | |
| Revenge of Shinobi, The | Playable | |
| Ristar | Playable | |
| Rocket Knight Adventures | Playable | |
| Shadowrun | Playable | |
| Shining Force | Menus | Intro |
| Shining Force II | Menus | Character name screen |
| Shinobi III | Playable | |
| Soleil | Playable | |
| Sonic & Knuckles | Playable | |
| Sonic & Knuckles + Sonic 1 | Playable | Shows "No Way?" and Blue Spheres, as on the console |
| Sonic & Knuckles + Sonic 2 | Playable | |
| Sonic & Knuckles + Sonic 3 | Menus | Intro |
| Sonic The Hedgehog | Playable | |
| Sonic The Hedgehog 2 | Playable | |
| Sonic The Hedgehog 3 | Menus | Intro |
| Sonic Spinball | Playable | |
| Story of Thor, The | Playable | |
| Streets of Rage | Playable | |
| Streets of Rage 2 | Playable | |
| Streets of Rage 3 | Playable | |
| Strider | Playable | |
| Super Street Fighter II | Playable | SSF2 mapper |
| Sword of Vermilion | Menus | Intro |
| Taz in Escape from Mars (Europe and USA) | Playable | |
| TMNT: The Hyperstone Heist | Playable | |
| Thunder Force II | Playable | |
| Thunder Force IV | Playable | |
| ToeJam & Earl | Playable | |
| ToeJam & Earl in Panic on Funkotron | Playable | |
| Vectorman | Playable | |
| Wani Wani World | Playable | |
| Wonder Boy in Monster World | Playable | Saves to EEPROM |

## 32X

| Game | Status | Notes |
|---|---|---|
| After Burner Complete | Playable | |
| Blackthorne | Playable | |
| Knuckles' Chaotix | Playable | |
| Darxide | Playable | Fixed: black screen from the start before (see below) |
| Doom | Playable | |
| Kolibri | Menus | |
| Mortal Kombat II | Playable | |
| NBA Jam: Tournament Edition | Menus | Saves to EEPROM. Sin tocar el mando, la demo acaba en negro al rato |
| Pitfall: The Mayan Adventure | Playable | |
| Space Harrier | Playable | |
| Star Wars Arcade | Playable | |
| Tempo | Playable | Fixed: black screen from the start before (see below) |
| Virtua Fighter | Playable | |
| Virtua Racing Deluxe | Playable | |

## Mega CD

| Game | Status | Notes |
|---|---|---|
| Final Fight CD | Menus | Intro |
| Lunar: The Silver Star | Playable | |
| Road Avenger | Menus | Gameplay demo |
| Shining Force CD | Menus | Character name screen |
| Silpheed | Menus | Intro |
| Snatcher | Menus | |
| Sonic CD | Playable | |

## Master System

| Game | Status | Notes |
|---|---|---|
| Alex Kidd in Miracle World | Playable | |
| Alex Kidd in Shinobi World | Playable | |
| Alien 3 | Playable | |
| Double Dragon | Playable | |
| Fantasy Zone | Playable | |
| Golvellius | Playable | |
| New Zealand Story, The | Playable | |
| Ninja Gaiden | Playable | |
| Ninja, The | Playable | |
| Power Strike II | Playable | |
| Renegade | Menus | |
| Shinobi | Playable | |
| Sonic Chaos | Playable | |
| Sonic The Hedgehog | Playable | |
| Sonic The Hedgehog 2 | Playable | |
| Streets of Rage | Playable | |
| Streets of Rage II | Menus | |
| Taito Chase H.Q. | Playable | |
| Wonder Boy | Playable | |
| Wonder Boy III: The Dragon's Trap | Menus | |
| Wonder Boy in Monster Land | Playable | |
| Wonder Boy in Monster World | Playable | |
| Zillion | Playable | |

## Game Gear

| Game | Status | Notes |
|---|---|---|
| Arena | Playable | |
| Ax Battler | Playable | |
| Crystal Warriors | Playable | |
| Defenders of Oasis | Playable | |
| Shining Force II: The Sword of Hajya | Menus | |
| Shinobi | Playable | |
| Shinobi II: The Silent Fury | Playable | |
| Sonic Chaos | Playable | |
| Sonic The Hedgehog: Triple Trouble | Playable | |
| Tails Adventures | Playable | |

## Darxide and Tempo

Both stayed black because the cartridge's boot code (the same in every 32X game) returned
an error and the game stopped:

- **Darxide** was only released in Europe, so it runs in PAL. The 32X said NTSC in its
  mode register, and the boot code checks that it matches the Mega Drive. Now the 32X
  says PAL when the console is PAL.
- **Tempo** sets the SH-2s working straight away, and the second one wrote to the
  communication registers before the 68000 read the checksum there. Now both SH-2s wait
  for the 68000 to read and clear it before jumping to the game, as with the real BIOS,
  which takes a while to compute it.

## To do

- Check whether NBA Jam TE (32X) should end its demo in black when left alone.
- Play the games marked **Menus** by hand, and the list in section 9.5 of the plan (sound,
  raster effects, saving, 3D).
- Test more games: this collection has no 50 Hz PAL games and no 6-button games
  (SF2 Special Champion Edition).
