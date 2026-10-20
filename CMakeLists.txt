cmake_minimum_required(VERSION 3.16)
project(plumsg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PLUMSG_FETCH_SDL2 "Download and build SDL2 if it is not installed" ON)
option(PLUMSG_BUILD_TESTS "Build the test programs" ON)
# Off builds only the core, plumsg-headless and the tests: no SDL2, ImGui or OpenGL.
option(PLUMSG_BUILD_FRONTEND "Build the emulator with its window (needs SDL2 and OpenGL)" ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
  # The CRT's "use the _s variant" deprecation notices (sscanf, fopen, localtime); the calls are bounded
  # and single-threaded, and the _s variants do not exist outside MSVC.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
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
  src/core/ymfm_opn_all.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_adpcm.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_ssg.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_misc.cpp
  src/core/ymfm_opl_all.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_pcm.cpp
)
target_include_directories(plumsg_ymfm SYSTEM PUBLIC ${ymfm_SOURCE_DIR}/src)
if(MSVC)
  target_compile_options(plumsg_ymfm PRIVATE /W0)
else()
  target_compile_options(plumsg_ymfm PRIVATE -w)
endif()

# libchdr (BSD-3-Clause) reads Mega CD discs compressed as .chd. Pinned to a known commit.
# It brings its own copy of miniz for decompressing; its symbols are renamed so they
# do not clash with the miniz above.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libchdr
  GIT_REPOSITORY https://github.com/rtissera/libchdr.git
  GIT_TAG 47d794151b2b64f6aba60304e6480f4f5a2bc24b)
