# Runs Artemio Urbina's 240p Test Suite (tests/240p) and checks each screen it draws;
# run by ctest as
#   cmake -DHEADLESS=path/to/plumsg-headless -DWORK=scratch/dir -P 240p_tests.cmake
# Each tests/240p/guiones/NAME.txt walks the suite's menus from power on to one screen
# and quits there; the last frame's hash must be the one in referencias.txt. When one
# differs, WORK/NAME.png is what plumsg drew and tests/240p/capturas/NAME.png what it
# drew before.
#
# After a change that draws a screen differently on purpose, -DUPDATE=ON rewrites
# referencias.txt and the captures (look at them before committing).

if(NOT HEADLESS OR NOT WORK)
  message(FATAL_ERROR "HEADLESS and WORK must be set")
endif()
set(DIR "${CMAKE_CURRENT_LIST_DIR}/240p")
set(ROM "${DIR}/240pSuite-1.32.bin")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(lines "")
if(EXISTS "${DIR}/referencias.txt")
  file(STRINGS "${DIR}/referencias.txt" lines REGEX "^[^#]")
endif()
foreach(line ${lines})
  if(line MATCHES "^([^ ]+) +([0-9a-f]+)$")
    set("hash_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
  endif()
endforeach()

file(GLOB scripts "${DIR}/guiones/*.txt")
set(failed "")
set(updated "# The hash of the last frame of each guiones/NAME.txt (240p_tests.cmake -DUPDATE=ON writes it)\n")
foreach(script ${scripts})
  get_filename_component(name "${script}" NAME_WE)
  execute_process(COMMAND "${HEADLESS}" "${ROM}" --frames 10000 --input "${script}" --png "${WORK}/${name}.png" --quiet
                          --json "${WORK}/${name}.json"
                  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "${name}: plumsg-headless exited with ${code}\n${out}${err}")
  endif()
  file(READ "${WORK}/${name}.json" json)
  string(REGEX MATCH "\"hash\": \"([0-9a-f]+)\"" ok "${json}")
  set(hash "${CMAKE_MATCH_1}")
  string(APPEND updated "${name} ${hash}\n")
  if(UPDATE)
    configure_file("${WORK}/${name}.png" "${DIR}/capturas/${name}.png" COPYONLY)
  elseif(NOT DEFINED "hash_${name}")
    list(APPEND failed "${name}: no hash in referencias.txt (run with -DUPDATE=ON)")
  elseif(NOT hash STREQUAL "${hash_${name}}")
    list(APPEND failed "${name}: hash ${hash}, expected ${hash_${name}}; compare ${WORK}/${name}.png with ${DIR}/capturas/${name}.png")
  endif()
endforeach()

if(UPDATE)
  file(WRITE "${DIR}/referencias.txt" "${updated}")
  message(STATUS "updated ${DIR}/referencias.txt and capturas/")
elseif(failed)
  list(JOIN failed "\n" text)
  message(FATAL_ERROR "240p Test Suite screens that changed:\n${text}")
endif()
