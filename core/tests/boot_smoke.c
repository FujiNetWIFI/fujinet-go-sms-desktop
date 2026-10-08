/*
 * boot_smoke -- the paced host boots CONFIG and keeps real time.
 *
 * Starts the host thread (core/sms/host.h) BIOS-less on an NTSC and a PAL
 * console, waits for frames, and checks that CONFIG painted (MAME's visible
 * area, 268x224 / 268x240, with something other than the backdrop on it)
 * and that the frame rate is the machine's own -- 59.92 Hz and 49.70 Hz
 * within 2%. The siblings' hardest-won lesson: verify the throttle (one
 * once free-ran at >33000%).
 *
 * The rate is only held to 2% where the OS can sleep for a frame without
 * gross overshoot: a loaded CI virtual machine (the macOS runners above
 * all) turns a 16.7 ms sleep into 50, which says nothing about the pacing
 * code. There the test checks what still means something -- the machine
 * is throttled, and the deadline ladder sleeps -- and says why, as the
 * Atari 2600 sibling's host test does.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"

static int failures;
static int accurate_sleeps = 1;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void run(sms_model_t model, double want_hz, int want_height)
{
    static uint32_t fb[SMS_FB_WIDTH * SMS_FB_MAX_HEIGHT];
    sms_host_opts_t o;
    uint64_t serial = 0, f0, f1;
    int height = 0, colours = 0;
    double t0, t1, hz;
    uint32_t first;
    char what[160];

    memset(&o, 0, sizeof o);
    o.model = model;
    o.boip_hostport = "127.0.0.1:1";   /* no FujiNet: CONFIG runs link-down */
    if (sms_host_start(&o) != 0)
    {
        printf("FAIL: start: %s\n", sms_host_last_error());
        failures++;
        return;
    }

    /* CONFIG turns the display on after its first link attempts */
    for (int waited = 0; waited < 5000 && colours <= 1000; waited += 100)
    {
        sleep_ms(100);
        if (!sms_host_frame_copy(fb, &height, &serial))
            continue;
        colours = 0;
        first = fb[0];
        for (int i = 0; i < SMS_FB_WIDTH * height; i++)
            if (fb[i] != first)
                colours++;
    }
    check(serial != 0, "a frame was published");
    snprintf(what, sizeof what, "%s: the frame is %d lines", sms_models[model].id, want_height);
    check(height == want_height, what);
    snprintf(what, sizeof what, "%s: CONFIG painted (%d pixels off the backdrop)", sms_models[model].id, colours);
    check(colours > 1000, what);

    f0 = sms_host_frame_count();
    t0 = now_s();
    sleep_ms(2000);
    f1 = sms_host_frame_count();
    t1 = now_s();
    hz = (double)(f1 - f0) / (t1 - t0);
    snprintf(what, sizeof what, "%s reports %.4f Hz", sms_models[model].id, sms_host_frame_rate());
    check(sms_host_frame_rate() > want_hz - 0.01 && sms_host_frame_rate() < want_hz + 0.01, what);

    sms_host_stop();
    {
        sms_host_pacing_t pc;
        sms_host_pacing(&pc);
        printf("  pacing: %llu sleeps, %.2f ms late on average, worst %.2f ms, %llu resyncs\n",
               (unsigned long long)pc.sleeps,
               pc.sleeps ? (double)pc.late_ns / (double)pc.sleeps / 1e6 : 0.0,
               (double)pc.worst_late_ns / 1e6, (unsigned long long)pc.resyncs);
        snprintf(what, sizeof what, "%s is throttled: %.2f Hz, not free-running", sms_models[model].id, hz);
        check(hz < want_hz * 1.10 && pc.sleeps > 0, what);
        snprintf(what, sizeof what, "%s paces at %.2f Hz (want %.2f)", sms_models[model].id, hz, want_hz);
        if (accurate_sleeps)
            check(hz > want_hz * 0.98 && hz < want_hz * 1.02, what);
        else
            printf("note: %s -- not asserted, this OS's sleeps are coarse\n", what);
    }
}

/* How fast this machine runs the core with no host and no throttle: the
 * pacing check means nothing on a runner that cannot keep up. */
static double capacity(sms_model_t model)
{
    static sms_machine_t m;
    char why[160];
    sms_cart_t *c = sms_cart_create();
    double t0, t1;
    const int frames = 120;

    sms_cart_power_on(c, NULL, 0, NULL, "127.0.0.1:1", true, why, sizeof why);
    sms_machine_init(&m, model, NULL, 0, NULL, false, false, c);
    t0 = now_s();
    for (int f = 0; f < frames; f++)
    {
        sms_machine_run_frame(&m);
        sms_machine_latch_inputs(&m);
    }
    t1 = now_s();
    sms_machine_free(&m);
    sms_cart_destroy(c);
    return frames / (t1 - t0);
}

int main(void)
{
    /* ten 16.7 ms sleeps: 167 ms on a machine that can sleep for a frame */
    {
        double t0 = now_s(), took;
        for (int i = 0; i < 10; i++)
            nanosleep(&(struct timespec){ 0, 16666667L }, NULL);
        took = now_s() - t0;
        accurate_sleeps = took < 0.25;
        printf("  ten 16.7 ms sleeps took %.3f s (%s)\n", took,
               accurate_sleeps ? "accurate" : "coarse: the rate is reported, not asserted");
    }
    /* before the host exists: the cart device is one per process */
    printf("  the bare core runs %.0f frames/s (NTSC) and %.0f (PAL) here\n",
           capacity(SMS_MODEL_SMS1), capacity(SMS_MODEL_SMS2_PAL));
    run(SMS_MODEL_SMS1, 59.9227, 224);
    run(SMS_MODEL_SMS2_PAL, 49.7015, 240);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
