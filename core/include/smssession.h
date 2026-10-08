/*
 * smssession -- the toolkit-agnostic desktop session for FujiNet Go SMS.
 *
 * Owns the emulator (the Master System core on its own paced thread -- see
 * core/sms/host.h), the FujiNet cartridge, the SDL audio and gamepad
 * backends, the in-process FujiNet runtime, the shared settings store, the
 * remappable key/pad bindings, the imported BIOS images and the media path
 * layout. Frontends (GTK4, Qt6, AppKit, Win32) drive this API and do only
 * windowing, painting and event translation. A frontend that needs
 * something which is not one of those three things belongs here instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMSSESSION_H
#define SMSSESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MAME's visible area: 268 columns (2 of left border, the 256 active, 10 of
 * right border), 224 lines on NTSC and 240 on PAL. Frontends show it at the
 * TV's pixel aspect or with square pixels, per the "aspect" setting. */
#define SMSSESSION_FB_WIDTH      268
#define SMSSESSION_FB_MAX_HEIGHT 240

/* FujiNet's BoIP listener and its web admin UI. High ports of this app's own
 * so a standalone fujinet-pc, or a sibling FujiNet Go app, never collides:
 * ADAM uses 65216/65214, Apple II 1985/8000, CoCo 65504, MSX 65505/64003,
 * Intellivision 65503/64003, Astrocade 11500/11501, ColecoVision
 * 11502/11503, Atari 2600 11504/11505, NES 11506/11507, and the SMS work's
 * own MAME dev harness uses 9995.
 *
 * Direction: FujiNet LISTENS and the SMS FujiNet cartridge dials in, as on
 * every sibling. That decides startup ordering -- see smssession_start. */
#define SMSSESSION_BOIP_PORT  11508
#define SMSSESSION_WEBUI_PORT 11509

/* The host audio device rate; the core box-averages the PSG and FM to it. */
#define SMSSESSION_AUDIO_RATE 48000

/* The accent colour every frontend uses for its highlights (the Map target,
 * the debugger's current line, the FujiNet status dot): the Master System
 * red, as on the icon. */
#define SMSSESSION_ACCENT_RGB 0xE4002B

typedef struct smssession smssession;
typedef struct smsdebug smsdebug;

/* All members optional (NULL = default).
 *  config_dir:  default $XDG_CONFIG_HOME/fujinet-go-sms
 *  data_dir:    default $XDG_DATA_HOME/fujinet-go-sms
 *  fujinet_lib: path to libfujinet.so/.dylib/.dll; default searches
 *               $FUJINET_LIB, the executable's directory, the install
 *               libdir, then tools/fujinet/work/out. "" disables FujiNet.
 *  fujinet_runtime_src: directory holding the pristine fnconfig.ini + data/
 *               + SD/ used to provision the user's runtime tree on first
 *               start (a macOS app passes its bundle's runtime dir). */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} smssession_paths;

smssession *smssession_new(const smssession_paths *paths);
void smssession_free(smssession *s);

/* ---- settings (shared INI; one store for every frontend of this target) --- */
int         smssession_get_int(smssession *s, const char *key, int def);
void        smssession_set_int(smssession *s, const char *key, int value);
const char *smssession_get_str(smssession *s, const char *key,
                               const char *def);
void        smssession_set_str(smssession *s, const char *key,
                               const char *value);
void        smssession_settings_flush(smssession *s);

/* ---- the consoles ---------------------------------------------------------
 * MAME's machines, in this order. The PAL models run at 49.70 Hz; the
 * Japanese Master System has the YM2413 built in; the Mark III has no BIOS
 * socket and takes the FM Sound Unit. */
typedef enum {
    SMS_CONSOLE_SMS1 = 0,     /* sms1     Master System */
    SMS_CONSOLE_SMS1_PAL,     /* sms1pal  Master System (PAL) */
    SMS_CONSOLE_SMS2,         /* sms      Master System II */
    SMS_CONSOLE_SMS2_PAL,     /* smspal   Master System II (PAL) */
    SMS_CONSOLE_SMSJ,         /* smsj     Master System (Japan) */
    SMS_CONSOLE_MARK3,        /* sg1000m3 Mark III */
    SMS_CONSOLE_COUNT
} sms_console;

