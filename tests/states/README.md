# Estados de versiones anteriores

Cada archivo es un estado guardado por una versión publicada de plumsg con su
`plumsg-headless`, tras el fotograma 89 de una de las ROM de prueba que trae:

    plumsg-headless --test-rom --frames 90 --save-state VERSION-megadrive.state
    plumsg-headless --sms-test-rom --frames 90 --save-state VERSION-mastersystem.state
    plumsg-headless --gg-test-rom --frames 90 --save-state VERSION-gamegear.state

La prueba `headless` (`tests/headless_tests.cmake`) carga cada uno con la versión de
ahora y comprueba que sigue igual que una partida que nunca se paró. Si falla, los
estados que la gente ya tiene guardados dejarían de cargar o cargarían mal.

Al publicar una versión, añade los suyos (Master System y Game Gear existen desde la
0.0.25).
