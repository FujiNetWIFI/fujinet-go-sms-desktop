# FujiNet Go — Sega Master System

A self-contained Sega Master System with a built-in
[FujiNet](https://fujinet.online/): power on into the FujiNet CONFIG client,
browse a network host from the console, boot `.sms` images over the network,
and let a FujiNet-aware program keep talking to the network — all in one
desktop app. A member of the FujiNet Go desktop family
(`fujinet-go-adam-desktop`, `-apple2-`, `-coco-`, `-msx-`, `-intv-`,
`-astrocade-`, `-coleco-`, `-atari2600-`, `-nes-`).

| | |
|---|---|
| **Emulator** | MAME's Master System (`sms.cpp`, the 315-5124/5246 VDP, the SEGAPSG, the FM Sound Unit) transposed to C around floooh's cycle-stepped `z80.h`, with ymfm's YM2413 — the astrocade sibling's way. Frame for frame pixel-identical to MAME, and its sound matches MAME's (`tools/ab/`). |
| **Machines** | Master System and Master System II, NTSC and PAL; the Japanese Master System (YM2413 FM built in, the Rapid button); the Mark III, with or without the FM Sound Unit. No BIOS needed. |
| **Cartridge** | The FujiNet SMS cartridge (`fujinet-firmware` `pico/sms`): the mailbox at `$B000`, the 2K loader page at `$B800`, 1 MB of SRAM on the cartridge's own `smsmap` mapper engine (Sega, Codemasters, the Korean mappers, Zemina, Nemesis, Janggun, 4 PAK), CONFIG baked in. Its protocol (`fujimail`), wire codec (`fujibus`), mapper engine and load sequence are the cartridge firmware's own sources, staged verbatim. |
| **FujiNet** | The firmware's `RS232` PC target, built in-process as `libfujinet` and dialled by the cartridge over loopback (BoIP on **11508**, web admin on **11509**). |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32/GDI) — each with the display, a Controllers window, Preferences, a live FujiNet console log, and a debugger. |
| **Packaging** | Per-frontend DEB/RPM/TGZ, two Flatpaks, a Windows (x86-64) zip and NSIS installer, and macOS bundles for Apple Silicon (arm64) and Intel (x86_64), all through GitHub Actions. |

## What it is

- **CONFIG is the power-on program.** The CONFIG client (`fujinet-config`'s
  `sms/` build) is the cartridge's resident image, exactly as on the
  hardware. From CONFIG, pick a host and an image; FujiNet pushes the file to
  the cartridge, the loader page copies it into the SRAM one 8K window at a
  time, and it runs.
- **The cartridge, faithfully.** Every image — CONFIG, a network-booted game,
  a local cartridge file — runs on the cartridge's own `smsmap` engine, so
  what works here works on the cartridge: up to 1 MB, with the mapper chosen
  by the cartridge's own database and heuristics or by a `.cfg` file beside
  the image (`mapper=codemasters`, …). An image it cannot map is refused with
  the reason, as the cartridge refuses it. A program that carries the `FUJI`
  claim at `$7FDC` keeps the mailbox; a commercial game closes it.
- **Open Cartridge…** puts a local `.sms`/`.sg` straight into the cartridge's
  SRAM in place of CONFIG (and remembers it for the next start). **Import
  Cartridge to SD…** copies one, with its `.cfg`, into FujiNet's SD root,
  where CONFIG lists it. Dropping a file on the window opens a cartridge,
  imports a BIOS, or puts anything else on the SD card.
- **No BIOS, ever required.** Every console boots the cartridge directly.
  **Import BIOS…** takes your own BIOS image (identified by size and CRC
  against MAME's table; an unknown one is accepted with a warning), and
  Preferences chooses it per console — the BIOS then shows its logo and
  hands over to CONFIG as on a real console. The YM2413's instrument ROM is
  imported the same way. Nothing copyrighted ships with the app
  (`COMPLIANCE.md`).
- **The console's buttons.** **Pause** (Return) is the NMI button. The
  **Reset button** (Backspace) is the Master System's Reset and the Japanese
  console's Rapid; the Master System II and the Mark III have none. **Soft
  Reset** (F3) pulls the console's /RESET line: the cartridge goes back to
  CONFIG, or an opened cartridge restarts. **Reset to CONFIG** (Escape) is a
  power cycle: it ejects an opened cartridge and boots CONFIG again.
- **The debugger, in every frontend.** F12 opens it and stops the machine:
  Run/Stop (F5), Step (F7), Step Over (F8), Step Out (Shift+F8), Scanline+1,
  Frame+1. Tabs: a Prompt (`help`, `break`, `bpr`/`bpw`/`bpin`/`bpout` with
  conditions over registers, flags, memory and the beam, `print`, `mem`,
  `poke`, `vram`, `cram`, `vdp`, `sprites`, `psg`, `fm`, `io`, `cart`,
  `mailbox`, `label`, `runto`, `disasm`, `trace`, `save` … with Tab
  completion), CPU & RAM (editable, shadow registers included), Disassembly
  (click to toggle a breakpoint, follow PC, jump to a label), VDP (registers
  decoded, the name table with the scroll window, the 512 tiles, sprites,
  palette), Sound & I/O (PSG, YM2413, the memory and I/O control ports, BIOS
  paging, the pads), Breakpoints, and Cart (mode, mapper and banks, mailbox,
  load progress, the BIOS snoop). It loads WLA-DX `.sym`, z88dk `.map` and
  SDCC `.noi` symbols — banked symbols follow the cartridge's mapping — and
  the ports, vectors, mapper registers and the FujiNet mailbox are labelled
  out of the box.
- **Gamepads that come and go.** SDL3 hot-plug: a pad plugged in while a game
  runs is assigned to the next free player within a second, unplugging
  releases it, and a pad that comes back gets its player back. The
  **Controllers** window shows both joypads live, presses buttons with the
  mouse, and rebinds any control to a key or gamepad button (Map).

The icon is the family mark in white on the Master System's red
(`#E4002B`), over the square grid of its cartridge labels. The UI accent is
the same red, marking held buttons, the Map target, the current line in the
disassembly and the Run button while stopped.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

fujinet-firmware is cloned at its pinned commit (the `add-sms` branch) by
the configure step — a plain `git clone` with no `--recurse-submodules` is
enough. To develop against a working checkout of the cartridge firmware:

```sh
cmake -B build -DFUJINET_SRC=~/Workspace/fujinet-firmware
```

`-DFRONTEND=none` builds just the core and its tests; `-DWITH_FUJINET=OFF`
skips the firmware build (the machine boots CONFIG reporting the link down).
`-DWITH_SMS_ROMS=ON` embeds whatever BIOS images are in `tools/roms/` for
local development; it is off by default and in every published build.

The core is C11, with ymfm (the YM2413) built as C++17. The macOS bundle
needs macOS 13.3 or later.

The `netboot` test boots an image over the network with the cartridge
bring-up's own clients (`fujiboot.sms`, `hello.sms` and `fujibank.sms`, built
with `BOOT_PATH=/hello.sms`); point it at them to run it:

```sh
BOOT_PATH=/hello.sms tools/fujinet/work/fujinet-firmware/pico/sms/build.sh
SMS_TESTROM_DIR=$PWD/tools/fujinet/work/fujinet-firmware/pico/sms/build \
    ctest --test-dir build -R netboot
```

### Against MAME

`tools/ab/mame_ab.py` runs an image on MAME's own Master System (with the
FujiNet cartridge graft from `pico/sms/emu`) and on this core, frame for
frame, with the same scripted input, and demands identical pixels;
`tools/ab/audio_ab.py` does the same for sound. Both want a MAME tree in
`$MAME`.