/* Human-readable / MAME machine name; NULL past the end. */
const char *sms_console_name(int console);
const char *sms_console_id(int console);
int sms_console_is_pal(int console);
int sms_console_has_bios_socket(int console);
int sms_console_has_reset_button(int console);   /* Reset (SMS1) or Rapid (Japan) */
int sms_console_has_fm(int console);             /* built in (Japan) or optional (Mark III) */

/* ---- lifecycle ------------------------------------------------------------ */
typedef struct {
    const char *cart_path;    /* cartridge image; NULL boots the CONFIG client */
    int console;              /* sms_console */
    const char *bios;         /* a BIOS name (smssession_bios_info), or NULL/""
                                 to boot the cartridge directly */
    int fm_unit;              /* Mark III: the FM Sound Unit is fitted */
    int fm_unit_mutes_psg;    /* Mark III: FM on mutes the PSG (the hardware,
                                 as assumed; MAME's unit does not) */
    int analog_joystick;      /* gamepad sticks drive the D-pad too */
    int enable_fujinet;       /* start the in-process FujiNet runtime */
    int enable_audio;         /* open the SDL audio device */
    int enable_gamepad;       /* start the SDL gamepad thread */
} smssession_start_opts;

/* Fills opts from the settings store (keys: cart console bios.<console id>
 * fm_unit fm_unit_mutes_psg analog_joystick enable_fujinet enable_audio
 * enable_gamepad). */
void smssession_default_opts(smssession *s, smssession_start_opts *opts);

/* Starts FujiNet (if enabled) and then the machine.
 *
 * The order is not arbitrary: FujiNet listens and the cartridge dials in, so
 * the listener has to exist before the machine's first transaction or the
 * CONFIG client boots reporting no link. start() brings FujiNet up first and
 * waits briefly for the port.
 *
 * Returns 0, or -1 with smssession_last_error() set. FujiNet failing to
 * start is NOT fatal: the machine boots with the cartridge reporting the
 * link down, which is far more useful than refusing to run. */
int  smssession_start(smssession *s, const smssession_start_opts *opts);
void smssession_stop(smssession *s);
int  smssession_is_running(const smssession *s);
const char *smssession_last_error(const smssession *s);
/* Power-cycle with the current settings (stop + default_opts + start):
 * what Preferences does after a machine option changes. */
int  smssession_restart(smssession *s);

/* ---- cartridges ------------------------------------------------------------
 * The FujiNet cartridge is always in the slot; CONFIG is its resident image.
 *
 * load_cart puts a local image (.sms, .sg, .bin, .rom) into the cartridge's
 * SRAM directly, in place of CONFIG, and power-cycles the console onto it
 * (the cartridge's own mapper engine runs it; an image it cannot map is
 * refused with the reason). A .cfg file beside it (mapper=<MAME slot name>)
 * overrides the mapper choice, as on the hardware. The path is remembered
 * in the "cart" setting, so it boots again next start.
 * Returns 0 or -1 + error. */
int  smssession_load_cart(smssession *s, const char *path);
const char *smssession_cart_path(const smssession *s);
/* 1 if the image can run on the FujiNet cartridge; else 0 and why. */
int  smssession_check_cart(const char *path, char *why, int whysz);

/* The console running now (while stopped: the one the settings name). The
 * "console" setting can run ahead of it while Preferences is open. */
int  smssession_console(const smssession *s);

/* Soft Reset: the console's /RESET line (MAME's soft reset). The cartridge
 * goes back to CONFIG, or an opened cartridge restarts. F3 by default. */
int  smssession_soft_reset(smssession *s);
/* Reset to CONFIG: a power cycle. Ejects an opened cartridge (clears the
 * "cart" setting) and boots CONFIG, exactly as at power-on. Escape by
 * default. Every power cycle (this, Eject, Open Cartridge) takes the
 * running console's BIOS and FM options from the settings as they are now,
 * so an imported BIOS boots here; changing the console itself takes
 * smssession_restart. */
