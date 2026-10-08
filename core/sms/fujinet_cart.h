/* fujinet_cart.h -- the FujiNet SMS cartridge, the one cart this core has.
 *
 * A C transposition of the firmware's own MAME device model
 * (fujinet-firmware pico/sms/emu/fujinet.cpp, BSD-3-Clause, Thomas
 * Cherryhomes): an RP2354B with 1 MB of SRAM behind the cart's page table,
 * the 4K arena at $B000 (the mailbox, plus the 2K loader page at $B800) and
 * CONFIG served RESIDENT at $0000-$7FFF. The mapper engine (smsmap.c), the
 * bus decode and the BIOS snoop (sms_cart.h), the load sequence
 * (fuji_load.c), the protocol (fujimail.c) and the wire codec (fujibus.c)
 * are the cartridge firmware's own sources, staged verbatim into
 * fuji-generated/ by cmake/StageFujiProto.cmake; this device is only the
 * port.
 *
 * Where MAME has to guess, the core tells: the machine passes the real /M1
 * with every read, and every I/O cycle and every write to $C000 reaches the
 * snoop whether the cartridge is enabled or not (the edge sees the whole
 * bus; MAME needs memory taps for that).
 *
 * Like the cartridge, the mailbox service runs on its own thread (the
 * RP2354B's core0) while the bus is served on the emulation thread (core1),
 * so a slow network transaction never stalls the picture; a synchronous
 * mode (FUJINET_SYNC=1, or the sync flag) runs it inline instead, which is
 * MAME's behaviour and deterministic for tests.
 *
 * Single instance per process: fujimail's port interface is C function
 * pointers with no context argument (the constraint is real hardware's
 * too -- one cart slot, one cart).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMS_FUJINET_CART_H
#define SMS_FUJINET_CART_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sms_cart sms_cart_t;

/* Create the (single) cart; NULL if one already exists. */
sms_cart_t *sms_cart_create(void);
void sms_cart_destroy(sms_cart_t *c);

/* Power on with `image` (NULL: the baked CONFIG client), as MAME's
 * device_start + first device_reset with -cart <image>:
 *   - a claimed client of at most 32K becomes the RESIDENT image;
 *   - anything else goes straight into the SRAM, served from power-on
 *     (direct boot), and FN_HOT_CONFIG then reaches the baked CONFIG.
 * `cfg_mapper` overrides the mapper choice (a MAME slot name, or NULL).
 * `boip` is fujinet-pc's BoIP listener ("host:port"). Returns 0, or -1 with
 * the reason (an image the cartridge cannot map) in `why`. */
int sms_cart_power_on(sms_cart_t *c, const uint8_t *image, uint32_t len,
                      const char *cfg_mapper, const char *boip, bool sync,
                      char *why, int whysz);
/* Power off: stop the mailbox worker, close the link. */
void sms_cart_power_off(sms_cart_t *c);
/* The console's /RESET (device_reset after the first): back to CONFIG, or
 * a direct-booted image restarts; the mailbox's state survives. */
void sms_cart_console_reset(sms_cart_t *c);

/* ---- the bus (the machine calls these only with the cart enabled) ---- */
uint8_t sms_cart_read(sms_cart_t *c, uint16_t a, bool m1, bool commit);
void sms_cart_write(sms_cart_t *c, uint16_t a, uint8_t d);       /* $0000-$BFFF */
void sms_cart_write_mapper(sms_cart_t *c, uint16_t a, uint8_t d); /* $FFFC-$FFFF */

/* ---- the snoop (every cycle, enabled or not) ---- */
void sms_cart_snoop_c000(sms_cart_t *c, uint8_t d);
void sms_cart_snoop_iowrite(sms_cart_t *c, uint8_t port, uint8_t d);
void sms_cart_snoop_ioread(sms_cart_t *c, uint8_t port);

/* Bring what the mailbox worker published into the arena (also done lazily
 * on arena reads); the host calls it once a frame. */
void sms_cart_frame(sms_cart_t *c);

/* ---- status, for the session and the debugger ---- */
typedef struct {
    bool powered;
    bool link_up;             /* the BoIP socket is connected */
    bool busy;                /* a FujiBus transaction is in flight */
    bool direct;              /* an opened image, direct-booted */
    bool resident_client;     /* an opened FujiNet client (<= 32K, claimed)
                                 served in CONFIG's place */
    bool booted_game;         /* anything other than CONFIG is in the SRAM */
    int mode;                 /* FN_MODE_* */
    int mapper;               /* SMSMAP_* of the live map (GAME/APP) */
    const char *mapper_name;
    uint8_t bank[6];          /* the live map's banks */
    uint8_t ram_en, ram_we;
    uint32_t image_size, image_crc, ram_size;
    bool claim;
    int load_state;           /* FN_LOAD_* as published */
    int load_win, load_nwin, load_pct;
    int boot_state, boot_pct, boot_err;
    uint32_t boot_got, boot_total;
    uint8_t ackseq, status, err, reply_cmd;
    uint16_t rxlen;
    bool bios_phase;
    uint8_t snoop_c000, snoop_3e, snoop_3f;
    uint8_t snoop_vdp[11];
    unsigned queue;           /* hotspot events waiting for the worker */
} sms_cart_status_t;

void sms_cart_status(sms_cart_t *c, sms_cart_status_t *out);

/* The live map's page table, for bank-aware debugger symbols: the 8K SRAM
 * bank behind each 1K page of $0000-$BFFF, or -1 where the cart serves its
 * own memory (CONFIG, the arena, the load window). */
void sms_cart_page_banks(sms_cart_t *c, int16_t out[48]);

/* Debugger memory access to what the cart serves: peek is side-effect
 * free; poke writes the backing store (the SRAM, the resident image or the
 * arena) without firing a hotspot. Return 1 if the address is the cart's. */
int sms_cart_peek(sms_cart_t *c, uint16_t a, uint8_t *out);
int sms_cart_poke(sms_cart_t *c, uint16_t a, uint8_t d);

/* Copy the SRAM (1 MB), the arena (4K) or the resident image (32K) for the
 * debugger's Save. Return bytes copied. */
uint32_t sms_cart_copy_sram(sms_cart_t *c, uint8_t *dst, uint32_t max);
uint32_t sms_cart_copy_arena(sms_cart_t *c, uint8_t *dst, uint32_t max);

/* 1 if `image` (len bytes) can run on the cartridge; else 0 and why. */
int sms_cart_check_image(const uint8_t *image, uint32_t len,
                         const char *cfg_mapper, char *why, int whysz);

#ifdef __cplusplus
}
#endif

#endif /* SMS_FUJINET_CART_H */