### Cross-building Windows on Linux

```sh
curl -LO https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-devel-3.4.12-mingw.tar.gz
tar xzf SDL3-devel-3.4.12-mingw.tar.gz
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake \
      -DFRONTEND=windows -DWITH_FUJINET=OFF \
      -DCMAKE_PREFIX_PATH="$PWD/SDL3-3.4.12/x86_64-w64-mingw32"
cmake --build build-win
cp SDL3-3.4.12/x86_64-w64-mingw32/bin/SDL3.dll build-win/frontends/windows/
wine build-win/frontends/windows/fujinet-go-sms-windows.exe
```

This is the desk-side check (the core and session tests pass under Wine).
`fujinet.dll` does not cross-build — the firmware's PC target needs mingw
builds of its dependencies — so the release build is native MSYS2/UCRT64,
which builds it.

## Ports

FujiNet's BoIP listener is on **11508** and its web admin UI on **11509** —
high ports of this app's own, continuing the family's table (Astrocade
11500/11501, ColecoVision 11502/11503, Atari 2600 11504/11505, NES
11506/11507), so a standalone `fujinet-pc` or a sibling app never collides.

FujiNet listens and the cartridge dials in, so the session starts FujiNet
first and waits for its listener before powering on. The cartridge's mailbox
service runs on its own thread, as it runs on the cartridge's second core, so
a slow network transaction never stalls the picture.

## Keys

| | |
|---|---|
| Player 1 | arrows; **Z** = 1, **X** = 2 |
| Player 2 | **I J K L**; **N** = 1, **M** = 2 |
| Gamepads | D-pad / left stick; South = 1, East = 2, Start = Pause, Back = Reset |
| Pause / Reset button | **Return** / **Backspace** |
| Soft Reset / Reset to CONFIG | **F3** / **Escape** |
| Controllers / Fullscreen / Debugger | F9 / F11 / F12 (debugger: F5 run/stop, F7 step, F8 step over, Shift+F8 step out) |

Every control is remappable in the Controllers window.

## How this was verified

