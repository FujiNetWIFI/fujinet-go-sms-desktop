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
