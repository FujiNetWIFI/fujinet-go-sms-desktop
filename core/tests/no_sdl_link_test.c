/*
 * no_sdl_link_test -- the core links and runs on its own: no SDL, no
 * session, no frontend. Links sms_core alone (a session symbol would fail
 * the link), runs the staged wire codec's self test, plans the baked CONFIG
 * client with the staged mapper engine, and boots a machine for a frame.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "sms_internal.h"
#include "fujibus.h"
#include "smsmap.h"
#include "fujiconfigrom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    static sms_machine_t m;
    smsmap_plan_t plan;
    char why[160];
    sms_cart_t *c;

    check(fujibus_selftest(), "the staged fujibus codec passes its self test");
    check(smsmap_plan(_configrom, FUJI_CONFIGROM_SIZE, NULL, &plan) == SMSMAP_OK,
          "the staged smsmap plans the baked CONFIG client");
    check(plan.claim && plan.header && plan.size == 0x8000, "CONFIG carries the claim and a Sega header");

    c = sms_cart_create();
    check(c != NULL, "the cartridge device creates");
    check(sms_cart_power_on(c, NULL, 0, NULL, "127.0.0.1:1", true, why, sizeof why) == 0,
          "it powers on with CONFIG");
    sms_machine_init(&m, SMS_MODEL_SMS1, NULL, 0, NULL, false, false, c);
    sms_machine_run_frame(&m);
    check(m.vdp.frame_count == 1, "a BIOS-less Master System runs a frame");
    /* run_frame stops at the instruction boundary after VBLANK begins */
    check(m.cycles >= (uint64_t)SMS_CYCLES_PER_LINE * SMS_VDP_HEIGHT_NTSC &&
          m.cycles < (uint64_t)SMS_CYCLES_PER_LINE * SMS_VDP_HEIGHT_NTSC + 24,
          "in one frame's cycles (from power-on at VBLANK)");
    sms_machine_free(&m);
    sms_cart_destroy(c);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
