/*
 * bios_synth_test -- the BIOS socket, the memory control port and the
 * cartridge's BIOS snoop, with BIOS images this test writes itself (no
 * Sega firmware is needed or used).
 *
 *   - A hand-over like a real BIOS's: it writes $C000 and a VDP register,
 *     reads with the BIOS and the cartridge both enabled (MAME ANDs the
 *     two), reads $0000 as data, copies a stub to RAM that turns itself off
 *     through $3E and jumps to $0000. The cartridge's snoop must hold what
 *     the BIOS left, and its BIOS phase must end on the /M1 fetch of $0000
 *     -- not on the data read before it.
 *   - Paging: a 128K BIOS on the Master System banks through $FFFF (and the
 *     write lands in RAM's $DFFF mirror); the Japanese console's BIOS does
 *     not page.
 *   - $3E's I/O-chip and work-RAM disables, from a BIOS-less cartridge.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_internal.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static sms_machine_t m;

static sms_cart_t *boot(sms_model_t model, const uint8_t *bios, uint32_t bios_size,
                        const uint8_t *cart, uint32_t cart_size, int frames)
{
    char why[160];
    sms_cart_t *c = sms_cart_create();
    if (sms_cart_power_on(c, cart, cart_size, NULL, "127.0.0.1:1", true, why, sizeof why) != 0)
    {
        printf("cart: %s\n", why);
        exit(1);
    }
    sms_machine_init(&m, model, bios, bios_size, NULL, false, false, c);
    for (int f = 0; f < frames; f++)
    {
        sms_machine_run_frame(&m);
        sms_machine_latch_inputs(&m);
    }
    return c;
}

static void done(sms_cart_t *c)
{
    sms_machine_free(&m);
    sms_cart_destroy(c);
}

static void handover(void)
{
    static const uint8_t prog[] = {
        0xF3,                   /* 0000 di */
        0x31, 0xF0, 0xDF,       /*      ld sp,$dff0 */
        0x3E, 0xAB,             /*      ld a,$ab */
        0x32, 0x00, 0xC0,       /*      ld ($c000),a      the snooped byte */
        0x3E, 0xA0,             /*      ld a,$a0 */
        0xD3, 0xBF,             /*      out ($bf),a */
        0x3E, 0x81,             /*      ld a,$81 */
        0xD3, 0xBF,             /*      out ($bf),a       VDP R1 = $A0 */
        0x3E, 0xA3,             /*      ld a,$a3          cartridge AND BIOS on */
        0xD3, 0x3E,             /*      out ($3e),a */
        0x3A, 0x00, 0x04,       /*      ld a,($0400)      BIOS $F0 & cart $3C */
        0x32, 0x01, 0xC1,       /*      ld ($c101),a */
        0x3A, 0x00, 0x00,       /*      ld a,($0000)      data, not /M1 */
        0x32, 0x02, 0xC1,       /*      ld ($c102),a */
        0x21, 0x30, 0x00,       /*      ld hl,stub */
        0x11, 0x00, 0xC7,       /*      ld de,$c700 */
        0x01, 0x07, 0x00,       /*      ld bc,7 */
        0xED, 0xB0,             /*      ldir */
        0xC3, 0x00, 0xC7,       /*      jp $c700 */
    };
    static const uint8_t stub[] = {
        0x3E, 0xAB,             /* ld a,$ab   cartridge on, BIOS off */
        0xD3, 0x3E,             /* out ($3e),a */
        0xC3, 0x00, 0x00,       /* jp $0000   the /M1 fetch that ends the phase */
    };
    static const uint8_t cart_prog[] = {
        0x3E, 0x5A,             /* ld a,$5a */
        0x32, 0x00, 0xC1,       /* ld ($c100),a */
        0x18, 0xFE,             /* jr $ */
    };
    uint8_t *bios = calloc(1, 0x2000), *cart = malloc(0x8000);
    sms_cart_status_t st;
    sms_cart_t *c;

    memcpy(bios, prog, sizeof prog);
    memcpy(bios + 0x30, stub, sizeof stub);
    bios[0x400] = 0xF0;
    memset(cart, 0xFF, 0x8000);
    memcpy(cart, cart_prog, sizeof cart_prog);
    cart[0x400] = 0x3C;

    c = boot(SMS_MODEL_SMS1, bios, 0x2000, cart, 0x8000, 3);
    sms_cart_status(c, &st);
    check(m.mainram[0x100] == 0x5A, "the BIOS handed over and the cartridge ran");
    check(m.mainram[0x101] == 0x30, "BIOS and cartridge both enabled: their bytes are ANDed");
    check(m.mainram[0x102] == (0xF3 & 0x3E), "a data read of $0000 reads both too");
    check(!st.bios_phase, "the BIOS phase ended (on the /M1 fetch of $0000)");
    check(st.snoop_c000 == 0xAB, "the snoop kept the BIOS's $C000 byte");
    check(st.snoop_3e == 0xAB, "and its last port $3E value");
    check(st.snoop_vdp[1] == 0xA0, "and the VDP register it wrote");
    check(m.mem_ctrl_reg == 0xAB, "the console's $3E is the stub's");
    done(c);

    /* the same BIOS, stopped before the hand-over: the data read alone must
     * not end the phase */
    bios[0x21] = 0x18;          /* jr $ instead of the copy */
    bios[0x22] = 0xFE;
    c = boot(SMS_MODEL_SMS1, bios, 0x2000, cart, 0x8000, 2);
    sms_cart_status(c, &st);
    check(st.bios_phase, "reading $0000 as data does not end the BIOS phase");
    done(c);
    free(bios);
    free(cart);
}

