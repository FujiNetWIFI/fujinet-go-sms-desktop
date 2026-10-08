# Compliance

Per-component provenance for `fujinet-go-sms-desktop`, written before the
first public build, in the family tradition (see
`fujinet-go-astrocade-desktop/COMPLIANCE.md`,
`fujinet-go-coleco-desktop/COMPLIANCE.md`).

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **The Master System core** (`core/sms/machine.c`, `vdp.c`, `psg.c`) | transposed to C from MAME: `src/mame/sega/sms.cpp`, `sms.h`, `sms_m.cpp`, `src/devices/video/315_5124.cpp/.h`, `src/devices/sound/sn76496.cpp`, `src/devices/bus/sg1000_exp/fm_unit.cpp` | BSD-3-Clause; the MAME copyright holders are kept in each file's header (Wilbert Pol, Enik Land, Charles MacDonald, Mathis Rosenhauer, Brad Oliver, Michael Luong, Fabio Priuli, Nicola Salmoria) | compiled into `sms_core` |
| **ymfm** (the YM2413) | MAME's `3rdparty/ymfm` (Aaron Giles), vendored verbatim in `core/sms/ymfm/` with its `LICENSE` | BSD-3-Clause | compiled into `sms_core` behind `core/sms/fm_ym2413.cpp` |
| **z80.h**, **z80dasm.h** | floooh's [chips](https://github.com/floooh/chips), vendored verbatim in `core/sms/z80/` | zlib | header-only; `CHIPS_IMPL` in `machine.c` |
| **The FujiNet SMS cartridge device** (`core/sms/fujinet_cart.c`) | transposed from the firmware's MAME device, `fujinet-firmware` `pico/sms/emu/fujinet.cpp` | BSD-3-Clause, © Thomas Cherryhomes (the transposition GPL-3.0-or-later) | compiled into `sms_core` |
| **The cartridge firmware's own sources** (`fujimail.c`, `fujibus.c`, `smsmap.c`, `smsmap_db.c`, `fuji_load.c` and headers, `sms_cart.h`, `fuji_mailbox.h`, the loader page `smsloaderrom.h`) | `fujinet-firmware` `pico/sms/firmware` on the `add-sms` branch, staged verbatim into `core/sms/fuji-generated/` by `cmake/StageFujiProto.cmake` | as marked in each file (the FujiNet project's, © Thomas Cherryhomes) | compiled into `sms_core`, never patched |
| **The CONFIG client** (`fujiconfigrom.h`, 32 768 bytes) | [`FujiNetWIFI/fujinet-config`](https://github.com/FujiNetWIFI/fujinet-config) `sms/`, built with z88dk and `fujinet-lib`'s `sms` target, converted by `pico/sms/tools/mkromh.py`, staged from `pico/sms/firmware/baked/` | GPL-3.0 (the FujiNet project's) | the cartridge's resident image, as on the hardware |
| **FujiNet firmware** (`libfujinet`) | [`FujiNetWIFI/fujinet-firmware`](https://github.com/FujiNetWIFI/fujinet-firmware), PC target `RS232` | GPL-3.0-or-later | built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`'d at run time |
| The frontends (`frontends/{gnome,kde,windows,macos}`) | ported from the NES sibling's (this project's family, © Thomas Cherryhomes) | GPL-3.0-or-later | one executable per platform |
| GTK4, libadwaita (GNOME) / Qt6 (KDE) | the GNOME and Qt projects | LGPL-2.1-or-later / LGPL-3.0 | system libraries, dynamically linked |
| SDL3 | libsdl-org | Zlib | system package on Linux; linked statically on macOS and Windows |
| mbedTLS 3.6.x | Mbed-TLS | Apache-2.0 | for `libfujinet`: system package where it is a usable 3.x, otherwise the pinned source |

Everything above is GPL-3.0-or-later or compatible with it: the combined work
is distributed under GPL-3.0.

## What is changed from MAME

The core is a transposition, not a copy: MAME's C++ device classes become C
structs and functions under MAME's own names (minus `m_`), so the two can be
read side by side. What is deliberately different:

- **The clockwork.** MAME drives the VDP with emu_timers placed by
  `screen().time_until_pos()`; here they are a per-line event table run
  against the CPU's cycle count (`core/sms/vdp.c`), with the beam's time
  origin, its rounding and the moment an event becomes visible to the CPU
  calibrated against MAME with probe ROMs (`tools/ab/probes`).
- **The CPU** is z80.h, not MAME's Z80; the power-on and reset register
  values are forced to MAME's.
- **Left out**: the Game Gear, the store display unit, the SegaScope glasses,
  the light phaser, paddle, sports pad and the other peripherals, the card
  and expansion slots.
- **The YM2413 patch set**: MAME loads `ym2413_instruments.bin`; without an
  imported copy, ymfm's built-in table is used (close, not identical).

`tools/ab/mame_ab.py` runs the core against MAME frame for frame.

## What is changed from the firmware's MAME device

`fujinet_cart.c` keeps `pico/sms/emu/fujinet.cpp`'s structure line for line,
with: the real /M1 from the CPU instead of MAME's "PC equals the address"
guess; the bus snoop fed by the machine on every cycle instead of memory
taps; the mailbox service (`fujimail`) on a worker thread, as on the
cartridge's second core; `FN_R_LINK` published after each service pass, as
the firmware does. The transport is this app's own portable
`core/sms/fujitcp_host.c` (abortable, and a closed peer is a dead link);
the staged `fujitcp.c` is staged but not compiled.

## System ROMs — not redistributed, never required

The Master System BIOSes (Sega's `mpr-10052`, `mpr-12808`, `mpr-11124` and
the others MAME lists) and the YM2413's internal patch set are copyrighted
firmware that is **not** freely licensed. This repository does not contain
or redistribute them, and the machine never needs them: every model boots
the FujiNet cartridge directly, as MAME's `none` BIOS does, and the YM2413
falls back to ymfm's own patch table.

- **Import BIOS…** (every frontend) copies a user's own image into the ROM
  directory, identified by size and CRC-32 against MAME's table; an image
  with a plausible size and an unknown CRC is accepted with a warning.
- `-DWITH_SMS_ROMS=ON` embeds whatever recognised images a developer put in
  `tools/roms/`, for local development only. Every artifact this project
  publishes is built with it **OFF** (every CI, release and Flatpak
  configure passes it explicitly), and the `no_embedded_roms_*` tests
  (`core/tests/no_embedded_roms.py`) check the shipped binaries for the
  ROMs' bytes.
- The tests never use Sega code either. `bios_synth` runs a BIOS the test
  writes itself (a few dozen hand-assembled bytes that do what a BIOS does
  at hand-over), and `media` checks identification with filler images whose
  last four bytes steer the CRC-32 to a table entry. Comparisons with real
  BIOS boots (`tools/ab/mame_ab.py --bios`) are run locally against a
  developer's own MAME ROM set and are not part of the repository.

## Trademarks and names

"Sega", "Master System", "Mark III" and "Sega Master System" are trademarks
of Sega; they are used here to name the machine being emulated, and the
project is not affiliated with or endorsed by Sega. "FujiNet" is the FujiNet
project's name. "MAME" is a registered trademark of Gregory Ember.

## Icon and name

The icon is the FujiNet Go family mark (the same artwork as the other
desktops) in white on the Master System's red, `#E4002B`, over the square
grid of its cartridge labels (`tools/icons/make-icons.py`).
