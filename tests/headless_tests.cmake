# Checks plumsg-headless end to end; run by ctest as
#   cmake -DHEADLESS=path/to/plumsg-headless -DWORK=scratch/dir -P headless_tests.cmake
# Each check runs the program like a user would and looks at what it wrote.

if(NOT HEADLESS OR NOT WORK)
  message(FATAL_ERROR "HEADLESS and WORK must be set")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(run expect_code)
  execute_process(COMMAND "${HEADLESS}" ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL expect_code)
    message(FATAL_ERROR "plumsg-headless ${ARGN}\nexit code ${code}, expected ${expect_code}\n${out}${err}")
  endif()
  set(output "${out}${err}" PARENT_SCOPE)
endfunction()

# The test ROM draws the same picture on every build: a change here means the VDP
# draws differently (on purpose or not; if on purpose, update the hash).
run(0 --test-rom --frames 120 --expect-hash d171be7e --quiet)
run(3 --test-rom --frames 120 --expect-hash 00000000 --quiet)
# The same for the Master System test ROM, and its savestates.
run(0 --sms-test-rom --frames 120 --expect-hash 4220de49 --quiet)
run(0 --gg-test-rom --frames 120 --expect-hash 72c5cc29 --quiet)
run(0 --sms-test-rom --frames 120 --hash-log "${WORK}/sms-log.txt" --quiet)
run(0 --sms-test-rom --frames 120 --expect-log "${WORK}/sms-log.txt" --quiet
    --do "59 savestate ${WORK}/sms.state" --do "60 loadstate ${WORK}/sms.state")

# Two runs give the same frames and sound, and a savestate taken after frame 59 and
# loaded back before frame 60 changes nothing.
run(0 --test-rom --frames 180 --hash-log "${WORK}/log.txt" --png-every 60 --out-dir "${WORK}" --quiet)
run(0 --test-rom --frames 180 --expect-log "${WORK}/log.txt" --quiet)
run(0 --test-rom --frames 180 --expect-log "${WORK}/log.txt" --quiet
    --do "59 savestate ${WORK}/s.state" --do "60 loadstate ${WORK}/s.state")
foreach(f 000000 000060 000120)
  if(NOT EXISTS "${WORK}/frame_${f}.png")
    message(FATAL_ERROR "--png-every did not write frame_${f}.png")
  endif()
endforeach()
# --gif and --avi record the run: a GIF89a and an AVI with its picture and sound.
run(0 --test-rom --frames 120 --gif "${WORK}/run.gif" --avi "${WORK}/run.avi" --quiet)
file(READ "${WORK}/run.gif" head LIMIT 6 HEX)
if(NOT head STREQUAL "474946383961")
  message(FATAL_ERROR "--gif did not write a GIF89a: ${head}")
endif()
file(READ "${WORK}/run.avi" head LIMIT 12 HEX)
if(NOT head MATCHES "^52494646........41564920$")
  message(FATAL_ERROR "--avi did not write an AVI: ${head}")
endif()
file(SIZE "${WORK}/run.avi" size)
if(size LESS 100000)
  message(FATAL_ERROR "--avi wrote only ${size} bytes")
endif()

# A run that goes different is caught on its first different frame: a reset before
# frame 100 changes nothing before it.
run(3 --test-rom --frames 180 --expect-log "${WORK}/log.txt" --do "100 reset")
if(NOT output MATCHES "FAILED: frame 1[0-9][0-9]:")
  message(FATAL_ERROR "the divergence was not reported at frame 100 or later:\n${output}")
endif()

# The same savestate, loaded by a new process, carries on where it was saved.
run(0 --test-rom --load-state "${WORK}/s.state" --frames 120 --hash-log "${WORK}/resumed.txt" --quiet)
file(STRINGS "${WORK}/log.txt" full REGEX "^119 ")
file(STRINGS "${WORK}/resumed.txt" resumed REGEX "^59 ")
string(REGEX REPLACE "^[0-9]+ " "" full "${full}")
string(REGEX REPLACE "^[0-9]+ " "" resumed "${resumed}")
if(NOT full STREQUAL resumed)
  message(FATAL_ERROR "a savestate loaded with --load-state did not continue the run: '${full}' vs '${resumed}'")
endif()

# Input scripts reach the game: the pad test ROM keeps what it reads from pad 1 at
# 0xFF0000 (..CBRLDU, 0 = pressed) and 0xFF0001 (..SA00DU).
file(WRITE "${WORK}/input.txt" "# pad test\n10+5 start\n20 right b\n20 dump-ram ${WORK}/ram20.bin\n30 dump-ram ${WORK}/ram30.bin\n")
run(0 --pad-test-rom --frames 40 --input "${WORK}/input.txt" --do "12 dump-ram ${WORK}/ram12.bin" --quiet)
foreach(check "12;7f13" "20;6733" "30;7f33")
  list(GET check 0 frame)
  list(GET check 1 want)
  file(READ "${WORK}/ram${frame}.bin" got LIMIT 2 HEX)
  if(NOT got STREQUAL want)
    message(FATAL_ERROR "pad bytes on frame ${frame}: ${got}, expected ${want}")
  endif()
endforeach()

# Bad scripts and options are refused with exit code 2.
run(2 --test-rom --do "10 jump")
run(2 --test-rom --do "ten start")
run(2 --test-rom --frames)
run(2 --test-rom --no-such-option)

# Netplay over UDP on this machine: a host and a guest, each with its own player's
# script, see the same frames as one console fed both scripts.
file(WRITE "${WORK}/p1.txt" "10-40 right\n30+20 a\n100-150 up b\n")
file(WRITE "${WORK}/p2.txt" "5-25 p2.left\n60-90 p2.c p2.start\n120-200 p2.down\n")
execute_process(
  COMMAND "${HEADLESS}" --two-pad-test-rom --frames 200 --netplay-host 47845 --input "${WORK}/p1.txt"
          --hash-log "${WORK}/net_host.txt" --quiet
  COMMAND "${HEADLESS}" --two-pad-test-rom --frames 200 --netplay-join 127.0.0.1:47845 --input "${WORK}/p2.txt"
          --hash-log "${WORK}/net_guest.txt" --quiet
  RESULTS_VARIABLE codes OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT codes STREQUAL "0;0")
  message(FATAL_ERROR "netplay host and guest ended with ${codes}\n${out}${err}")
endif()
file(READ "${WORK}/p1.txt" p1)
file(READ "${WORK}/p2.txt" p2)
file(WRITE "${WORK}/both.txt" "${p1}${p2}")
run(0 --two-pad-test-rom --frames 200 --input "${WORK}/both.txt" --expect-log "${WORK}/net_host.txt" --quiet)
run(0 --two-pad-test-rom --frames 200 --input "${WORK}/both.txt" --expect-log "${WORK}/net_guest.txt" --quiet)
# And the log really depends on pad 2: without its script it goes different.
run(3 --two-pad-test-rom --frames 200 --input "${WORK}/p1.txt" --expect-log "${WORK}/net_guest.txt" --quiet)
# Only buttons can be scripted in netplay.
run(2 --two-pad-test-rom --netplay-host 47846 --do "10 reset")