- **The core against MAME** — `tools/ab/mame_ab.py`: CONFIG, timing probes
  (the V and H counters read at every phase of a line, the beam at every
  interrupt entry) and 76 games from a local corpus, pixel-identical to MAME
  over 2400 frames of scripted play, on the Master System, the PAL models and
  the Mark III; real BIOS boots (the 1.3 BIOS, Alex Kidd, the Japanese 2.1
  BIOS with the YM2413 ROM) identical too. `tools/ab/audio_ab.py`:
  *Fantasy Zone II* (Japan) with FM on the Japanese Master System matches
  MAME's sound (no lag, loudness envelope correlation 0.999, the pitch of 98%
  of loud windows), on the Mark III's FM Sound Unit 0.998 / 98%; the PSG
  0.998 / 99% (NTSC) and 1.000 / 100% (PAL). The timing probes' MAME
  results are in the test suite (`vdp_timing`), so every build is checked
  against them.
- **The core on Linux** — `ctest`: the BIOS socket and the cartridge's BIOS
  snoop with a BIOS the test writes itself (`bios_synth`); the console/
  cartridge seam (`cart_seam`); the disassembler and the VDP decoders; the
  paced host (`boot_smoke`: 59.92 Hz and 49.70 Hz measured); the session
  (CONFIG paints; the joypad and Pause reach a program; a cartridge opens,
  survives Soft Reset and is ejected by Reset to CONFIG; an image the
  cartridge cannot map is refused with the reason; console changes; a
  relative path still works after FujiNet moves the working directory);
  media and Import BIOS (identified by CRC with filler images whose CRC was
  steered to MAME's table — no Sega code involved); bindings; gamepads; the
  debugger contract (every step kind, every breakpoint class with
  conditions, registers, memory, VRAM/CRAM, the views, symbols in every
  format, the trace, Save, the prompt, a power cycle). With the in-process
  FujiNet: the link (`fujibus_smoke`), CONFIG driven like a user — SD host,
  the image, OPEN/BOOT, FujiNet pushes it, it runs (`config_boot`) — and
  the bring-up's fujiboot → `hello.sms` network boot with the mailbox on its
  worker thread and inline, plus the 80K `fujibank` app (`netboot`).
  ThreadSanitizer is clean over every threaded test.
- **GNOME and KDE** — built together with no frontend warnings, desktop and
  metainfo files validated, and smoke-launched headless (GTK Broadway, Qt
  offscreen) with the debugger, Controllers and Preferences open; every
  window and debugger tab was rendered to an image and looked at.
- **Windows** — the Win32 frontend cross-builds with mingw-w64 with no
  warnings, the core and session tests pass under Wine, and CI builds and
  tests it natively with MSYS2/UCRT64 — including `config_boot` against the
  real `fujinet.dll`. Not yet run on a Windows desktop.
- **macOS** — compiled and tested only by CI, on Apple Silicon and Intel;
  never run on a Mac yet. The runners' sleeps are too coarse to hold the
  frame rate to 2% there (`boot_smoke` reports it and checks the throttle
  instead).

## Cutting a release

Pushing a `v*` tag builds every platform and, only if all of it passes,
publishes what it produced as a **draft** release:

| Asset | Contents |
|---|---|
| `fujinet-go-sms-gnome-<version>-Linux.{deb,rpm,tar.gz}` | the GNOME frontend, packaged with CPack |
| `fujinet-go-sms-kde-<version>-Linux.{deb,rpm,tar.gz}` | the KDE frontend, packaged with CPack |
| `fujinet-go-sms-<version>-windows.zip` | the exe, `fujinet.dll`, and the `fujinet/` runtime tree |
| `fujinet-go-sms-<version>-windows-setup.exe` | NSIS installer, per-user, no admin rights |
| `fujinet-go-sms-<version>-macos-{arm64,x86_64}.zip` | the `.app` bundle for Apple Silicon and Intel, FujiNet inside |
| `online.fujinet.go.sms.{gnome,kde}.flatpak` | single-file bundles: `flatpak install ./…flatpak` |

The version is declared in the tree (`project(… VERSION …)` and both
metainfo files, which template it), not derived from the tag; `check-version`
stops the release if they disagree. To release 0.2.0: set the version in
`CMakeLists.txt`, add a `<release>` entry with its date to both
`frontends/*/data/*.metainfo.xml.in`, commit, then
`git tag -a v0.2.0 && git push origin v0.2.0`. Running the workflow by hand
(Actions → Release → Run workflow) is a dry run: everything is built and
tested and the artifacts are attached to the run, but no release is made.

The Windows release build is native MSYS2/UCRT64, and the `release.yml` job
checks the exe's and `fujinet.dll`'s import tables against a system-DLL
whitelist. The macOS jobs sign and notarise when the `MACOS_*` secrets are
set, and check the bundle for leaked Homebrew dylibs.

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance.
"Sega", "Master System" and "Mark III" are Sega's trademarks, used to name
the machine emulated; this project is not affiliated with Sega.
