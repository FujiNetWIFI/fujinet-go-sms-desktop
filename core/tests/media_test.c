/*
 * media_test -- where a dropped file goes, Import Cartridge to SD (with its
 * .cfg sibling), and Import BIOS.
 *
 * No Sega firmware is used: the BIOS images here are filler whose last four
 * bytes are chosen so the CRC-32 matches an entry in MAME's table (a CRC is
 * linear, so any four bytes can steer it), which exercises identification
 * by size and CRC exactly as a real image would.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "smssession.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void write_file(const char *path, size_t n)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    for (i = 0; i < n; i++) fputc((int)((i * 7 + 3) & 0xff), f);
    fclose(f);
}

static uint32_t table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
}

/* Filler of `size` bytes whose CRC-32 is `crc`: run the register forward
 * over all but the last four bytes, run it backward from the wanted final
 * register over four zero bytes, and the difference is the four bytes. */
static void write_forged(const char *path, uint32_t size, uint32_t crc)
{
    uint8_t *b = malloc(size);
    uint32_t s = 0xFFFFFFFFu, t = crc ^ 0xFFFFFFFFu;
    FILE *f;

    for (uint32_t i = 0; i < size; i++)
        b[i] = (uint8_t)(i * 13 + 5);
    for (uint32_t i = 0; i < size - 4; i++)
        s = (s >> 8) ^ table[(s ^ b[i]) & 0xFF];
    for (int k = 0; k < 4; k++) {
        int j = 0;
        while ((table[j] >> 24) != (t >> 24))
            j++;
        t = ((t ^ table[j]) << 8) | (uint32_t)j;
    }
    s ^= t;
    for (int k = 0; k < 4; k++)
        b[size - 4 + k] = (uint8_t)(s >> (8 * k));
    f = fopen(path, "wb");
    fwrite(b, 1, size, f);
    fclose(f);
    free(b);
}

static int bios_index(const char *name)
{
    return smssession_bios_find(name);
}

