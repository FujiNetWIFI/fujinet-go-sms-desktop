/*
 * cart_seam_test -- the seam between the console and the FujiNet cartridge
 * (core/sms/fujinet_cart.h), with images this test writes itself:
 *
 *   - routing: a Sega-mapped image banks through $FFFD-$FFFF (with the
 *     first 1K fixed), a Codemasters one through writes to its ROM space,
 *     and the machine's $FFFC-$FFFF writes reach both the cart and RAM's
 *     $DFFC-$DFFF copy;
 *   - modes: an unclaimed image runs as GAME (the arena is its own ROM), a
 *     claimed one as APP (the mailbox is at $B000);
 *   - the armed flip (FN_HOT_GO) happens only on a committed /M1 fetch of
 *     $0000 -- not on a data read, not on a debugger peek;
 *   - the console's /RESET re-plans a direct-booted image (banks back to
 *     power-on, BIOS snoop re-armed);
 *   - debugger peeks fire nothing: a peek of a hotspot changes no state.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_internal.h"
#include "fuji_mailbox.h"
#include "smsmap.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

/* `banks` 16K banks, each filled with its own number, a Sega header, and
 * optionally the FUJI claim */
static uint8_t *image(unsigned banks, int claim)
{
    uint8_t *img = malloc((size_t)banks * 0x4000);
    for (unsigned b = 0; b < banks; b++)
        memset(img + b * 0x4000, (int)b, 0x4000);
    memcpy(img + 0x7ff0, "TMR SEGA", 8);
    if (claim)
        memcpy(img + FN_CLAIM_OFFSET, FN_R_CLAIM_SIG, FN_R_CLAIM_LEN);
    return img;
}

static sms_cart_t *power(const uint8_t *img, uint32_t len, const char *cfg)
{
    char why[160];
    sms_cart_t *c = sms_cart_create();
    if (!c || sms_cart_power_on(c, img, len, cfg, "127.0.0.1:1", true, why, sizeof why) != 0)
    {
        printf("FAIL: power on: %s\n", c ? why : "create");
        exit(1);
    }
    return c;
}

static uint8_t rd(sms_cart_t *c, uint16_t a)
{
    return sms_cart_read(c, a, false, true);
}

static void sega_routing(void)
{
    uint8_t *img = image(16, 0);   /* 256K */
    sms_cart_t *c = power(img, 16 * 0x4000, NULL);
    sms_cart_status_t st;

    sms_cart_status(c, &st);
    check(st.direct && st.mode == FN_MODE_GAME, "an unclaimed image direct-boots as GAME");
    check(rd(c, 0x0000) == 0 && rd(c, 0x4000) == 1 && rd(c, 0x8000) == 2,
          "Sega mapper powers on with banks 0, 1, 2");
    check(rd(c, 0xB409) == 2, "GAME: the arena is the image's own ROM");

    sms_cart_write_mapper(c, 0xFFFF, 9);
    sms_cart_write_mapper(c, 0xFFFE, 7);
    sms_cart_write_mapper(c, 0xFFFD, 5);
    check(rd(c, 0x8000) == 9 && rd(c, 0xBFFF) == 9, "$FFFF banks slot 2");
    check(rd(c, 0x4000) == 7, "$FFFE banks slot 1");
    check(rd(c, 0x0400) == 5 && rd(c, 0x03FF) == 0, "$FFFD banks slot 0 above the fixed first 1K");

    sms_cart_console_reset(c);
    sms_cart_status(c, &st);
    check(rd(c, 0x0400) == 0 && rd(c, 0x4000) == 1 && rd(c, 0x8000) == 2,
          "/RESET re-plans the image: banks back to 0, 1, 2");
    check(st.bios_phase, "and re-arms the BIOS snoop");

    sms_cart_destroy(c);
    free(img);
}

static void codemasters_routing(void)
{
    uint8_t *img = image(8, 0);    /* 128K */
    sms_cart_t *c = power(img, 8 * 0x4000, "codemasters");
    sms_cart_status_t st;

    sms_cart_status(c, &st);
    check(st.mapper == SMSMAP_CODEMASTERS, "a .cfg override picks the Codemasters mapper");
    sms_cart_write(c, 0x8000, 6);
    check(rd(c, 0x8000) == 6, "Codemasters: a write to $8000 banks slot 2");
    sms_cart_write(c, 0x4000, 3);
    check(rd(c, 0x4000) == 3, "and to $4000 banks slot 1");
    sms_cart_write_mapper(c, 0xFFFF, 1);
    check(rd(c, 0x8000) == 6, "the Sega registers do nothing on it");
    sms_cart_destroy(c);
    free(img);
}

/* A claimed client of at most 32K is served RESIDENT, in CONFIG's place,
 * as the cartridge serves its own resident image -- and says so. */