int  smssession_reset_to_config(smssession *s);
/* Eject: the same as Reset to CONFIG (there is no "no cartridge" state
 * worth having on a FujiNet cartridge). */
int  smssession_eject(smssession *s);

/* ---- video ---------------------------------------------------------------
 * copy_frame copies the latest frame into dst (FB_WIDTH*FB_MAX_HEIGHT uint32
 * XRGB8888 pixels) iff its serial differs from *serial_inout, updates it,
 * writes the frame's line count (224 or 240) into *height and returns 1;
 * returns 0 when unchanged, leaving dst alone. Pass 0 to force a copy. */
int  smssession_copy_frame(smssession *s, uint32_t *dst, int *height,
                           uint64_t *serial_inout);
/* 60 or 50: the running console's refresh rate (rounded). */
int  smssession_refresh_rate(smssession *s);

/* Feed the UI's frame-clock ticks (CLOCK_MONOTONIC ns). While a steady
 * stream near the console's refresh rate arrives, the machine phase-locks
 * one frame per tick; otherwise it paces on the wall clock. A frontend with
 * no frame clock simply never calls this. */
void smssession_notify_vsync(smssession *s, int64_t frame_time_ns);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend
 * that wants the device itself can pull interleaved stereo float frames at
 * SMSSESSION_AUDIO_RATE instead. Returns the frames written (silence is
 * written for any shortfall). */
int  smssession_render_audio(smssession *s, float *out, int nframes);
void smssession_set_volume(smssession *s, int percent);

/* ---- input ---------------------------------------------------------------
 * Every control the machine has, flattened into one target index so the
 * bindings table, the settings store and the frontends can all name them:
 * per port the joypad, then the console's buttons, then the session's own
 * actions. */
typedef enum {
    SMS_ACT_UP = 0, SMS_ACT_DOWN, SMS_ACT_LEFT, SMS_ACT_RIGHT,
    SMS_ACT_1, SMS_ACT_2,
    SMS_ACT_PER_PORT
} sms_action;

typedef enum {
    SMS_SW_PAUSE = 0,                 /* Return by default: the Pause button (NMI) */
    SMS_SW_RESET,                     /* Backspace: the Reset (SMS1) / Rapid (Japan) button */
    SMS_SW_COUNT
} sms_switch;

typedef enum {
    SMS_SYSACT_RESET_CONFIG = 0,      /* Escape by default */
    SMS_SYSACT_SOFT_RESET,            /* F3 by default (MAME's) */
    SMS_SYSACT_DEBUG_STOP,            /* stop in the debugger */
    SMS_SYSACT_COUNT
} sms_sysaction;

#define SMS_TARGET_PORT(port, act) ((port) * SMS_ACT_PER_PORT + (act))
#define SMS_TARGET_SWITCH(sw)      (2 * SMS_ACT_PER_PORT + (sw))
#define SMS_TARGET_SYSACT(sa)      (2 * SMS_ACT_PER_PORT + SMS_SW_COUNT + (sa))
#define SMS_TARGET_COUNT           (2 * SMS_ACT_PER_PORT + SMS_SW_COUNT + SMS_SYSACT_COUNT)

/* Apply one control directly. `down` is press/release. The console buttons
 * are held, as the console samples them once a frame. */
void smssession_press(smssession *s, int target, int down);
void smssession_sysaction(smssession *s, int sysact);
/* Release everything the keyboard holds (focus loss). */
void smssession_release_all(smssession *s);
/* What the console sees held on a port right now (keyboard, gamepads and
 * the on-screen controller together), one bit per sms_action. For a
 * controller window's highlights. */
unsigned smssession_buttons_held(smssession *s, int port);
/* Whether a console button is held (sms_switch). */
int smssession_switch_held(smssession *s, int sw);