FetchContent_GetProperties(libchdr)
if(NOT libchdr_POPULATED)
  FetchContent_Populate(libchdr)
  add_subdirectory(${libchdr_SOURCE_DIR} ${libchdr_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
foreach(sym miniz_def_alloc_func miniz_def_free_func miniz_def_realloc_func mz_adler32 mz_crc32 mz_error mz_free
    mz_inflate mz_inflateEnd mz_inflateInit mz_inflateInit2 mz_inflateReset mz_uncompress mz_uncompress2 mz_version
    tinfl_decompress tinfl_decompress_mem_to_callback tinfl_decompress_mem_to_heap tinfl_decompress_mem_to_mem
    tinfl_decompressor_alloc tinfl_decompressor_free)
  target_compile_definitions(miniz PRIVATE ${sym}=chdr_${sym})
  target_compile_definitions(chdr-static PRIVATE ${sym}=chdr_${sym})
endforeach()
foreach(t chdr-static miniz chdr-lzma zstd)
  if(MSVC)
    target_compile_options(${t} PRIVATE /W0)
  else()
    target_compile_options(${t} PRIVATE -w)
  endif()
endforeach()

# rcheevos (MIT, RetroAchievements) for the achievements: its client, runtime and web
# API, without its own hashing (plumsg hashes the games itself). Pinned to v12.5.0.
FetchContent_Declare(rcheevos
  GIT_REPOSITORY https://github.com/RetroAchievements/rcheevos.git
  GIT_TAG 1433173220a7eaede6a9ed7a18e94117be1821e0)
FetchContent_GetProperties(rcheevos)
if(NOT rcheevos_POPULATED)
  FetchContent_Populate(rcheevos)
endif()
file(GLOB PLUMSG_RCHEEVOS_SOURCES ${rcheevos_SOURCE_DIR}/src/rapi/*.c ${rcheevos_SOURCE_DIR}/src/rcheevos/*.c)
add_library(plumsg_rcheevos STATIC ${PLUMSG_RCHEEVOS_SOURCES}
  ${rcheevos_SOURCE_DIR}/src/rc_client.c
  ${rcheevos_SOURCE_DIR}/src/rc_compat.c
  ${rcheevos_SOURCE_DIR}/src/rc_util.c
  ${rcheevos_SOURCE_DIR}/src/rc_version.c
  ${rcheevos_SOURCE_DIR}/src/rhash/md5.c)
target_include_directories(plumsg_rcheevos SYSTEM PUBLIC ${rcheevos_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
target_link_libraries(plumsg_rcheevos PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(plumsg_rcheevos PRIVATE /W0)
else()
  target_compile_options(plumsg_rcheevos PRIVATE -w)
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
  src/core/ym2413.cpp
  src/core/audio_mixer.cpp
  src/core/roland.cpp
  src/core/vgm.cpp
  src/core/song_extract.cpp
  src/core/megadrive.cpp
  src/core/rewind.cpp
  src/core/cheats.cpp
  src/core/sms.cpp
  src/core/vdp_sms.cpp
  src/core/system.cpp
  src/core/sh2.cpp
  src/core/mars.cpp
  src/core/cd_image.cpp
  src/core/megacd.cpp
  src/core/netplay.cpp
  src/core/i18n.cpp
  src/core/recording.cpp
  src/render/planes.cpp
  src/render/profile3d.cpp
)
# stb_image_write (public domain / MIT, vendored) encodes the JPEG frames of the AVI recordings.
target_include_directories(plumsg_core PUBLIC src src/core PRIVATE third_party/tsf third_party/stb)
target_link_libraries(plumsg_core PUBLIC plumsg_miniz Threads::Threads PRIVATE plumsg_ymfm chdr-static)

# UDP sockets for netplay (the session itself is in the core).
add_library(plumsg_net STATIC src/net/udp.cpp)
target_link_libraries(plumsg_net PUBLIC plumsg_core)
if(WIN32)
  target_link_libraries(plumsg_net PUBLIC ws2_32)
endif()

# RetroAchievements: the game's hash, its memory as the achievements see it, and the
# rcheevos client with its requests made on worker threads.
add_library(plumsg_achievements STATIC src/frontend/achievements.cpp)
target_link_libraries(plumsg_achievements PUBLIC plumsg_core plumsg_rcheevos)
target_include_directories(plumsg_achievements PRIVATE ${rcheevos_SOURCE_DIR}/src/rhash)

# The emulator without a window, for tests, CI and reproducing bugs (see --help).
add_executable(plumsg-headless src/headless/main.cpp)
target_include_directories(plumsg-headless PRIVATE tests)
target_link_libraries(plumsg-headless PRIVATE plumsg_core plumsg_net)

# Everything below up to endif() is the windowed emulator.
if(PLUMSG_BUILD_FRONTEND)
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
  src/frontend/game_options.cpp
  src/frontend/instrument_editor.cpp
  src/frontend/file_dialog.cpp
  src/frontend/ui.cpp
  src/frontend/library.cpp
  src/frontend/library_names.cpp
  src/frontend/game_info.cpp
  src/frontend/game_info_data.cpp
  src/frontend/http.cpp
  src/frontend/cheat_database.cpp
  src/frontend/languages.cpp
  src/frontend/achievements_ui.cpp
  src/render/gl.cpp
  src/render/gl_renderers.cpp
  src/render/upscale.cpp
)
# The plum icon of plumsg.exe (Explorer, taskbar, shortcuts).
if(WIN32)
  target_sources(plumsg PRIVATE src/frontend/plumsg.rc)
endif()
# stb_image (public domain / MIT, vendored) decodes the library's PNG and JPEG covers.
target_include_directories(plumsg PRIVATE third_party/stb)

# The PLUMSG logo and the plum icon, built into the executable as byte arrays
# (logo_png.h, icon_png.h).
function(plumsg_embed_png name var)
  set(png ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/assets/${name}.png)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${png})
  file(READ ${png} hex HEX)
  string(REPEAT "[0-9a-f][0-9a-f]" 32 line)
  string(REGEX REPLACE "(${line})" "\\1\n" hex "${hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  set(out ${CMAKE_CURRENT_BINARY_DIR}/generated/${name}_png.h)
  file(WRITE ${out}.tmp
    "// Generated by CMakeLists.txt from src/frontend/assets/${name}.png.\n#pragma once\n"
    "static const unsigned char ${var}[] = {\n${hex}\n};\n")
  configure_file(${out}.tmp ${out} COPYONLY)
endfunction()
plumsg_embed_png(logo kLogoPng)
plumsg_embed_png(icon kIconPng)

# The translations (src/frontend/lang/*.po), built in the same way (lang_po.h), so a
# new language needs no file next to the executable or the installer.
set(PLUMSG_LANGUAGES en fr pt)
set(lang_header "// Generated by CMakeLists.txt from src/frontend/lang/*.po.\n#pragma once\n")
foreach(lang ${PLUMSG_LANGUAGES})
  set(po ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/lang/${lang}.po)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${po})
  file(READ ${po} hex HEX)
  string(REPEAT "[0-9a-f][0-9a-f]" 32 line)
  string(REGEX REPLACE "(${line})" "\\1\n" hex "${hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  string(APPEND lang_header "static const unsigned char kLangPo_${lang}[] = {\n${hex}\n};\n")
endforeach()
set(out ${CMAKE_CURRENT_BINARY_DIR}/generated/lang_po.h)
file(WRITE ${out}.tmp "${lang_header}")
configure_file(${out}.tmp ${out} COPYONLY)
target_include_directories(plumsg PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
find_package(Threads REQUIRED)
target_link_libraries(plumsg PRIVATE plumsg_core plumsg_net plumsg_achievements plumsg_imgui Threads::Threads)
if(TARGET SDL2::SDL2main)
  target_link_libraries(plumsg PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(plumsg PRIVATE ${PLUMSG_SDL2_TARGET})
if(WIN32)
  target_link_libraries(plumsg PRIVATE comdlg32 wininet)
endif()

# Copy SDL2.dll next to plumsg.exe so it runs straight from the build folder.
if(WIN32 AND TARGET SDL2::SDL2)
  add_custom_command(TARGET plumsg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:plumsg>)
endif()
endif()  # PLUMSG_BUILD_FRONTEND

if(PLUMSG_BUILD_TESTS)
  enable_testing()
  add_executable(core_tests tests/core_tests.cpp src/frontend/library_names.cpp src/frontend/game_info_data.cpp)
  target_include_directories(core_tests PRIVATE tests)
  target_compile_definitions(core_tests PRIVATE PLUMSG_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(core_tests PRIVATE plumsg_core plumsg_achievements)
  add_test(NAME core_tests COMMAND core_tests)

  # Every text in tr() has its translation in every .po (tools/i18n.py --update adds the new ones).
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME i18n COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/i18n.py --check)
  endif()

  # Runs a ROM (or the built-in test ROM) without a window and writes a PNG.
  add_executable(rom_runner tests/rom_runner.cpp)
  target_include_directories(rom_runner PRIVATE tests)
  target_link_libraries(rom_runner PRIVATE plumsg_core)
  add_test(NAME rom_runner_test_rom COMMAND rom_runner --test-rom --frames 60)

  # plumsg-headless end to end: hashes, savestates, input scripts and checks.
  add_test(NAME headless COMMAND ${CMAKE_COMMAND} -DHEADLESS=$<TARGET_FILE:plumsg-headless>
           -DWORK=${CMAKE_CURRENT_BINARY_DIR}/headless_test -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_tests.cmake)

  # Tom Harte's 68000 single-step tests: m68k_json_runner path/to/v1/*.json.gz
  add_executable(m68k_json_runner tests/m68k_json_runner.cpp)
  target_link_libraries(m68k_json_runner PRIVATE plumsg_core)

  # Runs the ZEXDOC/ZEXALL Z80 exercisers: zex_runner path/to/zexdoc.com
  add_executable(zex_runner tests/zex_runner.cpp)
  target_link_libraries(zex_runner PRIVATE plumsg_core)

  # The frontend's files (a game's own options and controls); needs SDL2 for the key names.
  if(PLUMSG_BUILD_FRONTEND)
    add_executable(frontend_tests tests/frontend_tests.cpp src/frontend/game_options.cpp src/frontend/input.cpp
                   src/frontend/settings.cpp)
    target_link_libraries(frontend_tests PRIVATE plumsg_core ${PLUMSG_SDL2_TARGET})
    add_test(NAME frontend_tests COMMAND frontend_tests)
  endif()
endif()
