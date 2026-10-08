# FujiNet Go — Sega Master System

A self-contained Sega Master System with a built-in
[FujiNet](https://fujinet.online/): power on into the FujiNet CONFIG client,
browse a network host from the console, boot `.sms` images over the network,
and let a FujiNet-aware program keep talking to the network — all in one
desktop app. A member of the FujiNet Go desktop family
(`fujinet-go-adam-desktop`, `-apple2-`, `-coco-`, `-msx-`, `-intv-`,
`-astrocade-`, `-coleco-`, `-atari2600-`, `-nes-`).

> Bring-up in progress; see `TODO` for the milestone log.

| | |
|---|---|
| **Emulator** | MAME's Master System (`sms.cpp`, the 315-5124/5246 VDP, the SEGAPSG, ymfm's YM2413) transposed to C around floooh's cycle-stepped `z80.h`, the astrocade sibling's way. Pixel-identical to MAME frame for frame (`tools/ab/mame_ab.py`). |
| **Machines** | Master System, Master System II (NTSC and PAL), the Japanese Master System (YM2413) and the Mark III (optional FM Sound Unit). No BIOS needed. |
| **Cartridge** | The FujiNet SMS cartridge (`fujinet-firmware` `pico/sms`): the mailbox at `$B000`, the 2K loader page at `$B800`, 1 MB of SRAM on the cartridge's own `smsmap` mapper engine, CONFIG baked in. Its protocol, mapper and load sources are the firmware's own, staged verbatim. |
| **FujiNet** | The firmware's `RS232` PC target, built in-process as `libfujinet` (BoIP on **11508**, web admin on **11509**). |

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

fujinet-firmware is cloned at its pinned commit by the configure step. To
develop against a working checkout of the cartridge firmware:

```sh
cmake -B build -DFUJINET_SRC=~/Workspace/fn-sms
```

`-DFRONTEND=none` builds just the core and its tests.

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance.
