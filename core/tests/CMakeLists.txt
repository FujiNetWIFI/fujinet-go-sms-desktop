# core tests (see each file's header). Network and ROM-dependent tests exit
# 77, which ctest reports as skipped.

function(sms_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sms_core)
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/core/debugger")
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

# The core links and runs alone: no SDL, no session (a session symbol would
# fail the link).
sms_test(no_sdl_link_test no_sdl_link_test.c)
add_test(NAME no_sdl_link COMMAND no_sdl_link_test)

# The timing probes on every model against what MAME's own Master System
# left in work RAM (probe_golden.h, from tools/ab/make_goldens.py).
sms_test(vdp_timing_test vdp_timing_test.c)
add_test(NAME vdp_timing COMMAND vdp_timing_test)

# The paced host: CONFIG paints, NTSC and PAL keep their own frame rates.
sms_test(boot_smoke boot_smoke.c)
add_test(NAME boot_smoke COMMAND boot_smoke)
set_tests_properties(boot_smoke PROPERTIES TIMEOUT 60)

# The BIOS socket, $3E and the cartridge's BIOS snoop, with BIOS images the
# test writes itself.
sms_test(bios_synth_test bios_synth_test.c)
add_test(NAME bios_synth COMMAND bios_synth_test)

# The console/cartridge seam: mapper routing, GAME vs APP, the armed flip
# only on a committed /M1 fetch of $0000, re-plan on /RESET, inert peeks.
sms_test(cart_seam_test cart_seam_test.c)
add_test(NAME cart_seam COMMAND cart_seam_test)

# The disassembler against hand-checked encodings, and the VDP views'
# decoders (both VDP kinds, mode 4 and the TMS modes) on built snapshots.
sms_test(dasm_test dasm_test.c)
add_test(NAME dasm COMMAND dasm_test)
sms_test(vdp_decode_test vdp_decode_test.c)
add_test(NAME vdp_decode COMMAND vdp_decode_test)

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

# The frontend contract, against a real temporary XDG tree: settings persist,
# paths resolve, CONFIG boots and paints, input reaches the machine, a
# cartridge opens, Soft Reset keeps it, Reset to CONFIG ejects it, an image
# the cartridge cannot map is refused with a reason, console changes.
sms_session_test(session_test session_test.c)
add_test(NAME session COMMAND session_test)
set_tests_properties(session PROPERTIES TIMEOUT 120)

# The remappable binding table and the keyboard translator, pure.
sms_session_test(bindings_test bindings_test.c)
add_test(NAME bindings COMMAND bindings_test)

# Where a dropped file goes, Import Cartridge to SD (with its .cfg), and
# Import BIOS (identified by size and CRC; no Sega firmware involved).
sms_session_test(media_test media_test.c)
add_test(NAME media COMMAND media_test)

# The gamepad layer's pure helpers, and that the SDL subsystem starts and
# stops with no hardware attached.
sms_session_test(gamepad_test gamepad_test.c)
add_test(NAME gamepad COMMAND gamepad_test)

# The debugger contract: attach stops the machine, registers, every step
# kind, disassembly, breakpoints of every class with conditions, memory,
# VRAM/CRAM, the VDP views, I/O, the cartridge tab, symbols in every format,
# the trace, Save, the prompt, and detach resumes.
sms_session_test(debug_test debug_test.c)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(debug PROPERTIES TIMEOUT 120)

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
