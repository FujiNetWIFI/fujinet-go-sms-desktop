cmake_minimum_required(VERSION 3.20)

# Must be set before project() to take effect. Left unset, Clang defaults the
# bundle's minimum macOS version to whatever SDK the CI runner's Xcode ships
# -- e.g. the macos-latest runner's SDK can be newer than a real tester's OS,
# which produced "You can't use this version of the application ... with this
# version of macOS" on an M1 Mac for the sibling MSX port. 13.3 matches the
# NES and Atari 2600 siblings (ymfm, the YM2413 core, is C++17).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")
endif()

# The version lives here and nowhere else: the About boxes, the macOS bundle
# and the release workflow all take it from PROJECT_VERSION, and cutting a tag
# checks that the tag agrees with it (see "Cutting a release" in the README).
project(fujinet-go-sms-desktop VERSION 0.1.0 LANGUAGES C CXX)

add_compile_definitions(SMS_VERSION_STRING="${PROJECT_VERSION}")

# The core, the session layer and the GTK/Win32 frontends are C11; ymfm (the
# YM2413, vendored from MAME) and the Qt frontend are C++17.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Which native frontend(s) to build. Each is found-or-skipped so a lean
# machine can still build the others (or just the core + tests).
set(FRONTEND "all" CACHE STRING
    "Frontend to build: gnome, kde, macos, windows, all, none")

# The Master System BIOSes and the YM2413's internal patch set are
# copyrighted Sega/Yamaha firmware and are NOT freely licensed, so this
# repository does not redistribute them -- and the machine never needs them:
# every model boots the FujiNet cartridge directly, as MAME's "none" BIOS
# does, and the YM2413 falls back to ymfm's own patch table.
#
#   OFF (default) -- the app reads any BIOS / patch ROM the user imported
#                    ("Import BIOS...") from the ROM directory at run time.
#                    Every artifact this repository publishes is built this
#                    way, and core/tests/no_embedded_roms.py checks that claim
#                    against the shipped binaries.
#   ON            -- embed whatever recognised images are in tools/roms/ so a
#                    development build has them from the first start. NOT for
#                    redistribution.
#
# See COMPLIANCE.md.
option(WITH_SMS_ROMS "Embed the Master System BIOSes found in tools/roms (see COMPLIANCE.md)" OFF)

# Included here, at the top of the tree, so every subdirectory sees the
# *variables* it defines: GNUInstallDirs caches CMAKE_INSTALL_DATADIR as an
# empty string and only sets its non-cache fallback (share) in the including
# scope. Including it in a subdirectory (as core/ would) therefore leaves the
# frontends with an empty DATADIR -- and "${CMAKE_INSTALL_DATADIR}/applications"
# then resolves to the absolute path /applications, outside the prefix.
include(GNUInstallDirs)

include(cmake/Dependencies.cmake)
include(cmake/StageFujiProto.cmake)

enable_testing()
add_subdirectory(core)

# The FujiNet runtime: built from the pinned firmware sources as part of the
# normal build and installed alongside the app (the session provisions the
# user's runtime tree from the installed copy on first start). Set up before
# the frontends so the macOS bundle can depend on the library it packs.
include(cmake/FujiNetRuntime.cmake)

# Each frontend is native to one platform; with FRONTEND=all (the default)
# only the host's viable frontends are considered, so a Windows/macOS
# configure never descends into the Linux (pkg-config) frontends and vice
# versa. Naming a frontend explicitly still forces just that one.
#
# add_frontend also skips a directory that has no CMakeLists.txt yet, which is
# how a milestone that has not landed stays out of the way instead of breaking
# the configure for everyone. Naming it explicitly is an error, so nobody can
# ask for a frontend and silently get nothing.
function(add_frontend name)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/frontends/${name}/CMakeLists.txt")
    if(FRONTEND STREQUAL name)
      message(FATAL_ERROR "The ${name} frontend is not implemented yet.")
    endif()
    message(STATUS "${name} frontend: not implemented yet -- skipping")
    return()
  endif()
  add_subdirectory("frontends/${name}")
endfunction()

