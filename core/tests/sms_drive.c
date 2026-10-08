/*
 * sms_drive -- drive the whole app headless, like a user: a session with the
 * in-process FujiNet, a script of presses and waits, and screenshots. For
 * exploring CONFIG and checking a flow end to end; built, not registered.
 *
 *   sms_drive [--cart image] [--import-sd file]... [--console n] SCRIPT
 *
 * SCRIPT is a sequence of words:
 *   wait:MS            let the machine run
 *   press:TARGET[:MS]  hold a control (default 100 ms), then release it;
 *                      TARGET is up, down, left, right, 1, 2 (player 1),
 *                      pause or reset
 *   shot:NAME          write the current frame to NAME.ppm
 *   status             print the cartridge's status
 *   until:TEXT:MS      wait up to MS for the status to contain TEXT
 *   text               print the name table's tiles as text (mode 4, $3800)
 *
 * Settings and data go in a fresh tree under $TMPDIR.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "smssession.h"
#include "smsdebug.h"
#include "test_tmpdir.h"

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int target_of(const char *name)
{
    static const struct { const char *n; int t; } map[] = {
        { "up", SMS_TARGET_PORT(0, SMS_ACT_UP) }, { "down", SMS_TARGET_PORT(0, SMS_ACT_DOWN) },
        { "left", SMS_TARGET_PORT(0, SMS_ACT_LEFT) }, { "right", SMS_TARGET_PORT(0, SMS_ACT_RIGHT) },
        { "1", SMS_TARGET_PORT(0, SMS_ACT_1) }, { "2", SMS_TARGET_PORT(0, SMS_ACT_2) },
        { "pause", SMS_TARGET_SWITCH(SMS_SW_PAUSE) }, { "reset", SMS_TARGET_SWITCH(SMS_SW_RESET) },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(name, map[i].n))
            return map[i].t;
    return -1;
}

static void shot(smssession *s, const char *name)
{
    static uint32_t px[SMSSESSION_FB_WIDTH * SMSSESSION_FB_MAX_HEIGHT];
    uint64_t serial = 0;
    int h = 0;
    char path[512];
    FILE *f;

    smssession_copy_frame(s, px, &h, &serial);
    snprintf(path, sizeof path, "%s.ppm", name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", SMSSESSION_FB_WIDTH, h);
    for (int i = 0; i < SMSSESSION_FB_WIDTH * h; i++)
    {
        uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("shot %s\n", path);
}

int main(int argc, char **argv)
{
    char cfg[512], data[512], st[160], dest[1200];
    const char *cart = NULL;
    smssession_paths p;
    smssession *s;
    smssession_start_opts o;
    int console = -1, i;

    test_tmpdir(cfg, sizeof cfg, "drcfg");
    test_tmpdir(data, sizeof data, "drdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    s = smssession_new(&p);
    if (!s)
        return 1;

    for (i = 1; i < argc - 1; i++)
    {
        if (!strcmp(argv[i], "--cart"))
            cart = argv[++i];
        else if (!strcmp(argv[i], "--console"))
            console = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--import-sd"))
            i++;   /* after start: the SD tree exists once FujiNet is up */
        else
            break;
    }

    smssession_default_opts(s, &o);
    o.enable_audio = 0;
    o.enable_gamepad = 0;
    o.cart_path = cart;
    if (console >= 0)
        o.console = console;
    if (smssession_start(s, &o) != 0)
    {
        fprintf(stderr, "start: %s\n", smssession_last_error(s));
        return 1;
    }
    for (int k = 1; k < argc - 1; k++)
        if (!strcmp(argv[k], "--import-sd"))
        {
            if (smssession_import_cart_to_sd(s, argv[k + 1], dest, sizeof dest) == 0)
                printf("imported %s\n", dest);
            else
                printf("import failed: %s\n", smssession_last_error(s));
            k++;
        }

    {
        char *script = strdup(argv[argc - 1]);
        for (char *w = strtok(script, " \n"); w; w = strtok(NULL, " \n"))
        {
            if (!strncmp(w, "wait:", 5))
                sleep_ms(atoi(w + 5));
            else if (!strncmp(w, "press:", 6))
            {
                char name[32];
                int ms = 100, t;
                const char *c = strchr(w + 6, ':');
                snprintf(name, sizeof name, "%.*s", c ? (int)(c - (w + 6)) : (int)strlen(w + 6), w + 6);
                if (c)
                    ms = atoi(c + 1);
                t = target_of(name);
                if (t < 0) { fprintf(stderr, "no target %s\n", name); continue; }
                smssession_press(s, t, 1);
                sleep_ms(ms);
                smssession_press(s, t, 0);
                sleep_ms(60);
            }
            else if (!strncmp(w, "shot:", 5))
                shot(s, w + 5);
            else if (!strcmp(w, "text"))
            {
                smsdebug *d = smssession_debugger(s);
                uint8_t nt[28 * 64];
                smsdebug_vram_read(d, 0x3800, nt, sizeof nt);
                for (int r = 0; r < 28; r++)
                {
                    char line[33];
                    for (int c = 0; c < 32; c++)
                    {
                        int t = nt[r * 64 + c * 2] | ((nt[r * 64 + c * 2 + 1] & 1) << 8);
                        line[c] = (t >= 0x20 && t < 0x7f) ? (char)t : '.';
                    }
                    line[32] = '\0';
                    printf("%2d [%s]", r, line);
                    for (int c = 0; c < 8; c++)
                        printf(" %03X", nt[r * 64 + c * 2] | ((nt[r * 64 + c * 2 + 1] & 1) << 8));
                    printf("\n");
                }
            }
            else if (!strcmp(w, "status"))
            {
                smssession_cart_status(s, st, sizeof st);
                printf("status: %s (booted %d)\n", st, smssession_cart_booted_game(s));
            }
            else if (!strncmp(w, "until:", 6))
            {
                char text[64];
                const char *c = strrchr(w + 6, ':');
                int ms = c ? atoi(c + 1) : 5000, waited = 0;
                snprintf(text, sizeof text, "%.*s", c ? (int)(c - (w + 6)) : (int)strlen(w + 6), w + 6);
                for (;;)
                {
                    smssession_cart_status(s, st, sizeof st);
                    if (strstr(st, text) || waited >= ms)
                        break;
                    sleep_ms(50);
                    waited += 50;
                }
                printf("until %s: %s after %d ms\n", text, st, waited);
            }
        }
        free(script);
    }
    smssession_stop(s);
    smssession_free(s);
    return 0;
}
