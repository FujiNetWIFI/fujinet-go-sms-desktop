/*
 * host.h -- the emulator host: owns the machine and the FujiNet cartridge,
 * runs the machine on its own thread at real speed, and publishes video
 * frames and audio for the session layer.
 *
 * Process-singleton by design, like every core in this family (and the
 * FujiNet cart below it is one too: fujimail's port interface is context-
 * free C function pointers). sms_host_start refuses a second concurrent
 * session.
 *
 * Pacing: the machine runs a whole frame (to the next VBLANK) at a time
 * against an absolute monotonic deadline ladder at the exact frame rate --
 * 59736 T-states at 3.579545 MHz (59.92 Hz) on NTSC, 71364 at 3.546895 MHz
 * (49.70 Hz) on PAL. While a frontend feeds display ticks at that rate
 * (sms_host_notify_vsync) the machine phase-locks one frame per tick
 * instead. The siblings' hardest-won lesson applies: VERIFY the pacing --
 * the boot_smoke test measures it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMS_HOST_H
#define SMS_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "sms_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sms_model_t model;
    const uint8_t *bios;         /* copied; NULL boots the cartridge directly */
    uint32_t bios_size;
    const uint8_t *instruments;  /* the YM2413 patch ROM (0x90), or NULL */
    bool fm_unit;                /* Mark III: the FM Sound Unit is fitted */
    bool fm_unit_mutes_psg;
    const uint8_t *cart;         /* an opened image, copied; NULL boots CONFIG */
    uint32_t cart_size;
    const char *cart_mapper;     /* a .cfg mapper override, or NULL */
    const char *boip_hostport;   /* fujinet-pc's BoIP listener, "host:port" */
    bool cart_sync;              /* run the mailbox inline (tests) */
} sms_host_opts_t;

/* Start the machine thread. Returns 0, or -1 with sms_host_last_error()
 * set (already running, an image the cartridge cannot map, allocation or
 * thread failure). */
int sms_host_start(const sms_host_opts_t *opts);

/* Stop and join the machine thread and power the cart off. Safe when not
 * running. */
void sms_host_stop(void);

bool sms_host_is_running(void);
const char *sms_host_last_error(void);

/* The console's /RESET line (MAME's soft reset): a one-shot latch the
 * machine thread applies at the next frame boundary. */
void sms_host_soft_reset(void);

/* ---- video: latest-wins single slot ----
 * Copies the newest frame into dst (SMS_FB_WIDTH x SMS_FB_MAX_HEIGHT XRGB
 * words) when its serial differs from *serial_inout; returns nonzero, sets
 * *height (224 NTSC / 240 PAL) and updates the serial when it copied. */
int sms_host_frame_copy(uint32_t *dst, int *height, uint64_t *serial_inout);
/* Frames the machine has run since start (for pacing checks). */
uint64_t sms_host_frame_count(void);
/* How the deadline ladder fared (boot_smoke prints it): sleeps taken, how
 * late they woke in total and at worst, and resyncs after falling more
 * than four frames behind. Read after sms_host_stop. */
typedef struct {
    uint64_t sleeps, late_ns, resyncs;
    long worst_late_ns;
} sms_host_pacing_t;
void sms_host_pacing(sms_host_pacing_t *out);
/* The running machine's exact frame rate in Hz (0 when stopped). */
double sms_host_frame_rate(void);

void sms_host_notify_vsync(int64_t frame_time_ns);

/* ---- audio: 48 kHz interleaved stereo float, self-correcting backlog ----
 * Returns frames written (no padding). */
int sms_host_audio_copy(float *dst, int max_frames);
void sms_host_audio_reset(void);

/* ---- inputs (callable from any thread; single-byte stores; latched by
 * the machine once a frame, at VBLANK, as MAME's input ports are) ---- */
void sms_host_pad_set(int port, uint8_t mask);   /* SMS_PAD_* */
void sms_host_pause_set(bool down);
void sms_host_reset_button_set(bool down);       /* Reset (sms1) / Rapid (smsj) */
void sms_host_release_all(void);

/* ---- the cartridge ---- */
void sms_host_cart_status(sms_cart_status_t *out);
/* The cart device, or NULL when not running (the debugger's page banks,
 * SRAM and arena dumps). Use it under sms_host_lock. */
sms_cart_t *sms_host_cart(void);

/* ---- the run lock ----
 * Held by the machine thread while it runs a frame. Any other thread that
 * reads or edits the machine or the cart (sms_host_machine, sms_host_cart)
 * holds it around the access: it waits for the frame in progress, at most a
 * couple of milliseconds. Re-entrant per thread; a no-op on the machine
 * thread. Never call sms_host_stop while holding it. */
void sms_host_lock(void);
void sms_host_unlock(void);
/* The debugger's park: the machine thread lets go of the run lock while it
 * is parked inside the instruction hook, and takes it back to run on. */
void sms_host_park_release(void);
void sms_host_park_reacquire(void);

/* ---- debugger plumbing ----
 * The live machine; valid only while running, and only touched under
 * sms_host_lock (or on the machine thread). */
sms_machine_t *sms_host_machine(void);

/* Instruction-boundary hook (machine thread; may block). */
void sms_host_set_instr_hook(void (*hook)(sms_machine_t *m, void *user), void *user);
/* Bus hook, fired only for the access kinds in `watch` (SMS_WATCH_*). */
void sms_host_set_bus_hook(void (*hook)(sms_machine_t *m, void *user, int kind,
                                        uint16_t addr, uint8_t data),
                           void *user, uint8_t watch);
/* Publish the machine's current framebuffer now (the debugger shows the
 * picture being drawn while stopped). Machine thread only. */
void sms_host_publish_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* SMS_HOST_H */
