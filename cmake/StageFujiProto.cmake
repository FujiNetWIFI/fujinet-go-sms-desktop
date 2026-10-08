# Provide the fujinet-firmware checkout (see cmake/Dependencies.cmake) and
# stage the FujiNet SMS cartridge's own sources into core/sms/fuji-generated,
# which is what core/CMakeLists.txt compiles into the emulator core.
#
# These are the cartridge firmware's OWN sources (pico/sms on the add-sms
# branch) -- fujimail.c (hotspot decode, the REGSEL/REGDATA interlock, ACKSEQ
# published last, the DBC push receiver), fujibus.c (SLIP + FujiBus wire
# codec), smsmap.c (the mapper engine: which 8K SRAM bank sits behind each 1K
# page, and the image planner -- CRC database, .cfg override, MAME's
# heuristic), smsmap_db.c (every MAME software-list entry's board by CRC),
# fuji_load.c (the load sequence and the hand-over), sms_cart.h (the bus
# decode and the BIOS snoop), fuji_mailbox.h (the arena layout) -- plus the
# baked CONFIG client and the 2K loader page (fujiconfigrom.h,
# smsloaderrom.h) and the emulator-side SLIP-over-TCP transport (fujitcp).
# The firmware's pico/sms/emu/apply.sh grafts the identical set into a MAME
# tree; this file is that graft for this repository. Emulator and cartridge
# stay identical by construction, not by discipline: nothing here is patched,
# only copied. The one port-specific file, the cart device itself, is this
# repository's own core/sms/fujinet_cart.c (transposed from the firmware's
# MAME device, pico/sms/emu/fujinet.cpp).
#
# apply.sh additionally generates a fujins.h that prefixes every shared
# symbol, because several FujiNet carts share one MAME binary. Only one
# FujiNet cart is ever linked here, so that prefixing is deliberately NOT
# reproduced. Do not add it "for symmetry".
#
# fujitcp.c is POSIX sockets; Windows builds compile the committed Winsock
# twin core/sms/fujitcp_win32.c against the SAME staged fujitcp.h instead
# (see core/CMakeLists.txt). The staged .c is still copied everywhere so the
# staging result does not depend on the host.
#
# Staging is automatic: it runs when the staged tree is missing, when the pin
# has moved, when this file changes, or on demand with -DFUJI_RESTAGE=ON
# (which is also how to pick up uncommitted edits in a working checkout
# pointed at by FUJINET_SRC).

set(FUJI_GEN "${CMAKE_SOURCE_DIR}/core/sms/fuji-generated")

option(FUJI_RESTAGE "Re-stage the FujiNet protocol sources from the firmware checkout" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

sms_provide_dependency(
  NAME fujinet-firmware
  PATH third_party/fujinet-firmware
  URL "${FUJINET_URL}"
  COMMIT "${FUJINET_COMMIT}"
  SENTINEL build.sh
  OVERRIDE FUJINET_SRC
  RESULT FUJINET_DIR)

set(FUJI_PROTO_DIR "${FUJINET_DIR}/pico/sms")
if(NOT EXISTS "${FUJI_PROTO_DIR}/firmware/include/fuji_mailbox.h")
  message(FATAL_ERROR
    "${FUJINET_DIR} has no pico/sms/firmware/include/fuji_mailbox.h -- "
    "the pin must be on the add-sms branch (or master once it has merged); "
    "see cmake/Dependencies.cmake.")
endif()

# The exact file list apply.sh grafts into MAME. The CONFIG client and the
# loader page come from firmware/baked/ -- apply.sh prefers a fresh
# build/gen when a developer has run build-cart.sh, but a pinned checkout
# never has one, and baked/ is what the cartridge firmware itself ships.
set(FUJI_STAGE_FILES
  firmware/src/fujimail.c
  firmware/src/fujibus.c
  firmware/src/smsmap.c
  firmware/src/smsmap_db.c
  firmware/src/fuji_load.c
  firmware/include/fujimail.h
  firmware/include/fujibus.h
  firmware/include/fuji_mailbox.h
  firmware/include/sms_cart.h
  firmware/include/smsmap.h
  firmware/include/smsmap_db.h
  firmware/include/fuji_load.h
  firmware/baked/fujiconfigrom.h
  firmware/baked/smsloaderrom.h
  emu/fujitcp.c
  emu/fujitcp.h)

# What the staged tree was made from: the source identity plus a hash of this
# file (the only transform), so that editing the stage re-stages rather than
# silently leaving the old result in place.
set(_fuji_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${FUJINET_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _fuji_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _fuji_head)
  set(_fuji_head "${FUJINET_COMMIT} ${FUJINET_DIR}")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _fuji_stage_hash)
set(_fuji_want "${_fuji_head} ${_fuji_stage_hash}")

set(_fuji_staged "")
if(EXISTS "${FUJI_GEN}/.source-info")
  file(READ "${FUJI_GEN}/.source-info" _fuji_staged)
  string(STRIP "${_fuji_staged}" _fuji_staged)
endif()

if(FUJI_RESTAGE OR NOT EXISTS "${FUJI_GEN}/fuji_mailbox.h"
   OR NOT _fuji_staged STREQUAL _fuji_want)
  message(STATUS "Staging FujiNet protocol sources from ${FUJI_PROTO_DIR}")
  file(REMOVE_RECURSE "${FUJI_GEN}")
  file(MAKE_DIRECTORY "${FUJI_GEN}")

  foreach(_f IN LISTS FUJI_STAGE_FILES)
    if(NOT EXISTS "${FUJI_PROTO_DIR}/${_f}")
      file(REMOVE_RECURSE "${FUJI_GEN}")
      message(FATAL_ERROR
        "FujiNet protocol staging failed: ${FUJI_PROTO_DIR}/${_f} is missing "
        "(pin drift? see cmake/Dependencies.cmake).")
    endif()
    get_filename_component(_base "${_f}" NAME)
    file(COPY_FILE "${FUJI_PROTO_DIR}/${_f}" "${FUJI_GEN}/${_base}")
  endforeach()

  file(WRITE "${FUJI_GEN}/.source-info" "${_fuji_want}\n")
endif()

message(STATUS "FujiNet protocol sources staged from ${FUJI_PROTO_DIR}")