int main(void)
{
    char cfg[512], data[512], src[700], dest[1200], sd[1200], msg[512];
    smssession_paths p;
    smssession *s;
    const sms_bios_info *bi;
    int idx;

    crc_init();
    test_tmpdir(cfg, sizeof cfg, "mcfg");
    test_tmpdir(data, sizeof data, "mdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = smssession_new(&p);
    if (!s) return 1;

    /* routing by extension */
    check(smssession_media_is_cartridge("game.sms"), ".sms is a cartridge");
    check(smssession_media_is_cartridge("GAME.SMS"), ".SMS is a cartridge (case-insensitive)");
    check(smssession_media_is_cartridge("game.sg"), ".sg is a cartridge");
    check(smssession_media_is_cartridge("game.bin") && smssession_media_is_cartridge("game.rom"),
          ".bin and .rom are cartridges unless they are a BIOS");
    check(!smssession_media_is_cartridge("disk.atr"), ".atr is not");
    check(!smssession_media_is_cartridge("notes.txt"), ".txt is nothing");

    snprintf(src, sizeof src, "%s/alex.sms", cfg);
    write_file(src, 0x8000);
    check(smssession_import_media(s, src, dest, sizeof dest) == 0, "a cartridge imports");
    check(strncmp(dest, smssession_carts_path(s), strlen(smssession_carts_path(s))) == 0,
          "into the cartridge directory");
    check(exists(dest), "and the copy exists");

    snprintf(src, sizeof src, "%s/notes.txt", cfg);
    write_file(src, 10);
    check(smssession_import_media(s, src, dest, sizeof dest) == -1,
          "anything else is refused while there is no SD folder");
    check(strstr(smssession_last_error(s), "nowhere to put it") != NULL, "with a useful message");

    /* Import Cartridge to SD: the image and its .cfg sibling. The SD folder
     * needs the runtime's provisioning; make it by hand here. */
    snprintf(src, sizeof src, "%s/wonder.sms", cfg);
    write_file(src, 0x20000);
    check(smssession_import_cart_to_sd(s, src, dest, sizeof dest) == -1,
          "SD import refuses when there is no SD folder");
    snprintf(sd, sizeof sd, "%s/fujinet", data); test_mkdir(sd);
    snprintf(sd, sizeof sd, "%s/fujinet/SD", data); test_mkdir(sd);
    {
        char cfgfile[760];
        FILE *f;
        snprintf(cfgfile, sizeof cfgfile, "%s/wonder.cfg", cfg);
        f = fopen(cfgfile, "w");
        fputs("mapper=codemasters\n", f);
        fclose(f);
    }
    check(smssession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "SD import copies into the SD root");
    snprintf(sd, sizeof sd, "%s/fujinet/SD/wonder.sms", data);
    check(strcmp(dest, sd) == 0 && exists(dest), "at the top of the SD tree");
    snprintf(sd, sizeof sd, "%s/fujinet/SD/wonder.cfg", data);
    check(exists(sd), "with its .cfg mapper override beside it");

    /* the BIOS table */
    check(smssession_bios_count() >= 11, "MAME's Master System BIOSes and the patch ROM are listed");
    idx = bios_index("bios13");
    bi = idx >= 0 ? smssession_bios_info(idx) : NULL;
    check(bi && bi->size == 0x2000 && bi->crc == 0x0072ed54u, "bios13: 8K, CRC 0072ED54");
    check(bios_index("nope") == -1, "an unknown name is not found");
    check(!smssession_bios_available(s, idx), "nothing is imported yet");
    check(smssession_console_bios(s, SMS_CONSOLE_SMS1)[0] == '\0', "the Master System boots without a BIOS");

    /* a dropped BIOS is recognised by size and CRC and filed under MAME's
     * name; it becomes the BIOS of the console it fits */
    snprintf(src, sizeof src, "%s/my-bios.bin", cfg);
    write_forged(src, 0x2000, 0x0072ed54u);
    check(smssession_media_is_bios(src), "a BIOS is recognised by size and CRC");
    check(smssession_import_media(s, src, dest, sizeof dest) == 0, "and a dropped one is imported");
    check(strstr(dest, "mpr-10052.rom") != NULL && exists(dest), "under MAME's file name in the ROM directory");
    check(smssession_bios_available(s, idx), "it is available");
    check(strcmp(smssession_console_bios(s, SMS_CONSOLE_SMS1), "bios13") == 0,
          "and selected for the Master System");
    {
        smssession_start_opts o;
        smssession_default_opts(s, &o);
        check(o.bios && strcmp(o.bios, "bios13") == 0, "the next power-on uses it");
    }

    /* a BIOS for another console is imported but not selected */
    snprintf(src, sizeof src, "%s/jp.rom", cfg);
    write_forged(src, 0x2000, 0x48d44a13u);
    idx = smssession_import_bios(s, src, msg, sizeof msg);
    check(idx == bios_index("jbios21"), "the Japanese BIOS is identified");
    check(strstr(msg, "different console") != NULL, "and the message says it fits another console");
    check(strcmp(smssession_console_bios(s, SMS_CONSOLE_SMS1), "bios13") == 0,
          "the Master System keeps its BIOS");

    /* the YM2413 patch ROM */
    snprintf(src, sizeof src, "%s/ym.bin", cfg);
    write_forged(src, 0x90, 0x6f582d01u);
    idx = smssession_import_bios(s, src, msg, sizeof msg);
    check(idx == bios_index("ym2413"), "the YM2413 patch ROM is identified");
    check(strstr(msg, "instrument") != NULL, "and described");

    /* an unknown image of a BIOS size: imported as custom, with a warning */
    snprintf(src, sizeof src, "%s/homebrew-bios.rom", cfg);
    write_file(src, 0x2000);
    check(!smssession_media_is_bios(src), "an unknown 8K image is not recognised");
    idx = smssession_import_bios(s, src, msg, sizeof msg);
    check(idx == smssession_bios_count(), "Import BIOS takes it as a custom BIOS");
    check(strstr(msg, "CRC-32") != NULL, "with a warning naming its CRC");
    check(strcmp(smssession_console_bios(s, SMS_CONSOLE_SMS1), "custom") == 0,
          "and it becomes the Master System's BIOS");

    /* and something that is not a BIOS at all */
    snprintf(src, sizeof src, "%s/tiny.bin", cfg);
    write_file(src, 1000);
    check(smssession_import_bios(s, src, msg, sizeof msg) == -1, "a 1000-byte file is refused");
    check(strstr(msg, "not a Master System BIOS") != NULL, "with the sizes a BIOS can be");

    smssession_set_console_bios(s, SMS_CONSOLE_SMS1, "");
    check(smssession_console_bios(s, SMS_CONSOLE_SMS1)[0] == '\0', "None (boot the cartridge) is selectable");

    smssession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
