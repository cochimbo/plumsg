# Windows installer

`plumsg.iss` is an [Inno Setup 6](https://jrsoftware.org/isinfo.php) script that builds
`plumsg-<version>-setup.exe`. The installer:

- Copies the emulator (`plumsg.exe`), `SDL2.dll` and any other file next to the executable.
- Creates the Start menu shortcut and, optionally, a desktop one.
- Can add plumsg to the "Open with" menu of Mega Drive ROMs (`.md`, `.gen`, `.smd`,
  `.bin`, `.zip`) without changing the program that opens them by default (`.md` files
  are also Markdown documents).
- Installs without administrator rights (or for all users, if chosen).
- Is in Spanish and English, with the plumsg plum and marquee (`wizard.bmp`,
  `wizard-2x.bmp`) on the welcome and finish pages, the plum on the others
  (`wizard-small.bmp`, `wizard-small-2x.bmp`) and the plumsg icon
  (`src/frontend/assets/plumsg.ico`) on the installer itself.

## Automatic build

The `.github/workflows/windows-installer.yml` workflow builds the emulator with CMake +
MSVC, downloads SDL2, builds the installer and uploads it as an artifact of the run
(`plumsg-windows-installer`). When a `vX.Y.Z` tag is pushed, the installer is attached to
the release.

## Local build

1. Build the emulator in Release and copy `plumsg.exe` and `SDL2.dll` to a `dist\`
   folder at the root of the repository.
2. Run:

   ```bat
   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.0 installer\plumsg.iss
   ```

The installer ends up in `dist-installer\`. If the executable has another name, add
`/DAppExeName=other.exe`.
