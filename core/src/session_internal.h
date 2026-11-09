/*
 * smssession's private state. Not installed; only the core/src sources
 * include it. One struct shared by the session's C modules (settings,
 * paths, media, roms, audio, gamepads, bindings).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMS_SESSION_INTERNAL_H
#define SMS_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "smssession.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct smssession {
    char config_dir[SMS_PATH_MAX];
    char data_dir[SMS_PATH_MAX];
    char carts_dir[SMS_PATH_MAX];
    char roms_dir[SMS_PATH_MAX];       /* <data>/roms: imported BIOS + patch ROM */
    char settings_file[SMS_PATH_MAX];

    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;

    char last_error[256];

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    char fujinet_root[SMS_PATH_MAX];    /* <data>/fujinet */
    char fujinet_config[SMS_PATH_MAX];  /* .../fnconfig.ini */
    char fujinet_sd[SMS_PATH_MAX];      /* .../SD */
    char fujinet_data[SMS_PATH_MAX];    /* .../data */
    char fujinet_lib[SMS_PATH_MAX];     /* resolved libfujinet path, "" until then */
    int  fujinet_disabled;              /* the caller passed fujinet_lib "" */
    char fujinet_runtime_src[SMS_PATH_MAX]; /* caller-given pristine tree, or "" */
    char webui_url[64];                 /* http://127.0.0.1:11509/ */
    int  fujinet_running;
    int  fujinet_external;              /* dialing a FujiNet that isn't ours */
    char boip_endpoint[80];             /* "host:port" the cartridge dials */
    char boip_addr[80];                 /* the external address as typed */

    /* cross-thread system-action latch (see smssession_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the running configuration */
    smssession_start_opts opts;
    char cart_path[SMS_PATH_MAX];
    char bios_name[32];

    /* keys the keyboard currently holds, by target, so a release clears
     * exactly what its press asserted even if the binding changed meanwhile */
    uint32_t held_keysym[SMS_TARGET_COUNT];

    /* what drives each joypad contact and console button, one bit per source
     * (SRC_*), so letting go of one source never releases another's.
     * input_mtx covers these arrays and the calls that hand the result to
     * the host (input.c); it is taken last (never while holding the gamepad
     * or bindings lock). */
    pthread_mutex_t input_mtx;
    uint32_t pad_src[SMS_PLAYERS][SMS_ACT_PER_PORT];
    uint32_t sw_src[SMS_SW_COUNT];

    void *audio;              /* audio_sdl.c state, NULL until started */
    void *gamepad;            /* gamepad_sdl.c state, NULL until started */
    void *debugger;           /* smsdebug, lazily created */
    int running;

    /* the last gamepad hot-plug event, for a frontend toast */
    pthread_mutex_t pad_event_mtx;
    char pad_event[128];
};

void settings_init(struct smssession *s);
void settings_free_all(struct smssession *s);

int paths_init(struct smssession *s, const char *config_dir,
               const char *data_dir);
/* The in-process FujiNet makes its runtime root the process's working
 * directory (its PC firmware opens data/ relative to it), so a path a user
 * gave relative to where they started the app -- a command-line argument --
 * is resolved against the directory the first session was created in.
 * paths_resolve returns `path` itself when it is absolute, else `buf`. */
void paths_capture_launch_dir(void);
const char *paths_resolve(const char *path, char *buf, size_t bufsz);
/* Locate libfujinet and provision the runtime tree (fnconfig.ini + data/ +
 * SD/) into <data>/fujinet on first run. Returns 0, or -1 if no runtime is
 * available (not fatal to the session -- see fujinet_start). */
int paths_provision_fujinet(struct smssession *s);

void session_set_error(struct smssession *s, const char *fmt, ...);

/* fujinet_runtime.c */
int  fujinet_start(struct smssession *s);
void fujinet_stop(struct smssession *s);
/* Block (up to timeout_ms) until the BoIP port accepts, so the cartridge's
 * first dial-out finds the listener. Returns 0 once up, -1 on timeout. */
int  fujinet_wait_for_boip(struct smssession *s, int timeout_ms);

/* roms.c */
/* Write any images compiled in with WITH_SMS_ROMS into the ROM directory
 * (once; an existing file is left alone). */
void roms_provision_embedded(struct smssession *s);
/* Read the named BIOS ("" or unknown: none) from the ROM directory; returns
 * a malloc'd buffer (caller frees) and its size, or NULL. */
uint8_t *roms_load_bios(struct smssession *s, const char *name, uint32_t *size);
/* The YM2413 patch ROM (0x90 bytes) if imported, else NULL. Caller frees. */
uint8_t *roms_load_instruments(struct smssession *s);

/* Who is holding an input down (the bits of the *_src arrays): the keyboard
 * and the controllers window's console buttons together, the window's pointer
 * (smssession_pointer_set), and for each gamepad (by the stable owner number
 * its slot was given, 0 to SMS_MAX_PADS - 1) its buttons and its analog
 * stick / hat separately. So letting go of one never releases another, even
 * two pads or two buttons bound to the one key. */
#define SMS_MAX_PADS 8
#define SRC_KEYBOARD 1u
#define SRC_PAD(owner)   (1u << (1 + (owner)))
#define SRC_STICK(owner) (1u << (1 + SMS_MAX_PADS + (owner)))
#define SRC_POINTER  (1u << (1 + 2 * SMS_MAX_PADS))

/* input.c: the joypads and the console's buttons, from every source. */
void input_init(struct smssession *ses);
/* Let go of every source and tell the host (a start). */
void input_clear(struct smssession *ses);

/* audio_sdl.c */
int  audio_start(struct smssession *s);
void audio_stop(struct smssession *s);

/* gamepad_sdl.c */
int  gamepad_start(struct smssession *s);
void gamepad_stop(struct smssession *s);
/* Called by the gamepad thread for the pad that owns `owner`. press: a pad
 * button drives a target (a joypad contact or a console button) -- the pad's
 * SRC_PAD source. stick: the analog stick or hat drives a joypad contact --
 * its SRC_STICK source. */
void session_gamepad_press(struct smssession *ses, int owner, int target, int down);
void session_gamepad_stick(struct smssession *ses, int owner, int port, int act, int down);
/* The live "analog sticks drive the D-pad" flag, read under input_mtx. */
int session_analog_joystick(struct smssession *ses);
/* Called by the gamepad thread on a connect/disconnect. */
void session_gamepad_event(struct smssession *s, const char *text);

/* debug.c: the session tells the debugger when the machine goes away and
 * comes back (a power cycle replaces it), and starts the FujiNet trace
 * before the first. */
void smsdebug_trace_fujinet(struct smssession *s);
void smsdebug_before_power_cycle(struct smssession *s);
void smsdebug_after_power_cycle(struct smssession *s);
void smsdebug_destroy(struct smssession *s);

#ifdef __cplusplus
}
#endif

#endif /* SMS_SESSION_INTERNAL_H */
