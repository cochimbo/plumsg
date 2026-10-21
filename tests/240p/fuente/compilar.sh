#!/bin/bash
# Rebuilds ../240pSuite-1.32.bin from the two source archives next to this script, on
# Linux with Debian/Ubuntu's m68k cross compiler (apt install gcc-m68k-linux-gnu):
#   tests/240p/fuente/compilar.sh [work dir]
# The suite's own build is for Windows (SGDK 1.11's .exe tools); this does the same steps
# with SGDK's tool sources built for the host and its prebuilt libmd.a.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$(mktemp -d)}
mkdir -p "$WORK"
cd "$WORK"
tar xJf "$HERE"/240pTestSuite-Genesis-*.tar.xz
tar xJf "$HERE"/SGDK_1_11_Suite-*.tar.xz
GDK=$WORK/sgdk
SUITE=$WORK/240p

# SGDK's tools, built for this machine.
mkdir -p tools
for t in rescomp sizebnd; do gcc -O1 -w -I sgdk/tools/$t/inc -I sgdk/tools/$t/src sgdk/tools/$t/src/*.c -o tools/$t -lm; done
gcc -O1 -w sgdk/tools/fixchecksum.c -o tools/fixchecksum
T=$WORK/tools

# The ROM, as SGDK's makefile.gen builds it.
CC=m68k-linux-gnu-gcc
FLAGS="-m68000 -Wall -fno-builtin -O1 -fomit-frame-pointer -std=gnu89 -fcommon -I. -Isrc -Ires -I$GDK/inc -I$GDK/res -w"
cd "$SUITE"
mkdir -p out/src out/res out/src/boot
(cd res && "$T"/rescomp resources.res resources.s)
OBJS=""
for f in res/*.s *.s; do $CC $FLAGS -c "$f" -o "out/${f%.s}.o"; OBJS="$OBJS out/${f%.s}.o"; done
for f in *.c; do $CC $FLAGS -c "$f" -o "out/${f%.c}.o"; OBJS="$OBJS out/${f%.c}.o"; done
$CC $FLAGS -c src/boot/rom_head.c -o out/rom_head.o
m68k-linux-gnu-ld -T "$GDK"/md.ld -nostdlib --oformat binary -o out/rom_head.bin out/rom_head.o
$CC $FLAGS -c src/boot/sega.s -o out/sega.o
$CC -m68000 -n -T "$GDK"/md.ld -nostdlib -Wl,--build-id=none -Wl,-z,noexecstack out/sega.o $OBJS \
    "$GDK"/lib/libmd.a "$GDK"/lib/libgcc.a -o out/rom.out
m68k-linux-gnu-objcopy -O binary out/rom.out out/rom.bin
"$T"/sizebnd out/rom.bin -sizealign 131072
"$T"/fixchecksum out/rom.bin

echo "$SUITE/out/rom.bin"
sha256sum out/rom.bin "$HERE"/../240pSuite-1.32.bin
