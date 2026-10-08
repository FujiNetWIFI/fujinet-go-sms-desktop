/*
 * session_test -- the frontend contract end to end, through the public API
 * only: settings round-trip and persist across sessions, paths resolve
 * inside the given tree, CONFIG boots and paints, the keyboard and the
 * console's buttons reach the machine, a cartridge opens and runs, Soft
 * Reset keeps it, Reset to CONFIG ejects it, an image the cartridge cannot
 * map is refused with the reason, a console change takes effect at the
 * power cycle, and a remembered cartridge boots at the next start.
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

static uint32_t px[SMSSESSION_FB_WIDTH * SMSSESSION_FB_MAX_HEIGHT];

static int wait_frames(smssession *s, uint64_t *serial, int count, int timeout_ms)
{
    int got = 0, waited = 0, h;
    while (waited < timeout_ms) {
        if (smssession_copy_frame(s, px, &h, serial)) {
            if (++got >= count) return 1;
        }
        sleep_ms(2); waited += 2;
    }
    return 0;
}

/* Poll the cartridge status until it contains `want`, or time out. */
static int wait_status(smssession *s, const char *want, int timeout_ms)
{
    char st[160];
    int waited = 0;
    while (waited < timeout_ms) {
        smssession_cart_status(s, st, sizeof st);
        if (strstr(st, want)) return 1;
        sleep_ms(20); waited += 20;
    }
    printf("  (status: %s)\n", st);
    return 0;
}

/* The test ROM keeps its state in work RAM; peek at it through the
 * debugger (which stops the machine while attached) and let it run on. */
static uint8_t ram(smssession *s, uint16_t addr)
{
    smsdebug *d = smssession_debugger(s);
    uint8_t v = 0;
    smsdebug_attach(d);
    smsdebug_read(d, addr, &v, 1);
    smsdebug_detach(d);
    return v;
}

