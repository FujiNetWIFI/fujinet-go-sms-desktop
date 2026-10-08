/*
 * vdp_timing_test -- the calibration against MAME, on every build.
 *
 * Runs the timing probes (tools/ab/probes: the V and H counters read at
 * every phase of a line, and the beam position at every interrupt entry)
 * on each model for PROBE_FRAMES frames and compares the work RAM with
 * what MAME's own Master System left (core/tests/probe_golden.h, captured
 * by tools/ab/make_goldens.py). A one-cycle error in the VDP's event
 * engine, an access skew or the interrupt look-ahead shows up here as a
 * changed byte.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_internal.h"
#include "probe_golden.h"

static sms_model_t model_by_id(const char *id)
{
    for (int i = 0; i < SMS_MODEL_COUNT; i++)
        if (strcmp(sms_models[i].id, id) == 0)
            return (sms_model_t)i;
    return SMS_MODEL_COUNT;
}

/* code, $FF to $7FF0, "TMR SEGA", zeros to 32K (see make_goldens.py) */
static uint8_t *probe_image(const uint8_t *code, unsigned n)
{
    uint8_t *img = malloc(0x8000);
    memset(img, 0xff, 0x7ff0);
    memcpy(img, code, n);
    memcpy(img + 0x7ff0, "TMR SEGA", 8);
    memset(img + 0x7ff8, 0x00, 8);
    return img;
}

int main(void)
{
    static sms_machine_t m;
    int failures = 0;
    char why[160];

    for (unsigned i = 0; i < sizeof probe_goldens / sizeof probe_goldens[0]; i++)
    {
        const probe_golden_t *g = &probe_goldens[i];
        const sms_model_t model = model_by_id(g->model);
        uint8_t *img = probe_image(g->code, g->code_size);
        sms_cart_t *c = sms_cart_create();
        int bad = 0, first = -1;

        if (sms_cart_power_on(c, img, 0x8000, NULL, "127.0.0.1:1", true, why, sizeof why) != 0)
        {
            printf("FAIL: %s on %s: %s\n", g->probe, g->model, why);
            failures++;
            continue;
        }
        sms_machine_init(&m, model, NULL, 0, NULL, false, false, c);
        for (int f = 0; f < PROBE_FRAMES; f++)
        {
            sms_machine_run_frame(&m);
            sms_machine_latch_inputs(&m);
        }
        for (unsigned k = 0; k < g->ram_size; k++)
        {
            if (m.mainram[(g->ram_addr - 0xc000) + k] != g->ram[k])
            {
                if (first < 0)
                    first = (int)k;
                bad++;
            }
        }
        if (bad)
        {
            printf("FAIL: %s on %s: %d bytes differ from MAME, first at $%04X (MAME %02X, core %02X)\n",
                   g->probe, g->model, bad, g->ram_addr + first, g->ram[first],
                   m.mainram[(g->ram_addr - 0xc000) + first]);
            failures++;
        }
        else
            printf("ok: %s on %s matches MAME (%u bytes)\n", g->probe, g->model, g->ram_size);
        sms_machine_free(&m);
        sms_cart_destroy(c);
        free(img);
    }
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
