# core tests (see each file's header). Network and ROM-dependent tests exit
# 77, which ctest reports as skipped.

function(sms_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sms_core)
  target_compile_definitions(${name} PRIVATE _GNU_SOURCE _DEFAULT_SOURCE
    SMS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  set_target_properties(${name} PROPERTIES C_STANDARD 11)
  if(WIN32)
    target_link_options(${name} PRIVATE -static)
  endif()
endfunction()

# A headless runner: boots a machine and writes frames as PPM (the MAME A/B
# harness's "side B", tools/ab/mame_ab.py). Built, not registered.
sms_test(sms_headless sms_headless.c)

# These need the session layer, which needs SDL3. On a build without it
# (a cross-toolchain, or a lean machine) only the core libraries build.
if(TARGET sms_session)

function(sms_session_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sms_session)
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/debugger")
  target_compile_definitions(${name} PRIVATE _GNU_SOURCE _DEFAULT_SOURCE
    SMS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  set_target_properties(${name} PROPERTIES C_STANDARD 11)
  if(WIN32)
    target_link_options(${name} PRIVATE -static)
  endif()
endfunction()

# The real in-process FujiNet: brings libfujinet up on its port, boots the
# CONFIG client and confirms the cartridge's link came up, and comes up again
# after a power cycle. SKIPs (77) when the runtime was not built
# (-DWITH_FUJINET=OFF). Serial: it owns the port.
sms_session_test(fujibus_smoke fujibus_smoke.c)
add_test(NAME fujibus_smoke COMMAND fujibus_smoke)
set_tests_properties(fujibus_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)

# A network boot end to end: the cartridge bring-up's fujiboot client mounts
# hello.sms (put there by Import Cartridge to SD) from the SD host, FujiNet
# pushes it, the loader copies it in and it runs; twice, with the mailbox on
# its worker thread and inline. Needs SMS_TESTROM_DIR (fujinet-firmware
# pico/sms/build, built with BOOT_PATH=/hello.sms); SKIPs without.
sms_session_test(netboot_test netboot_test.c)
add_test(NAME netboot COMMAND netboot_test)
set_tests_properties(netboot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)
add_test(NAME netboot_sync COMMAND netboot_test)
set_tests_properties(netboot_sync PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON
  ENVIRONMENT "SMS_CART_SYNC=1")

endif()
