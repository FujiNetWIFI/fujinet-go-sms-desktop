/*
 * config_boot_test -- CONFIG, driven like a user, boots an image through the
 * in-process FujiNet. The test suite's own counter image goes to the SD host
 * (Import Cartridge to SD); then, on CONFIG's own screens: SELECT A HOST ->
 * 1 opens SD -> the listing shows the file -> Down, 1 (OPEN/BOOT) -> FujiNet
 * mounts it as ROM media (tools/fujinet/patches/0002) and pushes it -> the
 * loader page copies it into the SRAM and it runs (its counter moves). Reset
 * to CONFIG brings the host list back.
 *
 * CONFIG's screens are recognised by their text, read from the name table
 * (CONFIG's font puts character c at tile c, bit 8 selecting its highlight
 * colours); a splash that asks for button 1 is left to go on by itself.
 * Needs nothing but the FujiNet runtime; SKIPs (77) without it.
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
#include "test_rom.h"
#include "test_crash.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* 1 if `text` is on one of the screen's 28 rows */
static int screen_has(smsdebug *d, const char *text)
{
    uint8_t nt[28 * 64];
    smsdebug_vram_read(d, 0x3800, nt, sizeof nt);
    for (int r = 0; r < 28; r++)
    {
        char line[33];
        for (int c = 0; c < 32; c++)
        {
            int t = nt[r * 64 + c * 2];
            line[c] = (t >= 0x20 && t < 0x7f) ? (char)t : ' ';
        }
        line[32] = '\0';
        if (strstr(line, text))
            return 1;
    }
    return 0;
}

static int wait_screen(smsdebug *d, const char *text, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 100)
    {
        if (screen_has(d, text))
            return 1;
        sleep_ms(100);
    }
    printf("  (no \"%s\" on screen)\n", text);
    return 0;
}

/* CONFIG's host list. The Phantasy Star-style CONFIG's splash says PRESS
 * BUTTON 1 but goes on by itself half a second after its fanfare, so the
 * test waits rather than presses: CONFIG queues a press, and one made while
 * the splash is fading out lands on the host list as 1 OPEN. */
static int wait_hosts(smsdebug *d, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 100)
    {
        if (screen_has(d, "SELECT A HOST"))
            return 1;
        sleep_ms(100);
    }
    printf("  (no host list on screen)\n");
    return 0;
}

static void press(smssession *s, int target)
{
    smssession_press(s, target, 1);
    sleep_ms(120);
    smssession_press(s, target, 0);
    sleep_ms(250);
}

/* Press until `text` shows, a few times over. */
static int press_until(smssession *s, smsdebug *d, int target, const char *text)
{
    for (int tries = 0; tries < 8; tries++)
    {
        sleep_ms(1000);
        press(s, target);
        for (int waited = 0; waited < 4000; waited += 100)
        {
            if (screen_has(d, text))
                return 1;
            sleep_ms(100);
        }
    }
    printf("  (no \"%s\" on screen)\n", text);
    return 0;
}

static uint32_t crc32_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint32_t c = 0xFFFFFFFFu;
    int ch;
    if (!f)
        return 0;
    while ((ch = fgetc(f)) != EOF)
    {
        c ^= (uint32_t)ch;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    fclose(f);
    return c ^ 0xFFFFFFFFu;
}

static uint8_t ram(smsdebug *d, uint16_t addr)
{
    uint8_t v = 0;
    smsdebug_read(d, addr, &v, 1);
    return v;
}

int main(void)
{
    char cfg[512], data[512], rom[700], dest[1200];
    smssession_paths p;
    smssession *s;
    smssession_start_opts o;
    smsdebug *d;
    uint32_t crc;

    test_crash_install();
    test_tmpdir(cfg, sizeof cfg, "cbcfg");
    test_tmpdir(data, sizeof data, "cbdata");
    snprintf(rom, sizeof rom, "%s/counter.sms", cfg);
    if (!test_rom_write(rom, TEST_ROM_COUNTER))
        return 1;
    crc = crc32_file(rom);

    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    s = smssession_new(&p);
    if (!s)
        return 1;
    smssession_default_opts(s, &o);
    o.enable_audio = 0;
    o.enable_gamepad = 0;
    o.enable_fujinet = 1;
    check(smssession_start(s, &o) == 0, "session starts with FujiNet");
    if (!smssession_fujinet_running(s))
    {
        printf("SKIP: no FujiNet runtime available\n");
        smssession_free(s);
        return 77;
    }
    d = smssession_debugger(s);

    check(smssession_import_cart_to_sd(s, rom, dest, sizeof dest) == 0, "the image imported to SD");
    check(wait_hosts(d, 60000), "CONFIG shows its host list");
    check(wait_screen(d, "1 SD", 20000), "with the SD host first, once FujiNet has sent the slots");

    /* CONFIG reads the pad once it has settled on a screen; a press it was
     * not yet looking at is pressed again */
    check(press_until(s, d, SMS_TARGET_PORT(0, SMS_ACT_1), "counter.sms"),
          "1 opens the SD host and lists the image");
    sleep_ms(1000);
    press(s, SMS_TARGET_PORT(0, SMS_ACT_DOWN));
    sleep_ms(500);
    press(s, SMS_TARGET_PORT(0, SMS_ACT_1));

    {
        smsdebug_cart c;
        int waited = 0, done = 0;
        while (waited < 30000 && !done)
        {
            smsdebug_cart_get(d, &c);
            done = c.present && c.mode == 1 && c.image_crc == crc;
            if (!done) { sleep_ms(100); waited += 100; }
        }
        printf("  after %d ms: mode %s, crc %08X (image %08X)\n", waited,
               c.mode_name ? c.mode_name : "?", c.image_crc, crc);
        check(done, "OPEN/BOOT: FujiNet pushed it, the loader copied it in, it runs as GAME");
        check(smssession_cart_booted_game(s) == 1, "the session reports a booted image");
    }
    {
        uint8_t a = ram(d, 0xC010);
        sleep_ms(300);
        check(ram(d, 0xC010) != a, "the image is running (its counter moves)");
    }

    test_crash_phase("resetting to CONFIG");
    check(smssession_reset_to_config(s) == 0, "reset to CONFIG");
    check(wait_hosts(d, 60000), "CONFIG's host list is back");
    check(!smssession_cart_booted_game(s), "and nothing booted");

    test_crash_phase("stopping the session");
    smssession_stop(s);
    test_crash_phase("freeing the session");
    smssession_free(s);
    test_crash_phase("returning from main");
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
