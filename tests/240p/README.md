# 240p Test Suite

`240pSuite-1.32.bin` is Artemio Urbina's
[240p Test Suite](https://github.com/ArtemioUrbina/240pTestSuite) for the Mega Drive
(version 1.32), a homebrew program with test patterns and video and sound tests. It is
free software under the GPL v2 ([LICENCIA-240p-GPLv2.txt](LICENCIA-240p-GPLv2.txt)), and
it is linked with the SGDK 1.11 library, which is under the LGPL v3
([LICENCIA-SGDK-LGPLv3.txt](LICENCIA-SGDK-LGPLv3.txt)).

## Source code

The ROM is not the one the author publishes: it is built from its source with the Linux
compiler, and its complete source is in `fuente/`:

- `240pTestSuite-Genesis-89adfdd.tar.xz`: the `240psuite/Genesis/240p` folder of
  github.com/ArtemioUrbina/240pTestSuite at commit
  89adfdd66853746fa9db9105104314ac58021791, without the files only the Mega CD version
  uses.
- `SGDK_1_11_Suite-ea1e49d.tar.xz`: github.com/ArtemioUrbina/SGDK_1_11_Suite at commit
  ea1e49d156ec071c8759517a56d781cce8543d6c (the library, its headers and the source of
  its tools), without the Windows executables, the samples or the documentation.
- `compilar.sh`: builds them again on Linux (`apt install gcc-m68k-linux-gnu`) and checks
  that the same ROM comes out, byte for byte.

Neither has any changes. The "Verify Suite ROM" screen shows 0x78F8E356 rather than the
file's CRC32 (0xB80FCDDB): when there is no Mega CD (its BIOS does not read 0xFF at
0x400070), the program replaces byte 0x70 with 0xFF while computing it, so as not to fail
in an emulator.

## The test

Each `guiones/NAME.txt` is an input script that powers the console on, goes through the
menus to one screen and stops there. The `240p` test in `ctest`
(`tests/240p_tests.cmake`) runs them all and compares the last frame with its hash in
`referencias.txt`; `capturas/NAME.png` is how it should look. If a screen changes, the
test says which one and where the new capture is, to compare them.

If the change is on purpose (a VDP fix, for example), look at the new captures and
update the references:

    cmake -DHEADLESS=build/plumsg-headless -DWORK=/tmp/240p -DUPDATE=ON -P tests/240p_tests.cmake
