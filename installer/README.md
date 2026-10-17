# Instalador de Windows

`plumsg.iss` es un script de [Inno Setup 6](https://jrsoftware.org/isinfo.php) que genera
`plumsg-<versión>-setup.exe`. El instalador:

- Copia el emulador (`plumsg.exe`), `SDL2.dll` y cualquier otro archivo que acompañe al ejecutable.
- Crea el acceso directo en el menú Inicio y, opcionalmente, en el escritorio.
- Puede añadir plumsg al menú "Abrir con" de las ROMs de Mega Drive (`.md`, `.gen`, `.smd`,
  `.bin`, `.zip`) sin cambiar el programa que las abre por defecto (`.md` también son
  documentos Markdown).
- Se instala sin pedir permisos de administrador (o para todos los usuarios, si se elige).
- Está en español e inglés.

## Compilación automática

El workflow `.github/workflows/windows-installer.yml` compila el emulador con CMake + MSVC,
descarga SDL2, genera el instalador y lo sube como artefacto de la ejecución
(`plumsg-windows-installer`). Al publicar un tag `vX.Y.Z` el instalador se adjunta a la release.

## Compilación local

1. Compila el emulador en Release y copia `plumsg.exe` y `SDL2.dll` a una carpeta `dist\`
   en la raíz del repositorio.
2. Ejecuta:

   ```bat
   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.0 installer\plumsg.iss
   ```

El instalador queda en `dist-installer\`. Si el ejecutable tiene otro nombre, añade `/DAppExeName=otro.exe`.
