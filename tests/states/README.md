# States from earlier versions

Each file is a state saved by a released version of plumsg with its `plumsg-headless`,
after frame 89 of one of its built-in test ROMs:

    plumsg-headless --test-rom --frames 90 --save-state VERSION-megadrive.state
    plumsg-headless --sms-test-rom --frames 90 --save-state VERSION-mastersystem.state
    plumsg-headless --gg-test-rom --frames 90 --save-state VERSION-gamegear.state

The `headless` test (`tests/headless_tests.cmake`) loads each one with the current
version and checks it carries on exactly like a run that never stopped. If it fails, the
states people have already saved would stop loading or load wrong.

When releasing a version, add its states (Master System and Game Gear exist since
0.0.25).
