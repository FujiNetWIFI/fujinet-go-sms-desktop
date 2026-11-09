# sms_session: the frontend contract (core/include/smssession.h), included
# from core/CMakeLists.txt once SDL3 is found.

set(SMS_SESSION_SOURCES
  src/session.c
  src/settings.c
  src/paths.c
  src/media.c
  src/roms.c
  src/fujinet_runtime.c
  src/audio_sdl.c
  src/bindings.c
  src/controls.c
  src/input.c
  src/hid_keys.c
  src/gamepad_sdl.c
  src/debug.c
  src/help.c)

# The embedded ROM table: empty unless WITH_SMS_ROMS (see COMPLIANCE.md).
set(SMS_ROMS_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(WITH_SMS_ROMS)
  file(GLOB SMS_ROM_INPUTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/*")
  add_custom_command(
    OUTPUT "${SMS_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            "${SMS_ROMS_C}" "${CMAKE_SOURCE_DIR}/tools/roms"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py" ${SMS_ROM_INPUTS}
    COMMENT "Embedding the BIOS images in tools/roms (WITH_SMS_ROMS=ON: NOT redistributable)"
    VERBATIM)
else()
  add_custom_command(
    OUTPUT "${SMS_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            --none "${SMS_ROMS_C}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
    VERBATIM)
endif()

# The Help, embedded: data/help/help.md as topics of styled spans
# (core/include/smshelp.h), which every frontend lays out with its own
# toolkit. help_data.h declares the table; the build generates it.
set(SMS_HELP_C "${CMAKE_BINARY_DIR}/generated/help_content.c")
add_custom_command(
  OUTPUT "${SMS_HELP_C}"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/help/make-help.py"
          --src "${CMAKE_SOURCE_DIR}/data/help/help.md"
          --out-c "${SMS_HELP_C}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/help/make-help.py"
          "${CMAKE_SOURCE_DIR}/data/help/help.md"
  COMMENT "Help"
  VERBATIM)

add_library(sms_session STATIC ${SMS_SESSION_SOURCES} "${SMS_ROMS_C}" "${SMS_HELP_C}")
set_target_properties(sms_session PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(sms_session PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(sms_session PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/debugger")
target_link_libraries(sms_session PUBLIC sms_core SDL3::SDL3)
# setenv()/getaddrinfo() under a strict -std=c11
target_compile_definitions(sms_session PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)
set_property(SOURCE ${SMS_SESSION_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
  "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>"
  # paths are bounded by SMS_PATH_MAX and joined with snprintf on purpose;
  # GCC's truncation analysis flags every such join
  "$<$<C_COMPILER_ID:GNU>:-Wno-format-truncation>")

# Where an uninstalled build and an installed copy find the FujiNet runtime
# (core/src/paths.c probes these after the executable's own directory).
target_compile_definitions(sms_session PRIVATE
  SMS_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  SMS_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-sms"
  SMS_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-sms")
if(WIN32)
  target_link_libraries(sms_session PUBLIC ws2_32)
else()
  target_link_libraries(sms_session PUBLIC ${CMAKE_DL_LIBS})
endif()