int main(void)
{
    char cfg[512], data[512], rom[700], big[700], launch[512];
    smssession_paths p;
    smssession *s;
    smssession_start_opts o;
    uint64_t serial = 0;
    int h;

    test_tmpdir(cfg, sizeof cfg, "cfg");
    test_tmpdir(data, sizeof data, "data");
    if (!test_getcwd(launch, sizeof launch))
        return 1;
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    p.fujinet_lib = "";     /* no runtime: the cart runs link-down */

    snprintf(rom, sizeof rom, "%s/counter.sms", cfg);
    check(test_rom_write(rom, TEST_ROM_COUNTER) > 0, "wrote a 32K test image");
    snprintf(big, sizeof big, "%s/huge.sms", cfg);
    check(test_rom_write(big, TEST_ROM_TOO_BIG) > 0, "wrote a 1 MB + 16K test image");

    s = smssession_new(&p);
    check(s != NULL, "session created");
    if (!s) return 1;

    check(strcmp(smssession_config_path(s), cfg) == 0, "config path is the given tree");
    check(strncmp(smssession_carts_path(s), data, strlen(data)) == 0, "carts dir is under the data tree");
    check(strncmp(smssession_sd_path(s), data, strlen(data)) == 0, "SD path is under the data tree");
    check(strncmp(smssession_roms_path(s), data, strlen(data)) == 0, "ROM dir is under the data tree");

    smssession_set_int(s, "answer", 42);
    smssession_set_str(s, "greeting", "hello");
    check(smssession_get_int(s, "answer", 0) == 42, "int setting round-trips");
    check(strcmp(smssession_get_str(s, "greeting", ""), "hello") == 0, "string setting round-trips");
    check(smssession_get_int(s, "nope", 7) == 7, "missing setting yields its default");

    smssession_default_opts(s, &o);
    check(o.enable_fujinet == 1 && o.enable_audio == 1, "default opts enable FujiNet and audio");
    check(o.console == SMS_CONSOLE_SMS1, "the default console is the Master System");
    check(!o.bios || !o.bios[0], "no BIOS by default: the cartridge boots directly");
    check(o.cart_path == NULL, "no remembered cartridge: CONFIG");

    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(smssession_start(s, &o) == 0, "session starts (CONFIG, no FujiNet)");
    if (!smssession_is_running(s)) { printf("error: %s\n", smssession_last_error(s)); return 1; }

    check(wait_frames(s, &serial, 30, 5000), "frames arrive");
    check(wait_status(s, "link down", 5000), "CONFIG runs link-down");
    check(!smssession_cart_booted_game(s), "CONFIG is not a booted game");
    {
        uint64_t z = 0; int distinct = 0, i, tries;
        check(smssession_copy_frame(s, px, &h, &z) == 1, "a forced copy (serial 0) always copies");
        check(h == 224, "the frame is 224 lines (MAME's NTSC visible area)");
        /* up to 15 s: a loaded CI runner can take a while to the first paint */
        for (tries = 0; tries < 150; tries++) {
            uint32_t first = px[0];
            distinct = 0;
            for (i = 0; i < SMSSESSION_FB_WIDTH * h; i++) if (px[i] != first) distinct++;
            if (distinct > 100) break;
            sleep_ms(100);
            z = 0;
            smssession_copy_frame(s, px, &h, &z);
        }
        check(distinct > 100, "the CONFIG client painted something");
    }
    check(smssession_refresh_rate(s) == 60, "an NTSC console: 60 Hz");

    /* keyboard: the default map */
    check(smssession_key(s, 'z', 1) == 1, "Z is bound (player 1 button 1)");
    check(smssession_buttons_held(s, 0) & (1u << SMS_ACT_1), "and holds it");
    check(smssession_key(s, 'z', 0) == 1, "and released");
    check(smssession_key(s, SMS_KEYSYM_F11, 1) == 0, "an unbound key is ignored");
    check(smssession_key_sysaction(s, SMS_KEYSYM_ESCAPE) == SMS_SYSACT_RESET_CONFIG,
          "Escape is the Reset to CONFIG system action");
    check(smssession_key_sysaction(s, SMS_KEYSYM_F3) == SMS_SYSACT_SOFT_RESET,
          "F3 is Soft Reset");
    check(smssession_target_for_key(s, SMS_KEYSYM_BACKSPACE) == SMS_TARGET_SWITCH(SMS_SW_RESET),
          "Backspace is the console's Reset button");
    check(smssession_target_for_key(s, SMS_KEYSYM_RETURN) == SMS_TARGET_SWITCH(SMS_SW_PAUSE),
          "Return is the Pause button");
    check(wait_frames(s, &serial, 10, 3000), "the machine keeps running after input");

    /* an image the cartridge cannot map */
    {
        char why[256];
        check(smssession_check_cart(big, why, sizeof why) == 0, "a 1 MB + 16K image is refused");
        check(strstr(why, "1 MB") != NULL, "with the reason");
        check(smssession_load_cart(s, big) == -1, "load_cart refuses it");
        check(!smssession_cart_booted_game(s) && smssession_cart_path(s)[0] == '\0',
              "and CONFIG keeps running");
    }

    /* open a cartridge: it goes straight into the SRAM and runs */
    check(smssession_check_cart(rom, NULL, 0) == 1, "the test image is accepted");
    check(smssession_load_cart(s, rom) == 0, "load_cart");
    check(strcmp(smssession_cart_path(s), rom) == 0, "the cart path is remembered");
    check(strcmp(smssession_get_str(s, "cart", ""), rom) == 0, "and persisted");
    check(wait_status(s, "game running", 3000), "the cartridge reports a game (mailbox closed)");
    {
        uint8_t a = ram(s, 0xC010);
        sleep_ms(200);
        check(ram(s, 0xC010) != a, "the image is running (its counter moves)");
    }

    /* the joypad and the Pause button reach the program */
    check((ram(s, 0xC012) & 0x10) != 0, "button 1 reads released");
    smssession_press(s, SMS_TARGET_PORT(0, SMS_ACT_1), 1);
    sleep_ms(150);
    check((ram(s, 0xC012) & 0x10) == 0, "button 1 held reads low on $DC");
    smssession_press(s, SMS_TARGET_PORT(0, SMS_ACT_1), 0);
    {
        uint8_t n = ram(s, 0xC011);
        smssession_press(s, SMS_TARGET_SWITCH(SMS_SW_PAUSE), 1);
        sleep_ms(150);
        smssession_press(s, SMS_TARGET_SWITCH(SMS_SW_PAUSE), 0);
        sleep_ms(100);
        check(ram(s, 0xC011) == (uint8_t)(n + 1), "Pause raises exactly one NMI");
    }

    /* Soft Reset: the same image restarts; it is still the cartridge's */
    check(smssession_soft_reset(s) == 0, "soft reset");
    serial = 0;
    check(wait_frames(s, &serial, 5, 3000), "frames after reset");
    check(smssession_cart_booted_game(s), "the game is still in the cartridge after a reset");
    check(strcmp(smssession_cart_path(s), rom) == 0, "and still the open cartridge");

    /* Reset to CONFIG: a power cycle that ejects it */
    check(smssession_reset_to_config(s) == 0, "reset to CONFIG");
    check(smssession_cart_path(s)[0] == '\0', "the cartridge is ejected");
    check(strcmp(smssession_get_str(s, "cart", "x"), "") == 0, "and forgotten");
    serial = 0;
    check(wait_frames(s, &serial, 10, 5000), "frames flow from the new console");
    check(wait_status(s, "link down", 5000), "CONFIG is back");
    check(!smssession_cart_booted_game(s), "no game in the cartridge");
    check(smssession_cart_link_up(s) == 0, "with no runtime the cart reports link down");

    /* a console change takes effect at the power cycle (restart reads the
     * settings, so keep the devices off there too) */
    smssession_set_int(s, "enable_fujinet", 0);
    smssession_set_int(s, "enable_audio", 0);
    smssession_set_int(s, "enable_gamepad", 0);
    smssession_set_int(s, "console", SMS_CONSOLE_SMS2_PAL);
    check(smssession_restart(s) == 0, "restart onto a PAL Master System II");
    serial = 0;
    check(wait_frames(s, &serial, 5, 5000), "frames from the PAL console");
    {
        uint64_t z = 0;
        smssession_copy_frame(s, px, &h, &z);
        check(h == 240, "the PAL frame is 240 lines");
    }
    check(smssession_refresh_rate(s) == 50, "and 50 Hz");
    smssession_set_int(s, "console", SMS_CONSOLE_MARK3);
    check(smssession_restart(s) == 0, "restart onto the Mark III");
    check(wait_status(s, "link down", 5000), "the Mark III boots CONFIG too");
    smssession_set_int(s, "console", SMS_CONSOLE_SMSJ);
    check(smssession_restart(s) == 0, "restart onto the Japanese Master System");
    check(wait_status(s, "link down", 5000), "and so does the Japanese console");
    smssession_set_int(s, "console", SMS_CONSOLE_SMS1);

    /* A path relative to where the app started still means that file after
     * the process's working directory moves: the in-process FujiNet makes
     * its runtime root the working directory. */
    {
        char rel[64], abs[1200];
        snprintf(rel, sizeof rel, "sms-rel-%ld.sms", (long)test_getpid());
        snprintf(abs, sizeof abs, "%s/%s", launch, rel);
        check(test_rom_write(abs, TEST_ROM_COUNTER) > 0, "wrote an image in the launch directory");
        check(test_chdir(data) == 0, "the working directory moves away");
        check(smssession_check_cart(rel, NULL, 0) == 1, "a bare name still finds it");
        check(smssession_load_cart(s, rel) == 0, "and opens it");
        check(strcmp(smssession_cart_path(s), abs) == 0, "remembered by its full path");
        check(wait_status(s, "game running", 3000), "and it runs");
        test_chdir(launch);
        remove(abs);
    }

    /* a remembered cartridge boots at the next start */
    check(smssession_load_cart(s, rom) == 0, "open the cartridge again");
    smssession_stop(s);
    check(!smssession_is_running(s), "session stops");
    smssession_free(s);

    s = smssession_new(&p);
    check(s && smssession_get_int(s, "answer", 0) == 42, "settings persisted across sessions");
    if (s) {
        smssession_default_opts(s, &o);
        check(o.cart_path && strcmp(o.cart_path, rom) == 0, "the cartridge is remembered");
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
        check(smssession_start(s, &o) == 0, "restart");
        check(wait_status(s, "game running", 3000), "and it boots the remembered cartridge");
        smssession_stop(s);
        smssession_free(s);
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