/* ---- keyboard translation ------------------------------------------------
 * keysym is an X11/xkb keysym (== a GDK keyval; Qt, Win32 and AppKit map
 * through the HID tables below), so one bindings table serves every
 * frontend. Returns 1 if the key drives a target (and has been applied /
 * released), 0 if it should be ignored. System actions are reported through
 * smssession_key_sysaction instead and left to the frontend. */
int  smssession_key(smssession *s, uint32_t keysym, int down);
/* The system action a keysym is bound to, or -1. */
int  smssession_key_sysaction(smssession *s, uint32_t keysym);

/* Non-printing keys in the keysym space the frontends translate to. */
enum {
    SMS_KEYSYM_NONE = 0,
    SMS_KEYSYM_UP = 0xff52, SMS_KEYSYM_DOWN = 0xff54,
    SMS_KEYSYM_LEFT = 0xff51, SMS_KEYSYM_RIGHT = 0xff53,
    SMS_KEYSYM_ESCAPE = 0xff1b, SMS_KEYSYM_RETURN = 0xff0d,
    SMS_KEYSYM_BACKSPACE = 0xff08, SMS_KEYSYM_TAB = 0xff09,
    SMS_KEYSYM_SPACE = 0x20,
    SMS_KEYSYM_F1 = 0xffbe, SMS_KEYSYM_F2, SMS_KEYSYM_F3, SMS_KEYSYM_F4,
    SMS_KEYSYM_F5, SMS_KEYSYM_F6, SMS_KEYSYM_F7, SMS_KEYSYM_F8,
    SMS_KEYSYM_F9, SMS_KEYSYM_F10, SMS_KEYSYM_F11, SMS_KEYSYM_F12,
    SMS_KEYSYM_LSHIFT = 0xffe1, SMS_KEYSYM_RSHIFT = 0xffe2,
    SMS_KEYSYM_LCTRL = 0xffe3, SMS_KEYSYM_RCTRL = 0xffe4,
    SMS_KEYSYM_LALT = 0xffe9, SMS_KEYSYM_RALT = 0xffea,
    SMS_KEYSYM_KP_0 = 0xffb0, SMS_KEYSYM_KP_1, SMS_KEYSYM_KP_2,
    SMS_KEYSYM_KP_3, SMS_KEYSYM_KP_4, SMS_KEYSYM_KP_5, SMS_KEYSYM_KP_6,
    SMS_KEYSYM_KP_7, SMS_KEYSYM_KP_8, SMS_KEYSYM_KP_9,
    SMS_KEYSYM_KP_ENTER = 0xff8d, SMS_KEYSYM_KP_MULTIPLY = 0xffaa,
    SMS_KEYSYM_KP_DIVIDE = 0xffaf, SMS_KEYSYM_KP_PERIOD = 0xffae
};
/* Native key codes for the platforms whose toolkits do not deliver keysyms:
 * Windows scan code (set 1, with the E0 flag), Linux evdev code (GTK/Qt
 * keycode minus 8) and macOS virtual key code, each mapped to a keysym. 0
 * when unknown. */
uint32_t smssession_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t smssession_keysym_from_evdev(unsigned code);
uint32_t smssession_keysym_from_macos_keycode(unsigned keycode);
/* Name for a keysym ("F1", "Space", "a", "Keypad 5"); returns length. */
int smssession_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* ---- remappable bindings --------------------------------------------------
 * Every target can be driven by one keyboard key and one gamepad button.
 * Rebinding STEALS: a key drives exactly one target, because one keystroke
 * doing two things is worse than losing the old binding. Persisted in the
 * settings store under "bindings" as only the entries that differ from the
 * defaults. */
