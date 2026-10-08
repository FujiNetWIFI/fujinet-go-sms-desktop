# sms_core: the toolkit-agnostic, SDL-free Master System core.
#
#   sms/                the emulator: MAME's Master System, VDP and PSG
#                       transposed to C (machine.c, vdp.c, psg.c) around
#                       floooh's cycle-stepped z80.h, MAME's own YM2413 (ymfm,
#                       vendored, behind fm_ym2413.cpp), the paced host
#                       thread, and the FujiNet cartridge device.
#
#   sms/fuji-generated/ the cartridge firmware's OWN sources -- fujimail.c
#                       (the mailbox), fujibus.c (the SLIP + FujiBus wire
#                       codec), smsmap.c / smsmap_db.c (the mapper engine and
#                       its CRC database), fuji_load.c (the load sequence),
#                       sms_cart.h (the bus decode and BIOS snoop) and the
#                       baked CONFIG client and loader page -- staged by
#                       cmake/StageFujiProto.cmake. Compiling these rather
#                       than reimplementing them is what makes the emulated
#                       cartridge and the real RP2354B one identical by
#                       construction.
#
#   debugger/           the debugger engine (core/include/smsdebug.h).
#
# The one port-specific cartridge file is sms/fujinet_cart.c, transposed
# from the firmware's MAME device (pico/sms/emu/fujinet.cpp). The transport
# is sms/fujitcp_host.c, this app's own portable twin of the staged
# fujitcp.c (see its header).
#
# Nothing here links SDL. The session layer (SDL3 audio + gamepad) is a
# separate target so that stays true; no_sdl_link guards it.

set(FUJI_GEN "${CMAKE_SOURCE_DIR}/core/sms/fuji-generated")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(NOT EXISTS "${FUJI_GEN}/fuji_mailbox.h")
  message(FATAL_ERROR
    "Staged FujiNet sources are missing. cmake/StageFujiProto.cmake should "
    "have produced them; re-run with -DFUJI_RESTAGE=ON.")
endif()

# The staged cartridge sources (the staged fujitcp.c is not compiled; see
# sms/fujitcp_host.c).
set(SMS_FUJI_SOURCES
  ${FUJI_GEN}/fujimail.c
  ${FUJI_GEN}/fujibus.c
  ${FUJI_GEN}/smsmap.c
  ${FUJI_GEN}/smsmap_db.c
  ${FUJI_GEN}/fuji_load.c)

# ymfm, vendored verbatim from MAME's 3rdparty tree: only the OPL family
# (ymfm_opl.cpp) is needed for the YM2413, plus the two files it includes.
set(SMS_YMFM_SOURCES
  sms/ymfm/ymfm_opl.cpp
  sms/ymfm/ymfm_adpcm.cpp
  sms/ymfm/ymfm_pcm.cpp)

set(SMS_EMU_SOURCES
  sms/machine.c
  sms/vdp.c
  sms/psg.c
  sms/fm_ym2413.cpp
  sms/fujinet_cart.c
  sms/fujitcp_host.c
  sms/host.c)

set(SMS_DEBUGGER_SOURCES)
foreach(_f debugger.c expr.c prompt.c z80dasm.c symbols.c symbols_builtin.c vdpview.c cartview.c)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/debugger/${_f}")
    list(APPEND SMS_DEBUGGER_SOURCES debugger/${_f})
  endif()
endforeach()

add_library(sms_core STATIC
  ${SMS_EMU_SOURCES} ${SMS_FUJI_SOURCES} ${SMS_YMFM_SOURCES} ${SMS_DEBUGGER_SOURCES})
set_target_properties(sms_core PROPERTIES
  C_STANDARD 11
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(sms_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/sms"
  # public because the core's own headers include the staged fuji_mailbox.h
  # and smsmap.h (the debugger and the tests read the cart's state)
  "${FUJI_GEN}")
target_include_directories(sms_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/debugger")

# getaddrinfo/clock_gettime/nanosleep under a strict -std=c11.
target_compile_definitions(sms_core PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)

target_link_libraries(sms_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(sms_core PUBLIC ws2_32)
else()
  target_link_libraries(sms_core PUBLIC m)
endif()

# smsmap.h fills its uint8_t page table through a uint32_t pointer
# (smsmap_fill): fine on the RP2354B, strict-aliasing territory for an
# optimising host compiler. Every unit that includes it gets the flag.
set(SMS_SMSMAP_USERS
  ${FUJI_GEN}/smsmap.c ${FUJI_GEN}/smsmap_db.c ${FUJI_GEN}/fuji_load.c
  sms/fujinet_cart.c ${SMS_DEBUGGER_SOURCES})
set_property(SOURCE ${SMS_SMSMAP_USERS} APPEND PROPERTY COMPILE_OPTIONS
  "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-fno-strict-aliasing>")

# Our own modules are warning-clean; the staged and vendored trees are built
# with whatever warnings upstream is comfortable with.
set(SMS_WARN_SOURCES sms/machine.c sms/vdp.c sms/psg.c sms/fm_ym2413.cpp
  sms/fujinet_cart.c sms/fujitcp_host.c sms/host.c ${SMS_DEBUGGER_SOURCES})
list(REMOVE_ITEM SMS_WARN_SOURCES debugger/symbols_builtin.c)
set_property(SOURCE ${SMS_WARN_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
  "$<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wall;-Wextra>"
  "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall;-Wextra>")
set_property(SOURCE ${SMS_YMFM_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
  "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>")
# the shim includes ymfm's headers, whose virtual defaults leave parameters
# unused
set_property(SOURCE sms/fm_ym2413.cpp APPEND PROPERTY COMPILE_OPTIONS
  "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-unused-parameter>")

# ---- sms_session: the frontend contract ----------------------------------
#
# A separate target from sms_core so the core stays SDL-free: audio_sdl.c and
# gamepad_sdl.c live here. Found-or-skipped, exactly as the frontends are:
# `-DFRONTEND=none` on a machine (or a cross-toolchain) with no SDL3 must
# still configure and build the emulator and its tests.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/session.c")
  find_package(SDL3 CONFIG QUIET)
  if(NOT SDL3_FOUND)
    message(STATUS "sms_session: SDL3 not found -- building the core and its "
                   "tests only (no audio, no gamepads, no frontends)")
  else()
    include(session.cmake)
  endif()
endif()

add_subdirectory(tests)
