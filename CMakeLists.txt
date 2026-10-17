cmake_minimum_required(VERSION 3.16)
project(plumsg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PLUMSG_FETCH_SDL2 "Download and build SDL2 if it is not installed" ON)
option(PLUMSG_BUILD_TESTS "Build the test programs" ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra)
endif()

include(FetchContent)

# miniz (vendored, MIT): zip files, savestate compression and PNG screenshots.
add_library(plumsg_miniz STATIC third_party/miniz/miniz.c)
set_target_properties(plumsg_miniz PROPERTIES LINKER_LANGUAGE C)
target_include_directories(plumsg_miniz PUBLIC third_party/miniz)
target_compile_definitions(plumsg_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
if(MSVC)
  target_compile_options(plumsg_miniz PRIVATE /W0)
else()
  target_compile_options(plumsg_miniz PRIVATE -w)
endif()

# ymfm (BSD-3-Clause, Aaron Giles) emulates the YM2612. Pinned to a known commit.
FetchContent_Declare(ymfm
  GIT_REPOSITORY https://github.com/aaronsgiles/ymfm.git
  GIT_TAG 81aec25ccbb98f4873a255f7551ac4dadac59b4a)
FetchContent_GetProperties(ymfm)
if(NOT ymfm_POPULATED)
  FetchContent_Populate(ymfm)
endif()
add_library(plumsg_ymfm STATIC
  ${ymfm_SOURCE_DIR}/src/ymfm_opn.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_adpcm.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_ssg.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_misc.cpp
)
target_include_directories(plumsg_ymfm SYSTEM PUBLIC ${ymfm_SOURCE_DIR}/src)
if(MSVC)
  target_compile_options(plumsg_ymfm PRIVATE /W0)
else()
  target_compile_options(plumsg_ymfm PRIVATE -w)
endif()

# Emulator core: no SDL, no OS dependencies.
add_library(plumsg_core STATIC
  src/core/m68k.cpp
  src/core/z80.cpp
  src/core/vdp_md.cpp
  src/core/cartridge.cpp
  src/core/io.cpp
  src/core/psg.cpp
  src/core/ym2612.cpp
  src/core/audio_mixer.cpp
  src/core/megadrive.cpp
  src/render/planes.cpp
)
target_include_directories(plumsg_core PUBLIC src src/core)
target_link_libraries(plumsg_core PUBLIC plumsg_miniz PRIVATE plumsg_ymfm)

# SDL2: use an installed copy (vcpkg, system package, SDL2_DIR) or fetch it.
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND AND PLUMSG_FETCH_SDL2)
  include(FetchContent)
  message(STATUS "SDL2 not found, fetching it")
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.9
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL2)
endif()

if(TARGET SDL2::SDL2)
  set(PLUMSG_SDL2_TARGET SDL2::SDL2)
else()
  set(PLUMSG_SDL2_TARGET SDL2::SDL2-static)
endif()

# Dear ImGui for the menus and option windows, drawn with OpenGL 3.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(plumsg_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(plumsg_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(plumsg_imgui PUBLIC ${PLUMSG_SDL2_TARGET})

add_executable(plumsg WIN32
  src/frontend/main.cpp
  src/frontend/input.cpp
  src/frontend/audio.cpp
  src/frontend/settings.cpp
  src/frontend/file_dialog.cpp
  src/frontend/ui.cpp
  src/render/gl.cpp
  src/render/gl_renderers.cpp
)
target_link_libraries(plumsg PRIVATE plumsg_core plumsg_imgui)
if(TARGET SDL2::SDL2main)
  target_link_libraries(plumsg PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(plumsg PRIVATE ${PLUMSG_SDL2_TARGET})
if(WIN32)
  target_link_libraries(plumsg PRIVATE comdlg32)
endif()

# Copy SDL2.dll next to plumsg.exe so it runs straight from the build folder.
if(WIN32 AND TARGET SDL2::SDL2)
  add_custom_command(TARGET plumsg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:plumsg>)
endif()

if(PLUMSG_BUILD_TESTS)
  enable_testing()
  add_executable(core_tests tests/core_tests.cpp)
  target_include_directories(core_tests PRIVATE tests)
  target_link_libraries(core_tests PRIVATE plumsg_core)
  add_test(NAME core_tests COMMAND core_tests)

  # Runs a ROM (or the built-in test ROM) without a window and writes a PNG.
  add_executable(rom_runner tests/rom_runner.cpp)
  target_include_directories(rom_runner PRIVATE tests)
  target_link_libraries(rom_runner PRIVATE plumsg_core)
  add_test(NAME rom_runner_test_rom COMMAND rom_runner --test-rom --frames 60)

  # Tom Harte's 68000 single-step tests: m68k_json_runner path/to/v1/*.json.gz
  add_executable(m68k_json_runner tests/m68k_json_runner.cpp)
  target_link_libraries(m68k_json_runner PRIVATE plumsg_core)

  # Runs the ZEXDOC/ZEXALL Z80 exercisers: zex_runner path/to/zexdoc.com
  add_executable(zex_runner tests/zex_runner.cpp)
  target_link_libraries(zex_runner PRIVATE plumsg_core)
endif()
