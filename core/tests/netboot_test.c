/*
 * netboot_test -- a network boot end to end, through the real in-process
 * FujiNet: the cartridge bring-up's fujiboot client (MOUNT_HOST ->
 * SET_DEVICE_FULLPATH -> MOUNT_IMAGE on host slot 0, "/hello.sms") runs as
 * the opened cartridge, FujiNet pushes hello.sms back over the DBC stream,
 * the loader page copies it into the SRAM one 8K window at a time, and the
 * new image runs (hello.sms carries the FUJI claim, so the mailbox stays
 * open). Then Reset to CONFIG brings CONFIG back.
 *
 * hello.sms reaches the SD folder through Import Cartridge to SD, so this
 * is also that feature's end-to-end check. Run twice by ctest: with the
 * mailbox on its worker thread (the default) and inline (SMS_CART_SYNC=1).
 *
 * Needs fujiboot.sms and hello.sms built with BOOT_PATH=/hello.sms by
 * fujinet-firmware's pico/sms/build.sh: SMS_TESTROM_DIR=/path/to/build
 * (the pinned checkout's: BOOT_PATH=/hello.sms
 * tools/fujinet/work/fujinet-firmware/pico/sms/build.sh). With fujibank.sms
 * there too, also opens that banked app. SKIPs (77) without them, with a
 * fujiboot built for another path, or without the FujiNet runtime.
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

static int wait_status(smssession *s, const char *want, int timeout_ms)
{
    char st[160] = "";
    int waited = 0;
    while (waited < timeout_ms) {
        smssession_cart_status(s, st, sizeof st);
        if (strstr(st, want)) return 1;
        sleep_ms(50); waited += 50;
    }
    printf("  (status: %s)\n", st);
    return 0;
}

static uint8_t *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    uint8_t *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)(*n > 0 ? *n : 1));
    if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static long slurp_size(const char *p)
{
    long n = 0;
    uint8_t *b = slurp(p, &n);
    if (!b) return -1;
    free(b);
    return n;
}

static uint32_t crc32_of(const uint8_t *p, long n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (long i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

int main(void)
{
    const char *dir = getenv("SMS_TESTROM_DIR");
    char cfg[512], data[512], boot[1024], hello[1024], bank[1024], dest[1200];
    smssession_paths p;
    smssession *s;
    smssession_start_opts o;
    uint8_t *img;
    long n = 0;
    uint32_t hello_crc;

    if (!dir) {
        printf("SKIP: set SMS_TESTROM_DIR to fujinet-firmware's pico/sms/build "
               "(built with BOOT_PATH=/hello.sms)\n");
        return 77;
    }
    snprintf(boot, sizeof boot, "%s/fujiboot.sms", dir);
    snprintf(hello, sizeof hello, "%s/hello.sms", dir);
    img = slurp(hello, &n);
    if (!img || n <= 0) {
        printf("SKIP: no hello.sms in %s\n", dir);
        return 77;
    }
    hello_crc = crc32_of(img, n);
    free(img);
    {
        long bn = 0, i;
        int found = 0;
        uint8_t *b = slurp(boot, &bn);
        if (!b) {
            printf("SKIP: no fujiboot.sms in %s\n", dir);
            return 77;
        }
        /* fujiboot mounts the BOOT_PATH it was built with */
        for (i = 0; i + 11 <= bn && !found; i++)
            found = memcmp(b + i, "/hello.sms", 11) == 0;
        free(b);
        if (!found) {
            printf("SKIP: %s was not built with BOOT_PATH=/hello.sms\n", boot);
            return 77;
        }
    }

    test_tmpdir(cfg, sizeof cfg, "ncfg");
    test_tmpdir(data, sizeof data, "ndata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    s = smssession_new(&p);
    if (!s) return 1;

    smssession_default_opts(s, &o);
    o.enable_audio = 0; o.enable_gamepad = 0; o.enable_fujinet = 1;
    check(smssession_start(s, &o) == 0, "session starts with FujiNet");
    if (!smssession_fujinet_running(s)) {
        printf("SKIP: no FujiNet runtime available\n");
        smssession_free(s);
        return 77;
    }

    /* the image to boot goes in the root of the SD host (host slot 0) */
    check(smssession_import_cart_to_sd(s, hello, dest, sizeof dest) == 0,
          "hello.sms imported to SD");

    check(smssession_load_cart(s, boot) == 0, "fujiboot opened as the cartridge");
    {
        smsdebug *d = smssession_debugger(s);
        smsdebug_cart c;
        int waited = 0, done = 0;

        while (waited < 60000 && !done) {
            smsdebug_cart_get(d, &c);
            done = c.mode == 2 && c.image_crc == hello_crc;
            if (!done) { sleep_ms(100); waited += 100; }
        }
        printf("  after %d ms: mode %s, mapper %s, crc %08X (hello %08X), boot state %d, load state %d\n",
               waited, c.mode_name ? c.mode_name : "?", c.mapper_name, c.image_crc, hello_crc,
               c.boot_state, c.load_state);
        check(done, "FujiNet pushed hello.sms, the loader copied it in, and it runs");
        check(c.claim, "hello.sms claims the mailbox, so it stays open");
        check(smssession_cart_booted_game(s) == 1, "the session reports a booted image");

        /* hello's text is on screen: its name table at $3800 spells
         * " HELLO, WORLD " on row 6 (its font puts character c at tile c) */
        {
            char row[33];
            int seen = 0;
            waited = 0;
            while (!seen && waited < 5000) {
                uint8_t vram[64];
                smsdebug_vram_read(d, 0x3800 + 6 * 64, vram, 64);
                for (int i = 0; i < 32; i++) {
                    int t = vram[i * 2] | ((vram[i * 2 + 1] & 1) << 8);
                    row[i] = (t >= 0x20 && t < 0x7f) ? (char)t : ' ';
                }
                row[32] = '\0';
                seen = strstr(row, "HELLO, WORLD") != NULL;
                if (!seen) { sleep_ms(100); waited += 100; }
            }
            printf("  name table row 6: [%s]\n", row);
            check(seen, "hello's greeting is in the name table");
        }
    }

    check(smssession_reset_to_config(s) == 0, "reset to CONFIG");
    check(wait_status(s, "connected", 10000), "CONFIG is back, link up");
    check(!smssession_cart_booted_game(s), "and nothing booted");

    /* a claimed client larger than 32K: opened, it runs from the SRAM on
     * the Sega mapper with the mailbox live, and reaches FujiNet */
    snprintf(bank, sizeof bank, "%s/fujibank.sms", dir);
    if (slurp_size(bank) > 0x8000) {
        smsdebug *d = smssession_debugger(s);
        smsdebug_cart c;
        int waited = 0;
        check(smssession_load_cart(s, bank) == 0, "fujibank (a banked app) opened");
        do {
            smsdebug_cart_get(d, &c);
            if (!c.present) { sleep_ms(20); waited += 20; }
        } while (!c.present && waited < 3000);
        printf("  fujibank: mode %d, mapper %s, claim %d, %u bytes\n", c.mode, c.mapper_name,
               c.claim, c.image_size);
        check(c.mode == 2 && c.claim && c.image_size > 0x8000, "it runs as an APP, mailbox live");
        check(wait_status(s, "connected", 10000), "and its link comes up");
        check(smssession_reset_to_config(s) == 0, "reset to CONFIG");
    }

    smssession_stop(s);
    smssession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
