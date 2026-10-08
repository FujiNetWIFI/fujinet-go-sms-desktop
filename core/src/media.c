/*
 * media -- routing a file the user dropped on the window to the right place.
 *
 * Three destinations, and which one a file wants is not a matter of taste:
 *
 *   BIOS images and the YM2413 patch ROM (recognised by size and CRC, not
 *   by name -- a BIOS dump is a .bin or a .rom like any cartridge) go to
 *   the ROM directory, through smssession_import_bios.
 *
 *   Cartridges (.sms, .sg, .bin, .rom) go to the cartridge directory and are
 *   opened onto the FujiNet cartridge directly, whose own mapper engine maps
 *   them.
 *
 *   Anything else goes to the FujiNet SD folder, because FujiNet is what
 *   serves it.
 *
 * Ported from the NES sibling's media.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "session_internal.h"

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

static int ext_is(const char *path, const char *const *exts)
{
    const char *dot = strrchr(base_name(path), '.');
    int i;
    if (!dot) return 0;
    for (i = 0; exts[i]; i++) {
        const char *a = dot + 1, *b = exts[i];
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            if (ca != *b) break;
            a++; b++;
        }
        if (!*a && !*b) return 1;
    }
    return 0;
}

/* What MAME's sega8 slots take (sms_cart: .bin; the Japanese slot also
 * .sg) plus the extensions images are kept under. The cartridge plans every
 * image itself (smsmap_plan: the CRC database, a .cfg override, MAME's
 * heuristic), so the extension only routes the file. */
static const char *const cart_exts[] = { "sms", "sg", "bin", "rom", NULL };

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[16384];
    size_t n;

    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    if (fclose(out) != 0) return -1;
    return 0;
}

int smssession_import_media(smssession *s, const char *src_path,
                            char *dest_out, int dest_sz)
{
    const char *name;
    const char *dir;
    struct stat st;
    char abs[SMS_PATH_MAX];

    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }
    src_path = paths_resolve(src_path, abs, sizeof abs);
    name = base_name(src_path);

    if (smssession_media_is_bios(src_path)) {
        char msg[256];
        int idx = smssession_import_bios(s, src_path, msg, sizeof msg);
        if (idx < 0)
            return -1;
        snprintf(dest_out, (size_t)dest_sz, "%s/%s", s->roms_dir,
                 idx < smssession_bios_count() ? smssession_bios_info(idx)->file : name);
        return 0;
    }

    if (ext_is(src_path, cart_exts)) {
        dir = s->carts_dir;
    } else {
        /* The SD tree only exists once the FujiNet runtime has been
         * provisioned. Test the DIRECTORY, not just the path string: the
         * path is always computed, so a string check passes and the copy
         * then fails with "could not copy", which tells the user nothing
         * about the actual problem. */
        if (!s->fujinet_sd[0] ||
            stat(s->fujinet_sd, &st) != 0 || !S_ISDIR(st.st_mode)) {
            session_set_error(s,
                "%s is not a cartridge, so FujiNet would serve it -- but the "
                "FujiNet runtime is not available, so there is nowhere to "
                "put it.", name);
            return -1;
        }
        dir = s->fujinet_sd;
    }

    snprintf(dest_out, (size_t)dest_sz, "%s/%s", dir, name);
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", name, dir);
        return -1;
    }
    return 0;
}

int smssession_media_is_cartridge(const char *path)
{
    return path ? ext_is(path, cart_exts) : 0;
}

/* The .cfg sibling of an image (game.sms -> game.cfg), if there is one:
 * FujiNet pushes it ahead of the image and the cartridge takes its
 * mapper= line, so it travels with the image. */
static int cfg_sibling(const char *path, char *out, int outsz)
{
    const char *dot = strrchr(base_name(path), '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    struct stat st;

    if ((int)stem + 5 > outsz)
        return 0;
    memcpy(out, path, stem);
    memcpy(out + stem, ".cfg", 5);
    return stat(out, &st) == 0 && S_ISREG(st.st_mode);
}

/* Into the SD root, so CONFIG shows it at the top of the SD host with no
 * navigation. FujiNet pushes .sms, .sg, .rom and .bin files to the
 * cartridge (tools/fujinet/patches/0002), which maps whatever its smsmap
 * can. */
int smssession_import_cart_to_sd(smssession *s, const char *src_path,
                                 char *dest_out, int dest_sz)
{
    const char *name;
    char cfg[SMS_PATH_MAX], cfg_dst[SMS_PATH_MAX];
    char abs[SMS_PATH_MAX];
    struct stat st;

    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }
    src_path = paths_resolve(src_path, abs, sizeof abs);
    name = base_name(src_path);
    if (!s->fujinet_sd[0] ||
        stat(s->fujinet_sd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        session_set_error(s,
            "The FujiNet runtime is not available, so there is no SD folder "
            "to import %s into.", name);
        return -1;
    }
    {
        char why[256];
        if (!smssession_check_cart(src_path, why, sizeof why)) {
            session_set_error(s, "%s: %s", name, why);
            return -1;
        }
    }
    snprintf(dest_out, (size_t)dest_sz, "%s/%s", s->fujinet_sd, name);
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", name, s->fujinet_sd);
        return -1;
    }
    if (cfg_sibling(src_path, cfg, sizeof cfg)) {
        snprintf(cfg_dst, sizeof cfg_dst, "%s/%s", s->fujinet_sd, base_name(cfg));
        if (copy_file(cfg, cfg_dst) != 0) {
            session_set_error(s, "Could not copy %s to %s", base_name(cfg),
                              s->fujinet_sd);
            return -1;
        }
    }
    return 0;
}