typedef enum {
    SMS_PAD_BTN_NONE = -1,
    SMS_PAD_BTN_SOUTH = 0, SMS_PAD_BTN_EAST, SMS_PAD_BTN_WEST,
    SMS_PAD_BTN_NORTH, SMS_PAD_BTN_BACK, SMS_PAD_BTN_GUIDE,
    SMS_PAD_BTN_START, SMS_PAD_BTN_LEFT_STICK, SMS_PAD_BTN_RIGHT_STICK,
    SMS_PAD_BTN_LEFT_SHOULDER, SMS_PAD_BTN_RIGHT_SHOULDER,
    SMS_PAD_BTN_DPAD_UP, SMS_PAD_BTN_DPAD_DOWN, SMS_PAD_BTN_DPAD_LEFT,
    SMS_PAD_BTN_DPAD_RIGHT,
    SMS_PAD_BTN_LEFT_TRIGGER, SMS_PAD_BTN_RIGHT_TRIGGER,
    SMS_PAD_BTN_COUNT,
    /* Raw joystick bands for devices SDL has no gamepad mapping for (a
     * plain HID adapter): button index, and hat directions (hat*8 + dir). */
    SMS_PAD_BTN_RAW_BASE = 64, SMS_PAD_BTN_RAW_LAST = 127,
    SMS_PAD_HAT_BASE = 192, SMS_PAD_HAT_LAST = 255
} sms_pad_button;
#define SMS_PAD_HAT_DIRS 8
#define SMS_PAD_BTN_IS_NAMED(b) ((b) >= 0 && (b) < SMS_PAD_BTN_COUNT)
#define SMS_PAD_BTN_IS_RAW(b)   ((b) >= SMS_PAD_BTN_RAW_BASE && (b) <= SMS_PAD_BTN_RAW_LAST)
#define SMS_PAD_BTN_IS_HAT(b)   ((b) >= SMS_PAD_HAT_BASE && (b) <= SMS_PAD_HAT_LAST)

typedef struct {
    uint32_t keysym;     /* 0 = no key */
    int      button;     /* sms_pad_button, NONE = no gamepad button */
} sms_binding;

const char *sms_target_name(int target);           /* "Player 1: Up" */
const char *sms_target_short_name(int target);     /* "Up" */
sms_binding smssession_binding_get(smssession *s, int target);
/* Bind; the previous holder of the key/button (if any) is described into
 * `stolen` (may be NULL). keysym 0 / button NONE unbinds. */
void smssession_binding_set_key(smssession *s, int target, uint32_t keysym,
                                char *stolen, int stolensz);
void smssession_binding_set_button(smssession *s, int target, int button,
                                   char *stolen, int stolensz);
void smssession_bindings_reset(smssession *s);
const char *sms_pad_button_name(int button);
/* The target a keysym / pad button drives, or -1. */
int smssession_target_for_key(smssession *s, uint32_t keysym);
int smssession_target_for_button(smssession *s, int port, int button);

/* Map mode: after begin(), the next gamepad button pressed on any pad is
 * reported by poll() (returns 1 and the button). cancel() disarms. */
void smssession_gamepad_capture_begin(smssession *s);
void smssession_gamepad_capture_cancel(smssession *s);
int  smssession_gamepad_capture_poll(smssession *s, int *button);

void smssession_set_analog(smssession *s, int joystick);

/* ---- gamepads (SDL, hotplugged; started by smssession_start) -------------
 * Pads are assigned to ports in connection order unless assigned
 * explicitly. A pad that disconnects and reconnects gets its port back. */
int  smssession_gamepad_count(smssession *s);
int  smssession_gamepad_name(smssession *s, int idx, char *dst, int dstsz);
void smssession_gamepad_assign(smssession *s, int idx, int port); /* -1 = auto */
int  smssession_gamepad_assignment(smssession *s, int idx);
int  smssession_gamepad_effective_port(smssession *s, int idx);
/* Bumped on every add/remove so a frontend can refresh its lists cheaply. */
unsigned smssession_gamepad_generation(smssession *s);
/* The last hot-plug event as text ("Connected: 8BitDo SN30 Pro (player 1)",
 * "Disconnected: ..."), for a toast; returns length, 0 if none yet. */
int  smssession_gamepad_last_event(smssession *s, char *dst, int dstsz);

