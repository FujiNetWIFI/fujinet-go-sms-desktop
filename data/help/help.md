This is the text of FujiNet Go SMS's built-in Help (Help ▸ FujiNet Go SMS
Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only to the
frontend it is about; here you see them all. Every topic below is one page in
the app.

# Getting started

FujiNet Go SMS is a Sega Master System with a FujiNet cartridge in its slot.
The console is MAME's: the Z80, the 315-5124 and 315-5246 VDP, the PSG and the
YM2413 FM sound. The FujiNet runs inside the app, so CONFIG comes up at once
and can load games from the network.

1. **Start the app.** The console powers on and runs FujiNet's CONFIG, the
   cartridge's resident image, exactly as on the hardware. It needs no BIOS:
   every console boots the cartridge directly. See
   [BIOS and ROMs](#bios-and-roms).
2. **Pick a game in CONFIG.** Choose a host (the SD folder or a server on the
   network) and an image on it. FujiNet pushes the file to the cartridge, the
   loader page copies it into the SRAM one 8K window at a time, and it runs.
   See [FujiNet](#fujinet).
3. **Or open a cartridge of your own.** **Open Cartridge…**
   (<kbd>Ctrl+O</kbd>) takes a `.sms`, `.sg`, `.bin` or `.rom` image. See
   [Cartridges](#cartridges).
4. **Play.** The keys, the gamepads and the Controllers window
   (<kbd>F9</kbd>) work the two joypads and the console's buttons. See
   [Controllers and keys](#controllers-and-keys).
5. **Change the console.** [Preferences](#preferences) picks the console, the
   BIOS, the FM sound unit, the picture, the sound and FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers window and the FujiNet
console log opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-sms-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The console's picture fills the window. The menus have:

- **Open Cartridge…**, **Eject Cartridge**, **Import Cartridge to SD…** and
  **Import BIOS…**: see [Cartridges](#cartridges) and
  [BIOS and ROMs](#bios-and-roms).
- **Pause**, the **Reset button**, **Soft Reset** and **Reset to CONFIG**.
- **Controllers** (<kbd>F9</kbd>) and **Debugger** (<kbd>F12</kbd>).
- **TV Aspect**, **Smooth Scaling** and **Fullscreen** (<kbd>F11</kbd>).
- **FujiNet Web UI** and **Console Log**.
- **Preferences** (<kbd>Ctrl+,</kbd>), **Help**, **About** and **Quit**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde windows -->

The menu bar has Machine, Console, View, FujiNet and Help. The Machine menu
has the cartridge and BIOS items, Preferences (Settings on Windows) and Quit
(Exit on Windows).

<!-- end -->

<!-- only: macos -->

On a Mac, Preferences (called Settings) and Quit are in the app menu, and the
cartridge and BIOS items are in the Machine menu.

<!-- end -->

The status shows the cartridge and FujiNet's state:

| Status | Means |
|---|---|
| connected | The cartridge is talking to FujiNet. |
| link down | FujiNet is off or not answering yet; CONFIG tries again. |
| loading 42% | FujiNet is sending a program to the cartridge. |
| game running; mailbox closed | A game that does not use FujiNet is running. |

Messages (a cartridge opened, a gamepad plugged in, FujiNet restarted) show
for a few seconds over the picture.

## Reset and power

- **Pause** (<kbd>Return</kbd>) is the NMI button.
- **Reset button** (<kbd>Backspace</kbd>) is the Master System's Reset and the
  Japanese console's Rapid; the Master System II and the Mark III have none.
- **Soft Reset** (<kbd>F3</kbd>) pulls the console's /RESET line: the
  cartridge goes back to CONFIG, or an opened cartridge restarts.
- **Reset to CONFIG** (<kbd>Ctrl+R</kbd>, or <kbd>Escape</kbd>) is a power
  cycle: it ejects an opened cartridge and boots CONFIG again.

## The picture

The console draws 268 × 224 pixels (240 lines on the PAL consoles). **TV
Aspect** shows them with a television's wide pixels (8:7 on NTSC), or square.
**Smooth Scaling** blends the pixels when the picture is stretched, and leaves
them sharp when it is off.

# Controllers and keys

The Master System has two joypads, each a D-pad and two buttons, **1** and
**2**. The console's buttons are **Pause** and **Reset**.

## Default keys

Keys go by where they are on the keyboard, whatever your layout.

| Does | Player 1 | Player 2 |
|---|---|---|
| Up, down, left, right | The arrow keys | <kbd>I</kbd>, <kbd>K</kbd>, <kbd>J</kbd>, <kbd>L</kbd> |
| Button 1 | <kbd>Z</kbd> | <kbd>N</kbd> |
| Button 2 | <kbd>X</kbd> | <kbd>M</kbd> |

| Console | Key |
|---|---|
| Pause | <kbd>Return</kbd> |
| Reset | <kbd>Backspace</kbd> |
| Soft Reset | <kbd>F3</kbd> |
| Reset to CONFIG | <kbd>Escape</kbd> |

A key does one thing: giving a key to a new job takes it from the old one.
Opposite directions held together cancel, so the D-pad never holds up and
down, or left and right, at once.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+O</kbd> | Open Cartridge |
| <kbd>Ctrl+R</kbd> | Reset to CONFIG |
| <kbd>Ctrl+,</kbd> | Preferences |
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Controllers |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome kde windows -->

Keys with <kbd>Ctrl</kbd> never reach the console.

<!-- end -->

<!-- only: macos -->

Keys with ⌘ never reach the console.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. A pad plugged in while a game runs is assigned to the next free player
within a second, unplugging releases it, and a pad that comes back gets its
player back. A toast says when one comes or goes. Preferences ▸ Input and the
Controllers window can fix which pad is which player. A pad's player is
remembered between runs: plug it back in, or start the app again, and it gets
its player back.

| Gamepad | Does |
|---|---|
| D-pad | Up, down, left, right |
| South (A) | Button 1 |
| East (B) | Button 2 |
| Start | Pause |
| Back | Reset |

With **Left stick drives the D-pad** on, the left stick is the D-pad too: it
engages at about a third of its travel and lets go at about a seventh. A
gamepad with no standard layout (a plain USB adapter) uses its first hat, and
its buttons, which can be given jobs in the same way. Every pad, the keys and
the mouse hold separately, so letting go of one never lets go of another.

## Changing the controls

The controls are changed in the Controllers window (<kbd>F9</kbd>): click
**Map**, then click a control on the picture (17 in all: each joypad's four
directions and buttons 1 and 2, Pause, Reset, and the app's own Reset to
CONFIG, Soft Reset and Stop in Debugger), then press the key or the gamepad
button to give it. The control's old key or button is taken from it.

<!-- only: macos -->

A control can also be chosen from the list under the picture.

<!-- end -->

**Clear** takes the job off the key and the button of the control you picked,
**Defaults** puts every control back (so does **Reset Controls** in
Preferences ▸ Input), **Show bindings** writes each control's key on it, and
<kbd>Escape</kbd> leaves Map one step at a time. <kbd>F9</kbd>, <kbd>F11</kbd>
and <kbd>F12</kbd> are the app's and cannot be given to a control. The map is
kept in the settings, and only what differs from the defaults is written.

<!-- context: controllers -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) draws the console as a player sees it:
across the top the console's strip, with the SEGA wordmark and the model's
name, **PAUSE** and, where the model has one, **RESET** (the Japanese console
has **RAPID** in its place; the Master System II and the Mark III have only
PAUSE), and below it the two Control Pads, Player 1 on the left and Player 2
on the right. Each pad is black, with the D-pad (a dish with a cross) and the
square buttons **1** and **2**. It floats on its own at a fixed size, hidden
until you ask for it, and it keeps its size when the console changes. The
controls that are held light up in the accent colour, whoever holds them: the
keyboard, a gamepad or the mouse.

| With the mouse | Does |
|---|---|
| Press an arm of the D-pad | Holds that direction |
| Drag from the D-pad's dish | Holds the direction dragged toward, a diagonal too |
| Press button 1 or 2 | Holds it for as long as the button is down |
| Press PAUSE or RESET | Holds it |
| Drag off a control | Lets go of it |

The three buttons under the picture (Reset to CONFIG, Soft Reset and Stop in
Debugger) act when you release the mouse over them. Under them is a row for
each gamepad plugged in: its name, the player it drives, and a choice of
*Automatic*, *Player 1* or *Player 2*.

The console samples its controls once a frame, so a button is *held*, not
pulsed. Switching to another window lets go of everything the mouse holds.
Keys typed in the Controllers window go to the console. Pointing at a control
shows what drives it: its key and its gamepad button.

**Map** picks a control to change: once one is picked, the others dim and the
picked one is outlined. **Clear**, **Defaults** and **Show bindings** work
with it (see [Changing the controls](#changing-the-controls)). With Show
bindings on, or in Map, the arms and buttons show their keys, arrows as ←
↑ → ↓, and long names cut short. Show bindings is remembered between runs
(setting `controllers_show_bindings`).

# Machines

Preferences ▸ Machine ▸ **Console** picks the machine:

- **Master System**: the original, with the 315-5124 VDP and a Reset button.
- **Master System (PAL)**: the same, at 50 frames a second.
- **Master System II**: the later model, with the 315-5246 VDP and no Reset
  button.
- **Master System II (PAL)**: the same, at 50 frames a second.
- **Master System (Japan)**: with the YM2413 FM sound built in and the Rapid
  button.
- **Mark III**: the Japanese predecessor, with or without the FM Sound Unit.

A change starts a new console when Preferences closes: an opened cartridge
boots again on it.

# BIOS and ROMs

Nothing is required: every console boots the cartridge directly. The BIOS and
the YM2413's instrument ROM are copyrighted, so none comes with the app.

| ROM | Is |
|---|---|
| The BIOS | a console's boot ROM, which shows its logo and hands over to CONFIG as on a real console |
| `ym2413_instruments.bin` | the YM2413 (FM) instrument ROM |

**Import BIOS…** (in the Machine menu, and in Preferences) takes your own BIOS
image. It is identified by size and CRC against MAME's table, whatever the file
is called; an unknown one is accepted with a warning. The YM2413's instrument
ROM is imported the same way. Dropping the file on the window does the same.
Preferences ▸ Machine ▸ **BIOS** then chooses one per console. Imported images
are copied into the app's `roms` folder (see
[Files and folders](#files-and-folders)).

# Cartridges

A cartridge is a file: a `.sms`, `.sg`, `.bin` or `.rom`. Every image (CONFIG,
a game booted over the network, an opened cartridge file) runs on the FujiNet
cartridge's own `smsmap` mapper engine, so what works here works on the
cartridge: up to 1 MB, with the mapper chosen by the cartridge's own database
and heuristics, or by a `.cfg` file beside the image (`mapper=codemasters`, …).
It knows Sega, Codemasters, the Korean mappers, Zemina, Nemesis, Janggun and 4
PAK. An image it cannot map is refused with the reason.

- **A FujiNet app** carries the `FUJI` claim at `$7FDC` and keeps the
  cartridge's mailbox live: it can talk to the network.
- **A game** is any other image; it closes the mailbox.

## Open Cartridge

**Open Cartridge…** (<kbd>Ctrl+O</kbd>) puts a local image straight into the
cartridge's SRAM in place of CONFIG. It is remembered and starts again next
time; **Eject Cartridge** or **Reset to CONFIG** forgets it. Dropping a
cartridge file on the window does the same; a BIOS is imported, and anything
else goes to the SD folder.

<!-- context: import-sd -->
## Import Cartridge to SD

**Import Cartridge to SD…** copies a cartridge, with its `.cfg`, into
FujiNet's SD folder, so CONFIG can list it and boot it through the cartridge.
The cartridge refuses what it cannot serve, and the message says why.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, and the
cartridge in the slot is its connection to the console: the mailbox at $B000,
the 2K loader page at $B800 and 1 MB of SRAM. The cartridge dials FujiNet over
a local socket (BoIP, FujiBus over TCP), and keeps trying again if the link
drops.

## CONFIG

CONFIG is FujiNet's program for choosing what to run:

- **Hosts** are where games come from: the SD folder, and servers on the
  network.
- Pick a host and an image on it. FujiNet pushes it to the cartridge, which
  runs it.
- **Reset to CONFIG** brings CONFIG back.

## The FujiNet menu

- **FujiNet Web UI** opens FujiNet's settings page in your browser, at
  `http://127.0.0.1:11509/`. Only this computer can reach it.
- **Console Log** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom. It is also kept in
`fujinet-console.log` in the app's `fujinet` folder.

## Turning it off

Preferences ▸ FujiNet ▸ **Enable FujiNet** turns the FujiNet off. The
cartridge then reports *link down*, CONFIG runs without hosts, and nothing is
dialled. Games are not affected.

## An external FujiNet

Preferences ▸ FujiNet ▸ **Use external FujiNet** skips the built-in FujiNet and
dials another FujiNet instance at the address you give, for example
`192.168.1.20:11508`. Leave the field empty for `127.0.0.1:11508`. Start that
instance listening there (its `[BOIP]` port). FujiNet Web UI and Console Log
belong to the built-in FujiNet, so they say the FujiNet is external; use the
other instance's own. The Debugger's FujiNet tab still shows the traffic.

## Behind the scenes

- The cartridge and FujiNet talk over TCP port 11508 on 127.0.0.1 (the
  BoIP port), and the web UI is on port 11509. Nothing from outside can
  reach either.
- FujiNet's settings are `fnconfig.ini` in the app's `fujinet` folder. It is
  written once and then left for FujiNet and you.

# Preferences

Preferences (<kbd>Ctrl+,</kbd>) set up the console. It is one page of groups.
The console, the BIOS and the FM settings start a new console when Preferences
closes; the picture, the controllers and the volume apply at once; the FujiNet
settings apply by restarting the session. The settings are one file, shared by
the GNOME and KDE apps on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (in the app menu).

<!-- end -->

<!-- only: windows -->

On Windows this window is called **Settings** (in the Machine menu).

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **Console**: which machine (see [Machines](#machines); setting `console`).
- **BIOS**: which BIOS the console boots, or none (see
  [BIOS and ROMs](#bios-and-roms)), with an **Import BIOS…** button.
- **FM Sound Unit**: the Mark III's optional YM2413 add-on is fitted (setting
  `fm_unit`).
- **FM mutes the PSG**: only the YM2413 is heard while a game has the FM
  switched on (setting `fm_unit_mutes_psg`).
- **FM sound**: the Japanese Master System always has the YM2413 built in.
- **YM2413 instrument ROM**: whether it is imported.

## Display settings

- **Aspect**: *Television (wide pixels)* or *Square pixels* (setting `aspect`).
- **Smooth scaling**: blend the pixels when the picture is stretched (setting
  `smooth`).

## Audio settings

- **Enable audio**: open the system audio device (setting `enable_audio`).
- **Volume**: the output level, 0 to 100 (setting `volume`).

<!-- context: prefs/Input -->
## Input settings

- **Enable gamepads**: poll USB and Bluetooth gamepads (setting
  `enable_gamepad`).
- **Left stick drives the D-pad**: off, only a gamepad's D-pad moves it
  (setting `analog_joystick`).
- **Gamepads**: each one plugged in, and which player it drives. A new
  gamepad drives Player 1, the next Player 2; choose *Player 1* or *Player 2*
  to fix it, or *Automatic*. The choice is remembered between runs (setting
  `pad_assign`), by the gamepad's id and name; two identical gamepads cannot
  be told apart, so the first to connect gets it.
- **Reset Controls**: puts every key and gamepad button back to its default.
- **Open Controllers**: shows the Controllers window, where the keys and
  buttons are changed (see
  [Changing the controls](#changing-the-controls)).

## FujiNet settings

- **Enable FujiNet**: run the built-in FujiNet (setting `enable_fujinet`).
- **Use external FujiNet**: dial another instance instead (setting
  `fujinet_external`).
- **Address**: its `host:port`, or just a port (see
  [An external FujiNet](#an-external-fujinet); setting
  `fujinet_external_addr`).

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The others are `cart` (the opened cartridge,
empty for CONFIG), `bios.<console id>` (the BIOS chosen for each console),
`bindings` (the controls that differ from the defaults), `pad_assign` (the
gamepads fixed to a player, remembered between runs; written by the app) and
`controllers_show_bindings` (1 when the Controllers window shows each control's
key).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has twelve tabs; closing it lets the console go on as it was. The Z80's
registers and flags sit in a strip under the tabs: edit a value while stopped
and press <kbd>Enter</kbd>.

| Button | Does |
|---|---|
| Stop or Run (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a CALL, to the instruction after it |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Scanline+1 | On to the next scanline |
| Frame+1 | On to the next frame |
| Clear Breakpoints | Removes every breakpoint |
| Break at | A symbol name or hex address, then <kbd>Enter</kbd> |
| Load Symbols | Reads a WLA-DX `.sym`, z88dk `.map`, SDCC `.noi` or plain list |
| Clear Symbols | Drops loaded symbols, keeping the built-ins |
| Save | Saves the disassembly, memory, VRAM, CRAM or registers to a file |
| Pop Out | Moves the tab that shows into a window of its own; **Dock** in that window puts it back |
| Help | Opens the topic of the tab that shows |

Every stop brings the views back to where the machine is, even if you had
scrolled away, and the line at the top says why it stopped.

<!-- context: debugger/console -->
## Console

The Console takes commands, with completion on <kbd>Tab</kbd>. Conditions run
over registers, flags, memory and the beam. `help` lists the commands.

| Command | Does |
|---|---|
| `break` | An execute breakpoint |
| `bpr`, `bpw`, `bpa` | Memory reads, writes, or both |
| `bpin`, `bpout` | I/O port reads or writes |
| `bpv`, `bpc` | Writes to video RAM or colour RAM |
| `print` | Evaluates an expression |
| `mem`, `poke` | Dumps memory; writes bytes |
| `vram`, `cram`, `vdp` | The VDP's video RAM, colour RAM and registers |
| `video` | The Video tab as text: `video colours`, `video lines` |
| `sprites` | The sprites |
| `psg`, `fm` | The PSG and the YM2413 |
| `io` | The I/O ports and the memory map |
| `cart`, `mailbox` | The cartridge, and FujiNet's mailbox |
| `label` | Name an address |
| `runto` | Runs to an address |
| `disasm` | Disassembles |
| `trace` | The instruction trace |
| `watch`, `unwatch` | The watch list: `watch score`, `unwatch 0` |
| `find` | Searches memory for bytes (`find 3E 5A`) or text (`find text HELLO`) |
| `save` | Saves state to a file |

<!-- context: debugger/disassembly -->
## Disassembly

The Z80 disassembled at the PC, with the opcode bytes and labels. The current
instruction is in the app's accent colour and a breakpoint is a red dot in the
margin (a grey hollow one when it is disabled); click a line to toggle its
breakpoint. **Follow PC** keeps the current line in view. Symbols load from
WLA-DX `.sym`, z88dk `.map` and SDCC `.noi` files; banked symbols follow the
cartridge's mapping. The ports, vectors, mapper registers and the FujiNet
mailbox are labelled out of the box.

<!-- context: debugger/memory -->
## Memory

The 64K the Z80 sees, 16 bytes to a row, each row labelled with what answers
there (the BIOS, a cartridge bank, RAM and its mirror, the mapper registers).
**Go To** takes an address, a label, an expression or a range (`$C000-$C0FF`,
`score..score+3`) and highlights what it found. **Poke** writes bytes. The
search bar finds hex bytes (`3E 5A`) or text (not case-sensitive) with **Find
Next** and **Find Previous**, wrapping once; the machine has to be stopped.

<!-- context: debugger/breakpoints -->
## Breakpoints

Every breakpoint and watchpoint. **Add** one with a type, an address or range
and, if you like, a condition:

- **Execute** stops before an instruction runs.
- **Read**, **Write** and **Access** (both) stop after the instruction that
  touched memory, and the line at the top says what it did, for instance
  `wrote $C010 = $41 (was $40)`.
- **IN** and **OUT** take a port number (0 to 255).
- **VRAM write** takes a video RAM address ($0000 to $3FFF) and **CRAM write** a
  colour RAM entry (0 to 31); they stop after the instruction that wrote it.

A condition can use `value` and `addr`, and `old`, what was there before:
`value != old` stops only when a write changes something. The box beside each
entry turns it off, and the list shows how many times it fired. **Clear Hits**
zeroes the counts and **Clear Breakpoints** removes them all.

<!-- context: debugger/watches -->
## Watches

Things to keep an eye on: type an address, a symbol, an expression or a range
and press <kbd>Enter</kbd>. Each shows its value while the machine is stopped
(`-` while it runs, `?` if it means nothing yet, which a later **Load Symbols**
may fix). **Show in Memory** jumps to it in the Memory tab, **Break on Change**
adds a Write breakpoint that fires when its value changes, and **Remove**
drops it.

<!-- context: debugger/cart -->
## Cart and I/O

The cartridge: its file, the mode, the mapper and its banks, the mailbox, the
load progress and the BIOS snoop. Under it the memory map as the console sees
it, then the memory and I/O control ports, the BIOS paging and the pads.

<!-- context: debugger/trace -->
## Trace

Every instruction the Z80 runs, live. **Record** turns it on (it slows the
console a little, so it starts off), **Clear** empties it, **Branches only**
shows just the jumps, calls and returns, and **In range** only a range of
addresses. Trace and History share one recording.

<!-- context: debugger/history -->
## History

The same recording, read at a stop: the lead-up to a breakpoint or a pause,
newest at the bottom, with the registers at each instruction. Turn **Record**
on before you run to the place you want to look at.

<!-- context: debugger/video -->
## Video

The VDP, with the picture on the left and five sections on the right. Games
change the scroll, the registers and the colours between lines (a status bar
that stays still while the playfield scrolls), so the console records what each
line was drawn with and the views use it.

| View | Shows |
|---|---|
| Display | The last finished frame, as the screen showed it |
| Decoded | Video RAM drawn through each line's own registers; a chooser picks the screen, the name table, the tiles (background or sprite palette), the sprites or the palette |
| Beam | Stopped only: this frame as far as the beam has drawn it, the last one dimmed below |
| Values | The raw colour indexes, cool for the background palette and warm for the sprite palette |

**Zoom** goes from 1 to 16 times, or **Fit**. **Overlays** switch on a grid of
tiles, the scroll window, the lines where a line interrupt fires, the beam, the
video RAM bytes written lately (they fade), the bytes that changed since the
last refresh, the outline of the active display, and a box on every sprite.

Point at a pixel to see it in **Pixel**: its name table entry and address, the
tile and its pattern address, the palette and colour, the sprite that owns it
and the scroll it was drawn with. Click to pin it, click again to let go.
Double-click it, or press **Break on writes**, to stop the machine when anything
writes that tile or entry.

**Registers** decodes R0 to R10 and the status; one that changed in the last
few refreshes is highlighted. **Colours** shows the 32 colour RAM entries with
the pixels that use them; **Break on CRAM write** stops the machine when one is
written. **Lines** lists the lines where the scroll, the registers or the
colours change. **Timing** shows the beam.

<!-- context: debugger/sound -->
## Sound

The PSG's three tones and noise, and the YM2413's registers when the console
has one.

<!-- context: debugger/symbols -->
## Symbols

Every known symbol, the built-ins and the ones you loaded. **Filter** narrows
the list, and activating a row goes to that address in the Disassembly.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every FujiBus packet on the cartridge's link, both ways, with hex, from the
start. **Clear** empties the list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-sms/settings.ini` and
the data is in `~/.local/share/fujinet-go-sms` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-sms\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-sms`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-sms/settings.ini` and
the data is in `~/.local/share/fujinet-go-sms`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the imported BIOS and instrument ROMs |
| `carts` | cartridges dropped on the window |
| `fujinet` | `fnconfig.ini`, the SD folder (`SD`), and the console log |

# Troubleshooting

- **link down**: FujiNet is off, not started yet, or the external address
  is wrong. CONFIG tries again; check Preferences ▸ FujiNet.
- **The cartridge was refused**: the message says why: too big, empty, not a
  cartridge image, or a mapper it cannot map; a `.cfg` file beside the image
  can name one.
- **Import Cartridge to SD refused a game**: the FujiNet cartridge cannot
  serve it; open it with Open Cartridge instead.
- **Keys don't reach the console**: a dialog or another window is in front,
  or the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A gamepad does nothing**: it may not be on the player you expect (see
  [Gamepads](#gamepads)), or its buttons may be given other jobs.
- **No FM sound**: the YM2413's instrument ROM is not imported, or the Mark
  III's FM Sound Unit is off.
- **No sound**: the audio device could not be opened, or the volume is 0.
- **A port is taken**: another program is using 11508 or 11509. Close the
  other FujiNet, or point the app at it with **Use external FujiNet**.

# About and licences

FujiNet Go SMS is free software under the GNU General Public License, version
3 or later. Copyright © 2026 Thomas Cherryhomes.

- **MAME** (the Master System driver, GPL-2.0-or-later and BSD-3-Clause) is
  what the console is modelled on, with floooh's `z80.h` (zlib) and ymfm's
  YM2413 (BSD-3-Clause).
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app, and the
  cartridge's own sources are the firmware's.
- No BIOS or other copyrighted ROM is part of the app; you import your own.

[fujinet.online](https://fujinet.online/) ·
[Report an issue](https://github.com/FujiNetWIFI/fujinet-go-sms-desktop/issues)
