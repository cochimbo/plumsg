# 240p Test Suite

`240pSuite-1.32.bin` es el [240p Test Suite](https://github.com/ArtemioUrbina/240pTestSuite)
de Artemio Urbina para Mega Drive (versión 1.32), un programa casero con cartas de
ajuste y pruebas de vídeo y sonido. Es software libre, con licencia GPL v2
([LICENCIA-240p-GPLv2.txt](LICENCIA-240p-GPLv2.txt)), y va enlazado con la biblioteca
de SGDK 1.11, que tiene licencia LGPL v3
([LICENCIA-SGDK-LGPLv3.txt](LICENCIA-SGDK-LGPLv3.txt)).

## El código fuente

La ROM no es la que publica el autor: está compilada desde su código con el
compilador de Linux, y su código completo está en `fuente/`:

- `240pTestSuite-Genesis-89adfdd.tar.xz`: la carpeta `240psuite/Genesis/240p` de
  github.com/ArtemioUrbina/240pTestSuite en el commit
  89adfdd66853746fa9db9105104314ac58021791, sin los archivos que solo usa la versión
  de Mega CD.
- `SGDK_1_11_Suite-ea1e49d.tar.xz`: github.com/ArtemioUrbina/SGDK_1_11_Suite en el
  commit ea1e49d156ec071c8759517a56d781cce8543d6c (la biblioteca, sus cabeceras y el
  código de sus herramientas), sin los ejecutables de Windows, los ejemplos ni la
  documentación.
- `compilar.sh`: los compila otra vez en Linux (`apt install gcc-m68k-linux-gnu`) y
  comprueba que sale la misma ROM, byte a byte.

Ninguno lleva cambios. La pantalla "Verify Suite ROM" da 0x78F8E356 y no la CRC32 del
archivo (0xB80FCDDB): cuando no hay Mega CD (su BIOS no lee 0xFF en 0x400070), el
programa cambia el byte 0x70 por 0xFF al calcularla, para no fallar en un emulador.

## La prueba

Cada `guiones/NOMBRE.txt` es un guion de mando que enciende la consola, va por los
menús hasta una pantalla y para ahí. La prueba `240p` de `ctest`
(`tests/240p_tests.cmake`) los pasa todos y compara el último fotograma con su hash en
`referencias.txt`; `capturas/NOMBRE.png` es cómo debe verse. Si una pantalla cambia,
la prueba dice cuál y dónde está la captura nueva para compararlas.

Si el cambio es a propósito (se ha arreglado algo del VDP, por ejemplo), mira las
capturas nuevas y actualiza las referencias:

    cmake -DHEADLESS=build/plumsg-headless -DWORK=/tmp/240p -DUPDATE=ON -P tests/240p_tests.cmake