static void resident_client(void)
{
    uint8_t *img = image(2, 1);    /* 32K, claimed */
    sms_cart_t *c = power(img, 2 * 0x4000, NULL);
    sms_cart_status_t st;

    sms_cart_status(c, &st);
    check(st.mode == FN_MODE_RESIDENT && !st.direct, "a claimed 32K client is served RESIDENT");
    check(st.resident_client && st.booted_game, "and reported as an opened client, not CONFIG");
    check(st.image_size == 0x8000 && st.claim, "with its size and claim");
    check(rd(c, 0x4000) == 1, "its own bytes are on the bus");
    sms_cart_destroy(c);
    free(img);

    c = power(NULL, 0, NULL);
    sms_cart_status(c, &st);
    check(!st.resident_client && !st.booted_game, "CONFIG itself is not a client");
    sms_cart_destroy(c);
}

static void app_and_flip(void)
{
    uint8_t *img = image(4, 1);    /* 64K, claimed: direct-booted, not resident */
    sms_cart_t *c = power(img, 4 * 0x4000, NULL);
    sms_cart_status_t st;
    uint8_t v;

    sms_cart_status(c, &st);
    check(st.mode == FN_MODE_APP && st.claim, "a claimed image direct-boots as APP");
    check(rd(c, 0xB000 + FN_R_MAGIC0) == 'F' && rd(c, 0xB000 + FN_R_MAGIC1) == 'N',
          "APP: the mailbox answers at $B000");

    /* a debugger peek of a hotspot fires nothing */
    check(sms_cart_peek(c, 0xB500 + FN_HOT_SWAP, &v) == 1, "the cart owns $B5FE");
    check(sms_cart_peek(c, 0xC000, &v) == 0, "and not $C000");
    sms_cart_status(c, &st);
    check(st.mode == FN_MODE_APP, "a peek of a hotspot changes nothing");

    /* SWAP: back to RESIDENT (nothing staged, so the load fails) */
    sms_cart_write(c, 0xB500 + FN_HOT_SWAP, 0);
    sms_cart_status(c, &st);
    check(st.mode == FN_MODE_RESIDENT, "FN_HOT_SWAP drops to RESIDENT");

    /* GO arms the flip back to the image on the next /M1 fetch of $0000 */
    sms_cart_write(c, 0xB500 + FN_HOT_GO, 0);
    (void)sms_cart_read(c, 0x0000, false, true);
    sms_cart_status(c, &st);
    check(st.mode == FN_MODE_RESIDENT, "a data read of $0000 does not flip");
    (void)sms_cart_read(c, 0x0000, true, false);
    sms_cart_status(c, &st);
    check(st.mode == FN_MODE_RESIDENT, "an uncommitted /M1 (a debugger read) does not flip");
    (void)sms_cart_read(c, 0x0001, true, true);
    sms_cart_status(c, &st);
    check(st.mode == FN_MODE_RESIDENT, "an /M1 fetch elsewhere does not flip");
    (void)sms_cart_read(c, 0x0000, true, true);
    sms_cart_status(c, &st);
    check(st.mode == FN_MODE_APP, "the /M1 fetch of $0000 flips");
    check(!st.bios_phase, "and ends the BIOS phase");

    sms_cart_destroy(c);
    free(img);
}

/* The machine's side of the seam: $FFFC-$FFFF writes reach the cart and
 * RAM's $DFFC-$DFFF copy; reads of $C000-$FFFF never reach the cart. */
static void machine_mirror(void)
{
    static const uint8_t prog[] = {
        0xF3, 0x31, 0xF0, 0xDF, /* di; ld sp,$dff0 */
        0x3E, 0x03,             /* ld a,3 */
        0x32, 0xFF, 0xFF,       /* ld ($ffff),a */
        0x3A, 0x00, 0x80,       /* ld a,($8000)   bank 3 */
        0x32, 0x00, 0xC1,       /* ld ($c100),a */
        0x3A, 0xFF, 0xDF,       /* ld a,($dfff)   RAM's copy */
        0x32, 0x01, 0xC1,       /* ld ($c101),a */
        0x3A, 0xFF, 0xFF,       /* ld a,($ffff)   reads RAM too */
        0x32, 0x02, 0xC1,       /* ld ($c102),a */
        0x18, 0xFE,             /* jr $ */
    };
    static sms_machine_t m;
    uint8_t *img = image(8, 0);
    sms_cart_t *c;

    memcpy(img, prog, sizeof prog);
    c = power(img, 8 * 0x4000, NULL);
    sms_machine_init(&m, SMS_MODEL_SMS2, NULL, 0, NULL, false, false, c);
    sms_machine_run_frame(&m);
    check(m.mainram[0x100] == 3, "the machine's $FFFF write banks the cart");
    check(m.mainram[0x101] == 3, "and lands in RAM at $DFFF");
    check(m.mainram[0x102] == 3, "$FFFF reads back from RAM");
    sms_machine_free(&m);
    sms_cart_destroy(c);
    free(img);
}

int main(void)
{
    sega_routing();
    codemasters_routing();
    resident_client();
    app_and_flip();
    machine_mirror();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
