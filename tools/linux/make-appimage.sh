#!/usr/bin/env bash
# Packs a Release build of plumsg into plumsg-<version>-x86_64.AppImage, one file that
# runs on most Linux desktops and on the Steam Deck (as a non-Steam game, without Proton).
#
#   tools/linux/make-appimage.sh <build dir> <version> [output dir]
#
# Inside the AppImage plumsg sits in usr/bin/ with what it looks for beside itself, as in
# the Windows installer: models/, soundfonts/ and libpdfium.so. SDL2 and the other
# libraries it links to go to usr/lib/ (linuxdeploy picks them); the graphics drivers
# (libGL, libEGL, Mesa) and glibc stay the system's.
set -euo pipefail

build=${1:?usage: make-appimage.sh <build dir> <version> [output dir]}
version=${2:?usage: make-appimage.sh <build dir> <version> [output dir]}
out=${3:-.}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=$(cd "$build" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)

# Pinned versions, as the Windows installer's workflow pins its own.
LINUXDEPLOY_TAG=${LINUXDEPLOY_TAG:-1-alpha-20250213-2}
GENERALUSER_COMMIT=${GENERALUSER_COMMIT:-main}
PDFIUM_BUILD=${PDFIUM_BUILD:-8138}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
app=$work/AppDir
bin=$app/usr/bin
mkdir -p "$bin"

install -m 755 "$build/plumsg" "$bin/plumsg"
cp -r "$root/assets/models" "$bin/models"

# The SoundFont for the Roland sound: GeneralUser GS (free to use and redistribute; its
# license goes with it).
mkdir -p "$bin/soundfonts"
gu=https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/$GENERALUSER_COMMIT
curl -sSfL -o "$bin/soundfonts/GeneralUser-GS.sf2" "$gu/GeneralUser-GS.sf2"
curl -sSfL -o "$bin/soundfonts/GeneralUser-GS-LICENSE.txt" "$gu/documentation/LICENSE.txt"

# PDFium, for the games' manuals in the 3D view (BSD and Apache licenses).
curl -sSfL -o "$work/pdfium.tgz" \
  "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/$PDFIUM_BUILD/pdfium-linux-x64.tgz"
mkdir -p "$work/pdfium"
tar -xzf "$work/pdfium.tgz" -C "$work/pdfium"
install -m 755 "$work/pdfium/lib/libpdfium.so" "$bin/libpdfium.so"
mkdir -p "$bin/pdfium-licenses"
cp "$work/pdfium/LICENSE" "$bin/pdfium-licenses/pdfium-LICENSE.txt"
cp "$work/pdfium/licenses/"* "$bin/pdfium-licenses/"

cp "$root/LICENSE" "$bin/LICENSE.txt"

# The menu entry and the plum icon.
cp "$here/plumsg.desktop" "$work/plumsg.desktop"
cp "$root/src/frontend/assets/icon.png" "$work/plumsg.png"

curl -sSfL -o "$work/linuxdeploy" \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-x86_64.AppImage"
chmod +x "$work/linuxdeploy"

# Without FUSE (containers, some CI runners) the AppImage tools unpack themselves instead.
export APPIMAGE_EXTRACT_AND_RUN=1
export LINUXDEPLOY_OUTPUT_VERSION=$version
# libpdfium.so is not given to linuxdeploy: it stays in usr/bin/, where plumsg loads it from
# when a manual is opened, and it needs nothing beyond glibc.
(cd "$work" && ./linuxdeploy --appdir "$app" \
  --executable "$bin/plumsg" \
  --desktop-file "$work/plumsg.desktop" \
  --icon-file "$work/plumsg.png" \
  --output appimage)

mv "$work"/plumsg-*.AppImage "$out/plumsg-$version-x86_64.AppImage"
echo "$out/plumsg-$version-x86_64.AppImage"