/* ---- cross-thread system actions ------------------------------------------
 * The gamepad thread cannot call into a UI toolkit, so a system action it
 * resolves is posted here and a frontend's timer takes it. */
void smssession_sysaction_post(smssession *s, int sysact);
int  smssession_sysaction_take(smssession *s, int *out);

/* ---- BIOS and patch ROMs ---------------------------------------------------
 * Never required (see COMPLIANCE.md): every console boots the cartridge
 * directly. Import BIOS copies a user's own image into the ROM directory,
 * identified by size and CRC-32 against MAME's table; the YM2413's patch
 * ROM is imported the same way and used whenever it is present. */
typedef struct {
    const char *name;         /* MAME's ROM_SYSTEM_BIOS name: "bios13" */
    const char *file;         /* stored as: "mpr-10052.rom" */
    const char *desc;         /* "US/European BIOS v1.3 (1986)" */
    uint32_t size, crc;
    unsigned consoles;        /* bit per sms_console it fits */
} sms_bios_info;

/* The known images (the last entry is the YM2413 patch ROM, consoles 0). */
int smssession_bios_count(void);
const sms_bios_info *smssession_bios_info(int i);
int smssession_bios_find(const char *name);       /* index, or -1 */
/* 1 if image i is in the ROM directory. */
int smssession_bios_available(smssession *s, int i);
/* Import a BIOS or patch ROM. Returns the image's index (>= 0), or -1 with
 * the error. An image of a plausible BIOS size whose CRC is not in the
 * table is imported as a custom BIOS (index smssession_bios_count(), name
 * "custom") with a warning in `msg`. `msg` always says what happened. A
 * BIOS imported for the current console becomes its BIOS (it boots at the
 * next power cycle). */
int smssession_import_bios(smssession *s, const char *path, char *msg, int msgsz);
/* 1 if `path` is a BIOS or patch ROM by size and CRC (a dropped file). */
int smssession_media_is_bios(const char *path);
/* The BIOS selected for a console ("" = none) and its setter; the setter
 * takes effect at the next power cycle. */
const char *smssession_console_bios(smssession *s, int console);
void smssession_set_console_bios(smssession *s, int console, const char *name);
const char *smssession_roms_path(const smssession *s);

/* ---- FujiNet -------------------------------------------------------------*/
int         smssession_fujinet_running(const smssession *s);
const char *smssession_fujinet_webui_url(const smssession *s);
int         smssession_fujinet_copy_log(smssession *s, char *dst, int max);
/* The cartridge's link to FujiNet: 1 up, 0 down, -1 not running. */
int         smssession_cart_link_up(smssession *s);
/* Status text from the cartridge ("connected", "link down", "loading 42%",
 * "game running; mailbox closed", ...). Returns length. */
int         smssession_cart_status(smssession *s, char *dst, int dstsz);
/* 1 once anything other than CONFIG is in the cartridge (a network boot or
 * an opened cartridge file). */
int         smssession_cart_booted_game(smssession *s);

/* ---- media ---------------------------------------------------------------
 * Import a cartridge (and its .cfg sibling, if any) into FujiNet's SD folder
 * so CONFIG can boot it through the cartridge. Returns 0 and the
 * destination path, or -1 with the error. */
int  smssession_import_cart_to_sd(smssession *s, const char *src_path,
                                  char *dest_out, int dest_sz);
/* Routes a dropped file: cartridge images to the cartridge directory (the
 * returned path is usable with load_cart), BIOS images to the ROM directory
 * (as import_bios), anything else to the FujiNet SD folder. */
int  smssession_import_media(smssession *s, const char *src_path,
                             char *dest_out, int dest_sz);
int  smssession_media_is_cartridge(const char *path);

const char *smssession_config_path(const smssession *s);
const char *smssession_data_path(const smssession *s);
const char *smssession_carts_path(const smssession *s);
const char *smssession_sd_path(const smssession *s);

/* ---- debugger ------------------------------------------------------------*/
smsdebug *smssession_debugger(smssession *s);

#ifdef __cplusplus
}
#endif

#endif /* SMSSESSION_H */