static void paging(void)
{
    static const uint8_t prog[] = {
        0xF3, 0x31, 0xF0, 0xDF, /* di; ld sp,$dff0 */
        0x3E, 0x05,             /* ld a,5 */
        0x32, 0xFF, 0xFF,       /* ld ($ffff),a   page BIOS bank 5 into slot 2 */
        0x3A, 0x00, 0x80,       /* ld a,($8000) */
        0x32, 0x00, 0xC1,       /* ld ($c100),a */
        0x3A, 0xFF, 0xDF,       /* ld a,($dfff)   the mapper write's RAM copy */
        0x32, 0x01, 0xC1,       /* ld ($c101),a */
        0x18, 0xFE,             /* jr $ */
    };
    uint8_t *bios = calloc(1, 0x20000);
    sms_cart_t *c;

    memcpy(bios, prog, sizeof prog);
    bios[0x14000] = 0x77;
    c = boot(SMS_MODEL_SMS1, bios, 0x20000, NULL, 0, 2);
    check(m.mainram[0x100] == 0x77, "a 128K BIOS pages through $FFFF");
    check(m.mainram[0x101] == 0x05, "the mapper write lands in RAM at $DFFF");
    done(c);

    /* the Japanese console's 8K BIOS is not paged: slot 2 still shows
     * page 0 of its single 16K page */
    c = boot(SMS_MODEL_SMSJ, bios, 0x2000, NULL, 0, 2);
    check(m.mainram[0x100] == 0xF3, "the Japanese console's BIOS does not page");
    done(c);
    free(bios);
}

static void mem_control(void)
{
    static const uint8_t prog[] = {
        0xF3, 0x31, 0xF0, 0xDF, /* di; ld sp,$dff0 */
        0x3E, 0xAF, 0xD3, 0x3E, /* ld a,$af; out ($3e),a   I/O chip off */
        0xDB, 0xDC,             /* in a,($dc) */
        0x32, 0x00, 0xC1,       /* ld ($c100),a */
        0x3E, 0xAB, 0xD3, 0x3E, /* I/O chip on */
        0xDB, 0xDC,             /* in a,($dc) */
        0x32, 0x01, 0xC1,       /* ld ($c101),a */
        0x3E, 0x12,             /* ld a,$12 */
        0x32, 0x00, 0xC2,       /* ld ($c200),a */
        0x3E, 0xBB, 0xD3, 0x3E, /* work RAM off */
        0x3A, 0x00, 0xC2,       /* ld a,($c200) */
        0x47,                   /* ld b,a */
        0x3E, 0xAB, 0xD3, 0x3E, /* work RAM on */
        0x78,                   /* ld a,b */
        0x32, 0x02, 0xC1,       /* ld ($c102),a */
        0x18, 0xFE,             /* jr $ */
    };
    uint8_t *cart = malloc(0x8000);
    sms_cart_t *c;
    char why[160];

    memset(cart, 0xFF, 0x8000);
    memcpy(cart, prog, sizeof prog);
    c = sms_cart_create();
    sms_cart_power_on(c, cart, 0x8000, NULL, "127.0.0.1:1", true, why, sizeof why);
    sms_machine_init(&m, SMS_MODEL_SMS1, NULL, 0, NULL, false, false, c);
    m.pad_live[0] = SMS_PAD_UP;
    sms_machine_latch_inputs(&m);
    for (int f = 0; f < 2; f++)
        sms_machine_run_frame(&m);
    check(m.mainram[0x100] == 0xFF, "$3E bit 2: the I/O chip off reads $FF");
    check(m.mainram[0x101] == 0xFE, "the I/O chip on reads the joypad (Up held)");
    check(m.mainram[0x102] == 0xFF, "$3E bit 4: work RAM off reads $FF");
    check(m.mainram[0x200] == 0x12, "and work RAM kept its byte");
    done(c);
    free(cart);
}

int main(void)
{
    handover();
    paging();
    mem_control();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