if(FRONTEND STREQUAL "gnome" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(gnome)
endif()
if(FRONTEND STREQUAL "kde" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(kde)
endif()
if(FRONTEND STREQUAL "macos" OR (FRONTEND STREQUAL "all" AND APPLE))
  add_frontend(macos)
endif()
if(FRONTEND STREQUAL "windows" OR (FRONTEND STREQUAL "all" AND WIN32))
  add_frontend(windows)
endif()

# The ROM-less claim, checked on every binary built with this option off --
# which is the default here, and what every published artifact is built with.
#
# Registered at the top level rather than in core/ so it can be pointed at the
# frontend executables: those are the artifacts a user actually downloads, and
# they link differently from the test binaries.
if(NOT WITH_SMS_ROMS)
  find_package(Python3 COMPONENTS Interpreter)
  set(SMS_ROM_CHECK_TARGETS "")
  if(TARGET boot_smoke)
    list(APPEND SMS_ROM_CHECK_TARGETS boot_smoke)
  endif()
  foreach(_fe gnome kde macos windows)
    if(TARGET fujinet-go-sms-${_fe})
      list(APPEND SMS_ROM_CHECK_TARGETS fujinet-go-sms-${_fe})
    endif()
  endforeach()
  if(Python3_Interpreter_FOUND)
    foreach(_t IN LISTS SMS_ROM_CHECK_TARGETS)
      add_test(NAME no_embedded_roms_${_t}
               COMMAND "${Python3_EXECUTABLE}"
                       "${CMAKE_SOURCE_DIR}/core/tests/no_embedded_roms.py"
                       "$<TARGET_FILE:${_t}>"
                       "${CMAKE_SOURCE_DIR}/tools/roms")
      # SKIPs (ctest 77) rather than fails when tools/roms has no real ROM to
      # probe with -- true of every CI/release checkout.
      set_tests_properties(no_embedded_roms_${_t} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
  endif()
endif()

# Application icons (rendered from source art by tools/icons/make-icons.py).
# Desktop environments look the icon up by the desktop-entry id, so the same
# art is installed under each frontend's id -- at every hicolor size, which is
# what makes it show up crisply everywhere from the 16px window list to the
# 256px app grid.
#
# Only for frontends actually built here: a package should not ship files for
# an app it does not contain.
set(SMS_ICON_SIZES 16 24 32 48 64 128 192 256 512)
set(SMS_ICON_TARGETS_AND_IDS
  fujinet-go-sms-gnome online.fujinet.go.sms.gnome
  fujinet-go-sms-kde online.fujinet.go.sms.kde)
list(LENGTH SMS_ICON_TARGETS_AND_IDS _sms_icon_n)
math(EXPR _sms_icon_last "${_sms_icon_n} - 1")
foreach(_i RANGE 0 ${_sms_icon_last} 2)
  list(GET SMS_ICON_TARGETS_AND_IDS ${_i} _target)
  math(EXPR _j "${_i} + 1")
  list(GET SMS_ICON_TARGETS_AND_IDS ${_j} _id)
  if(TARGET ${_target})
    foreach(_size IN LISTS SMS_ICON_SIZES)
      install(FILES data/icons/hicolor/${_size}x${_size}/apps/fujinet-go-sms.png
              DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
              RENAME ${_id}.png)
    endforeach()
  endif()
endforeach()

# ---- packaging --------------------------------------------------------
#
# One package per frontend, not one package for both: GNOME (GTK4/libadwaita)
# and KDE (Qt6) pull in entirely different runtime library stacks, and a user
# who wants one should not be made to depend on the other.
if(FRONTEND STREQUAL "gnome")
  set(CPACK_PACKAGE_NAME "fujinet-go-sms-gnome")
elseif(FRONTEND STREQUAL "kde")
  set(CPACK_PACKAGE_NAME "fujinet-go-sms-kde")
else()
  set(CPACK_PACKAGE_NAME "fujinet-go-sms")
endif()

set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "FujiNet")
set(CPACK_PACKAGE_CONTACT "https://github.com/FujiNetWIFI/fujinet-go-sms-desktop/issues")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://fujinet.online/")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Self-contained Sega Master System with built-in FujiNet")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

if(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR "DEB;RPM;TGZ")

  # Auto-detected from the actual linked libraries (dpkg-shlibdeps /
  # rpmbuild's own dependency generator) rather than hand-listed: GTK4 vs Qt6
  # alone makes one fixed list wrong for whichever frontend was not built, and
  # a hand-list silently goes stale the moment a dependency's package name
  # changes across distributions.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_SECTION "games")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")

  set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
  set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
  set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
  set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
endif()

include(CPack)
