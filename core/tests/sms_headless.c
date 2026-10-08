/* sms_headless -- run a machine without a host thread and write frames.
 *
 * The A/B harness's side B (tools/ab/mame_ab.py; side A is MAME with
 * -bios none -slot fujinet): both start at VBLANK at power-on and publish a
 * frame at every VBLANK, so frame N here is MAME's frame N.
 *
 *   sms_headless [--model sms1|sms1pal|sms|smspal|smsj|sg1000m3]
 *                [--cart image] [--bios image] [--inst ym2413.bin] [--fm]
 *                [--boip host:port] [--frames N] [--every K] [--first F]
 *                [--out prefix] [--input script] [--wav file]
 *                [--dumpram file]
 *
 * Writes <prefix>NNNNN.ppm (P6, 268 x 224/240) for frames F, F+K, ... up to
 * N. --input reads lines "frame port mask" (mask in hex, SMS_PAD_* bits;
 * port 2 is the Pause button, 3 the Reset button), set at that frame's
 * boundary and latched at the next, as MAME's input ports take a value a
 * script sets from a frame notifier. --dumpram writes the 8K of work RAM
 * after the last frame (the timing probes' results).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_internal.h"

static uint8_t *slurp(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)(n > 0 ? n : 1));
    if (buf && n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (uint32_t)(n > 0 ? n : 0);
    return buf;
}

static int write_ppm(const char *path, const uint32_t *fb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++)
    {
        uint8_t px[3] = { (uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    return 0;
}

static sms_model_t model_by_id(const char *id)
{
    for (int i = 0; i < SMS_MODEL_COUNT; i++)
        if (strcmp(sms_models[i].id, id) == 0)
            return (sms_model_t)i;
    fprintf(stderr, "unknown model %s\n", id);
    exit(2);
}

typedef struct { long frame; int port; unsigned mask; } input_ev_t;

int main(int argc, char **argv)
{
    sms_model_t model = SMS_MODEL_SMS1;
    const char *cart_path = NULL, *bios_path = NULL, *inst_path = NULL;
    /* not 9995, fujitcp's default: that is a developer's standalone
     * fujinet-pc, and its listener takes one client */
    const char *boip = "127.0.0.1:1", *out = "frame", *input_path = NULL, *wav_path = NULL;
    const char *ram_path = NULL;
    long frames = 120, every = 0, first = 0;
    bool fm_unit = false;
    uint8_t *cart = NULL, *bios = NULL, *inst = NULL;
    uint32_t cart_len = 0, bios_len = 0, inst_len = 0;
    input_ev_t *events = NULL;
    size_t nev = 0, iev = 0;
    FILE *wav = NULL;
    uint32_t wav_frames = 0;
    char why[200];
    static sms_machine_t m;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--model") && v) { model = model_by_id(v); i++; }
        else if (!strcmp(a, "--cart") && v) { cart_path = v; i++; }
        else if (!strcmp(a, "--bios") && v) { bios_path = v; i++; }
        else if (!strcmp(a, "--inst") && v) { inst_path = v; i++; }
        else if (!strcmp(a, "--boip") && v) { boip = v; i++; }
        else if (!strcmp(a, "--frames") && v) { frames = atol(v); i++; }
        else if (!strcmp(a, "--every") && v) { every = atol(v); i++; }
        else if (!strcmp(a, "--first") && v) { first = atol(v); i++; }
        else if (!strcmp(a, "--out") && v) { out = v; i++; }
        else if (!strcmp(a, "--input") && v) { input_path = v; i++; }
        else if (!strcmp(a, "--wav") && v) { wav_path = v; i++; }
        else if (!strcmp(a, "--dumpram") && v) { ram_path = v; i++; }
        else if (!strcmp(a, "--fm")) fm_unit = true;
        else { fprintf(stderr, "bad argument %s\n", a); return 2; }
    }

    if (cart_path && !(cart = slurp(cart_path, &cart_len))) { perror(cart_path); return 1; }
    if (bios_path && !(bios = slurp(bios_path, &bios_len))) { perror(bios_path); return 1; }
    if (inst_path && !(inst = slurp(inst_path, &inst_len))) { perror(inst_path); return 1; }
    if (input_path)
    {
        FILE *f = fopen(input_path, "r");
        char line[128];
        if (!f) { perror(input_path); return 1; }
        while (fgets(line, sizeof line, f))
        {
            input_ev_t e;
            if (sscanf(line, "%ld %d %x", &e.frame, &e.port, &e.mask) == 3)
            {
                events = realloc(events, (nev + 1) * sizeof *events);
                events[nev++] = e;
            }
        }
        fclose(f);
    }
    if (wav_path)
    {
        wav = fopen(wav_path, "wb");
        if (!wav) { perror(wav_path); return 1; }
        fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x03\0\x02\0", 1, 24, wav);
        uint32_t rate = SMS_AUDIO_RATE, bps = SMS_AUDIO_RATE * 8;
        fwrite(&rate, 4, 1, wav);
        fwrite(&bps, 4, 1, wav);
        fwrite("\x08\0\x20\0data\0\0\0\0", 1, 12, wav);
    }

    sms_cart_t *c = sms_cart_create();
    if (sms_cart_power_on(c, cart, cart_len, NULL, boip, true, why, sizeof why) != 0)
    {
        fprintf(stderr, "cart: %s\n", why);
        return 1;
    }
    sms_machine_init(&m, model, bios, bios_len, inst, fm_unit, false, c);

    for (long f = 0; f < frames; f++)
    {
        sms_machine_run_frame(&m);
        sms_cart_frame(c);
        if (wav)
        {
            fwrite(m.mix.out, sizeof(float), (size_t)m.mix.out_frames * 2, wav);
            wav_frames += (uint32_t)m.mix.out_frames;
        }
        m.mix.out_frames = 0;

        const long n = f + 1;   /* frames completed */
        if (n >= first && (every ? ((n - first) % every == 0) : n == frames))
        {
            char path[512];
            snprintf(path, sizeof path, "%s%05ld.ppm", out, n);
            write_ppm(path, m.vdp.fb, SMS_FB_WIDTH, m.vdp.fb_height);
        }

        /* MAME's order at a frame boundary: the input ports latch first
         * (ioport's frame notifier is registered before any script's), then
         * a script's new values go in -- so they reach the machine at the
         * NEXT boundary. The same order here keeps the two sides in step. */
        sms_machine_latch_inputs(&m);
        while (iev < nev && events[iev].frame <= n)
        {
            const input_ev_t *e = &events[iev++];
            if (e->port == 0 || e->port == 1)
                m.pad_live[e->port] = (uint8_t)e->mask;
            else if (e->port == 2)
                m.pause_live = e->mask != 0;
            else if (e->port == 3)
                m.reset_live = e->mask != 0;
        }
    }

    if (ram_path)
    {
        FILE *f = fopen(ram_path, "wb");
        if (f)
        {
            fwrite(m.mainram, 1, sizeof m.mainram, f);
            fclose(f);
        }
    }

    if (wav)
    {
        uint32_t data = wav_frames * 8, riff = 36 + data;
        fseek(wav, 4, SEEK_SET);
        fwrite(&riff, 4, 1, wav);
        fseek(wav, 40, SEEK_SET);
        fwrite(&data, 4, 1, wav);
        fclose(wav);
    }

    sms_machine_free(&m);
    sms_cart_destroy(c);
    free(cart);
    free(bios);
    free(inst);
    free(events);
    return 0;
}
