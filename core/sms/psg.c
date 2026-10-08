// license:BSD-3-Clause
// copyright-holders:Nicola Salmoria
/* psg.c -- the VDP's PSG: MAME's SEGAPSG variant of the SN76496.
 *
 * Transposed from MAME src/devices/sound/sn76496.cpp (BSD-3-Clause; see
 * COMPLIANCE.md): segapsg_device is sn76496_base_device with feedback mask
 * 0x8000, white-noise taps 0x01 and 0x08, negated output, mono, a clock
 * divider of 8 and the Sega-style register defaults. Only that variant is
 * kept. MAME streams the chip at clock/2 and clocks it once per stream
 * sample; the VDP gives it the CPU clock, so here one stream sample is two
 * CPU ticks.
 */

#include <string.h>

#include "sms_internal.h"

#define FEEDBACK_MASK   0x8000
#define WHITENOISE_TAP1 0x01
#define WHITENOISE_TAP2 0x08
#define CLOCK_DIVIDER   8
#define MAX_OUTPUT      0x7fff

void sms_psg_init(sms_psg_t *p)
{
    memset(p, 0, sizeof *p);

    /* Sega VDP PSG defaults to selected period reg for 2nd channel */
    p->last_register = 3;
    for (int i = 0; i < 8; i += 2)
    {
        p->reg[i] = 0;
        p->reg[i + 1] = 0xf;   /* volume = 0xf (off) on reset, Sega style */
    }
    for (int i = 0; i < 4; i++)
    {
        p->output[i] = 0;
        p->count[i] = 0;
        p->period[i] = 0;      /* Sega style: 0, not 0x400 */
    }
    p->RNG = FEEDBACK_MASK;
    p->output[3] = p->RNG & 1;
    p->current_clock = CLOCK_DIVIDER - 1;

    /* build volume table (2dB per step); four channels, each gets 1/4 of
     * the total range */
    double out = MAX_OUTPUT / 4;
    for (int i = 0; i < 15; i++)
    {
        /* limit volume to avoid clipping */
        if (out > MAX_OUTPUT / 4)
            p->vol_table[i] = MAX_OUTPUT / 4;
        else
            p->vol_table[i] = (int32_t)out;
        out /= 1.258925412;   /* = 10 ^ (2/20) = 2dB */
    }
    p->vol_table[15] = 0;

    for (int i = 0; i < 4; i++)
        p->volume[i] = p->vol_table[p->reg[i * 2 + 1]];
}

void sms_psg_write(sms_psg_t *p, uint8_t data)
{
    int r;

    if (data & 0x80)
    {
        r = (data & 0x70) >> 4;
        p->last_register = r;
        p->reg[r] = (p->reg[r] & 0x3f0) | (data & 0x0f);
    }
    else
    {
        r = p->last_register;
    }

    const int c = r >> 1;
    switch (r)
    {
    case 0: /* tone 0: frequency */
    case 2: /* tone 1: frequency */
    case 4: /* tone 2: frequency */
        if ((data & 0x80) == 0)
            p->reg[r] = (p->reg[r] & 0x0f) | ((data & 0x3f) << 4);
        p->period[c] = p->reg[r];   /* Sega style: 0 is a period of 0 */
        if (r == 4)
        {
            /* update noise shift frequency */
            if ((p->reg[6] & 0x03) == 0x03)
                p->period[3] = p->period[2] << 1;
        }
        break;
    case 1: /* tone 0: volume */
    case 3: /* tone 1: volume */
    case 5: /* tone 2: volume */
    case 7: /* noise: volume */
        p->volume[c] = p->vol_table[data & 0x0f];
        if ((data & 0x80) == 0)
            p->reg[r] = (p->reg[r] & 0x3f0) | (data & 0x0f);
        break;
    case 6: /* noise: frequency, mode */
    {
        if ((data & 0x80) == 0)
            p->reg[r] = (p->reg[r] & 0x3f0) | (data & 0x0f);
        const int n = p->reg[6];
        /* N/512,N/1024,N/2048,Tone #3 output */
        p->period[3] = ((n & 3) == 3) ? (p->period[2] << 1) : (1 << (5 + (n & 3)));
        p->RNG = FEEDBACK_MASK;
        break;
    }
    }
}

/* One MAME stream sample (sound_stream_update's loop body). */
static void sample(sms_psg_t *p)
{
    /* clock chip once */
    if (p->current_clock > 0)
    {
        /* not ready for new divided clock */
        p->current_clock--;
    }
    else
    {
        /* ready for new divided clock, make a new sample */
        p->current_clock = CLOCK_DIVIDER - 1;

        /* handle channels 0,1,2 */
        for (int i = 0; i < 3; i++)
        {
            p->count[i]--;
            if (p->count[i] <= 0)
            {
                p->output[i] ^= 1;
                p->count[i] = p->period[i];
            }
        }

        /* handle channel 3 */
        p->count[3]--;
        if (p->count[3] <= 0)
        {
            /* if noisemode is 1, both taps are enabled; if noisemode is 0,
             * the lower tap, whitenoisetap2, is held at 0 */
            if (((p->RNG & WHITENOISE_TAP1) != 0) !=
                (((p->RNG & WHITENOISE_TAP2) != 0) && ((p->reg[6] >> 2) & 1)))
            {
                p->RNG >>= 1;
                p->RNG |= FEEDBACK_MASK;
            }
            else
            {
                p->RNG >>= 1;
            }
            p->output[3] = p->RNG & 1;
            p->count[3] = p->period[3];
        }
    }

    int32_t out = ((p->output[0] != 0) ? p->volume[0] : 0) +
                  ((p->output[1] != 0) ? p->volume[1] : 0) +
                  ((p->output[2] != 0) ? p->volume[2] : 0) +
                  ((p->output[3] != 0) ? p->volume[3] : 0);
    p->sample = (int16_t)(-out);   /* negate */
}

void sms_psg_tick(sms_psg_t *p)
{
    if (++p->phase >= 2)
    {
        p->phase = 0;
        sample(p);
    }
}
