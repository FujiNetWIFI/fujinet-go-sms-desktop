/*
 * roms -- the Master System BIOSes and the YM2413's patch ROM: what MAME
 * knows, what the user imported, and what a developer build embedded.
 *
 * None of these is ever required. Every console boots the FujiNet cartridge
 * directly, as MAME's "none" BIOS does, and the YM2413 falls back to ymfm's
 * own patch table. They are copyrighted Sega and Yamaha firmware and this
 * project does not redistribute them (see COMPLIANCE.md): "Import BIOS..."
 * copies a user's own image into the ROM directory, recognised by size and
 * CRC-32 against MAME's ROM_START tables (src/mame/sega/sms.cpp,
 * src/devices/sound/ymopl.cpp) and stored under MAME's file name.
 *
 * WITH_SMS_ROMS=ON embeds the recognised images a developer put in
 * tools/roms/; published builds are built with it OFF and
 * core/tests/no_embedded_roms.py checks that claim against the shipped
 * binaries.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "roms_embedded.h"
#include "session_internal.h"

#define C(console) (1u << (console))
#define EXPORT_NTSC C(SMS_CONSOLE_SMS1)
#define EXPORT1     (C(SMS_CONSOLE_SMS1) | C(SMS_CONSOLE_SMS1_PAL))

/* MAME's ROM_SYSTEM_BIOS entries for the consoles this app has, and the
 * YM2413's internal patch set. The order is the menus' order. */
static const sms_bios_info s_bios[] = {
    { "bios13",   "mpr-10052.rom",  "US/European BIOS v1.3 (1986)",
      0x2000,  0x0072ed54u, EXPORT1 },
    { "hangonsh", "mpr-11459a.rom", "US/European BIOS v2.4 with Hang On and Safari Hunt (1988)",
      0x20000, 0x91e93385u, EXPORT1 },
    { "hangon",   "mpr-11458.rom",  "US/European BIOS v3.4 with Hang On (1988)",
      0x20000, 0x8edf7ac6u, EXPORT1 },
    { "missiled", "missiled.rom",   "US/European BIOS v4.4 with Missile Defense 3D (1988)",
      0x20000, 0xe79bb689u, EXPORT1 },
    { "v10",      "v1.0.bin",       "US Master System BIOS v1.0 (prototype)",
      0x2000,  0x72bec693u, EXPORT_NTSC },
    { "proto",    "m404prot.rom",   "US Master System Prototype BIOS",
      0x2000,  0x1a15dfccu, EXPORT_NTSC },
    { "bios20",   "mpr-10883.rom",  "European BIOS v2.0 (1987?)",
      0x2000,  0xb3d854f8u, C(SMS_CONSOLE_SMS1_PAL) },
    { "alexkidd", "mpr-12808.ic2",  "US/European BIOS with Alex Kidd in Miracle World (1990)",
      0x20000, 0xcf4a09eau, C(SMS_CONSOLE_SMS2) | C(SMS_CONSOLE_SMS2_PAL) },
    { "sonic",    "sonbios.rom",    "European/Brazilian BIOS with Sonic the Hedgehog (1991)",
      0x40000, 0x81c3476bu, C(SMS_CONSOLE_SMS2_PAL) },
    { "jbios21",  "mpr-11124.ic2",  "Japanese BIOS v2.1 (1987)",
      0x2000,  0x48d44a13u, C(SMS_CONSOLE_SMSJ) },
    { "ym2413",   "ym2413_instruments.bin", "YM2413 (FM) instrument ROM",
      0x90,    0x6f582d01u, 0 },
};
#define BIOS_COUNT ((int)(sizeof s_bios / sizeof s_bios[0]))
#define YM2413_INDEX (BIOS_COUNT - 1)

/* An image of a BIOS size whose CRC is not in the table is imported as the
 * one custom BIOS, usable on every console with a BIOS socket. */
static const sms_bios_info s_custom = {
    "custom", "custom-bios.rom", "Custom BIOS (unrecognised CRC)",
    0, 0, C(SMS_CONSOLE_SMS1) | C(SMS_CONSOLE_SMS1_PAL) | C(SMS_CONSOLE_SMS2)
        | C(SMS_CONSOLE_SMS2_PAL) | C(SMS_CONSOLE_SMSJ),
};

static const uint32_t s_bios_sizes[] = { 0x2000, 0x4000, 0x8000, 0x20000, 0x40000 };
#define MAX_IMAGE 0x40000

int smssession_bios_count(void)
{
    return BIOS_COUNT;
}

const sms_bios_info *smssession_bios_info(int i)
{
    if (i >= 0 && i < BIOS_COUNT)
        return &s_bios[i];
    if (i == BIOS_COUNT)
        return &s_custom;
    return NULL;
}

int smssession_bios_find(const char *name)
{
    if (!name || !*name)
        return -1;
    for (int i = 0; i < BIOS_COUNT; i++)
        if (strcmp(s_bios[i].name, name) == 0)
            return i;
    if (strcmp(name, s_custom.name) == 0)
        return BIOS_COUNT;
    return -1;
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int built;
    uint32_t c = 0xFFFFFFFFu;

    if (!built) {
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t v = k;
            for (int j = 0; j < 8; j++)
                v = (v & 1) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
            table[k] = v;
        }
        built = 1;
    }
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* The whole file, if it is no bigger than `max`; NULL otherwise. */
static uint8_t *read_file(const char *path, uint32_t max, uint32_t *size)
{
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    uint8_t *buf;
    size_t got;

    if (!f)
        return NULL;
    buf = malloc((size_t)max + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)max + 1, f);
    fclose(f);
    if (got == 0 || got > max) {
        free(buf);
        return NULL;
    }
    *size = (uint32_t)got;
    return buf;
}

static int identify(const uint8_t *data, uint32_t size)
{
    const uint32_t crc = crc32_of(data, size);
    for (int i = 0; i < BIOS_COUNT; i++)
        if (s_bios[i].size == size && s_bios[i].crc == crc)
            return i;
    return -1;
}

static int is_bios_size(uint32_t size)
{
    for (size_t i = 0; i < sizeof s_bios_sizes / sizeof s_bios_sizes[0]; i++)
        if (s_bios_sizes[i] == size)
            return 1;
    return 0;
}

static void rom_path(const struct smssession *s, const sms_bios_info *b, char *out, int n)
{
    snprintf(out, (size_t)n, "%s/%s", s->roms_dir, b->file);
}

static const sms_embedded_rom *embedded(const sms_bios_info *b)
{
    for (unsigned i = 0; i < sms_embedded_rom_count; i++)
        if (strcmp(sms_embedded_roms[i].file, b->file) == 0)
            return &sms_embedded_roms[i];
    return NULL;
}

int smssession_bios_available(smssession *s, int i)
{
    const sms_bios_info *b = smssession_bios_info(i);
    char path[SMS_PATH_MAX];
    struct stat st;

    if (!b)
        return 0;
    if (embedded(b))
        return 1;
    rom_path(s, b, path, sizeof path);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    return b->size ? (uint32_t)st.st_size == b->size : is_bios_size((uint32_t)st.st_size);
}

uint8_t *roms_load_bios(struct smssession *s, const char *name, uint32_t *size)
{
    const int i = smssession_bios_find(name);
    const sms_bios_info *b = smssession_bios_info(i);
    char path[SMS_PATH_MAX];
    uint8_t *buf;

    if (!b || i == YM2413_INDEX)
        return NULL;
    rom_path(s, b, path, sizeof path);
    buf = read_file(path, MAX_IMAGE, size);
    if (buf && (b->size ? *size == b->size : is_bios_size(*size)))
        return buf;
    free(buf);

    {
        const sms_embedded_rom *e = embedded(b);
        if (e) {
            buf = malloc(e->size);
            if (!buf)
                return NULL;
            memcpy(buf, e->data, e->size);
            *size = e->size;
            return buf;
        }
    }
    return NULL;
}

uint8_t *roms_load_instruments(struct smssession *s)
{
    uint32_t size = 0;
    const sms_bios_info *b = &s_bios[YM2413_INDEX];
    char path[SMS_PATH_MAX];
    uint8_t *buf;

    rom_path(s, b, path, sizeof path);
    buf = read_file(path, b->size, &size);
    if (buf && size == b->size)
        return buf;
    free(buf);
    {
        const sms_embedded_rom *e = embedded(b);
        if (e && e->size == b->size) {
            buf = malloc(e->size);
            if (buf)
                memcpy(buf, e->data, e->size);
            return buf;
        }
    }
    return NULL;
}

/* Materialise the embedded images into the ROM directory on first run, so a
 * developer build behaves like a configured install. */
void roms_provision_embedded(struct smssession *s)
{
    for (unsigned i = 0; i < sms_embedded_rom_count; i++) {
        char path[SMS_PATH_MAX];
        FILE *f;

        snprintf(path, sizeof path, "%s/%s", s->roms_dir, sms_embedded_roms[i].file);
        f = fopen(path, "rb");
        if (f) {
            fclose(f);   /* the user's own copy wins */
            continue;
        }
        f = fopen(path, "wb");
        if (!f)
            continue;
        fwrite(sms_embedded_roms[i].data, 1, sms_embedded_roms[i].size, f);
        fclose(f);
    }
}

int smssession_media_is_bios(const char *path)
{
    uint32_t size = 0;
    char abs[SMS_PATH_MAX];
    uint8_t *buf = read_file(paths_resolve(path, abs, sizeof abs), MAX_IMAGE, &size);
    int known;

    if (!buf)
        return 0;
    known = identify(buf, size) >= 0;
    free(buf);
    return known;
}

int smssession_import_bios(smssession *s, const char *path, char *msg, int msgsz)
{
    uint32_t size = 0;
    uint8_t *buf;
    int idx;
    const sms_bios_info *b;
    char dest[SMS_PATH_MAX];
    char abs[SMS_PATH_MAX];
    FILE *f;
    int console = s->running ? s->opts.console
                             : smssession_get_int(s, "console", SMS_CONSOLE_SMS1);

    if (msg && msgsz > 0)
        msg[0] = '\0';
    path = paths_resolve(path, abs, sizeof abs);
    buf = read_file(path, MAX_IMAGE, &size);
    if (!buf) {
        session_set_error(s, "%s is not a BIOS image: Master System BIOSes are "
                             "8K to 256K.", path);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "%s", s->last_error);
        return -1;
    }

    idx = identify(buf, size);
    if (idx < 0) {
        if (!is_bios_size(size)) {
            free(buf);
            session_set_error(s, "%s is not a Master System BIOS (%u bytes; a BIOS "
                                 "is 8K, 16K, 32K, 128K or 256K).", path, size);
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "%s", s->last_error);
            return -1;
        }
        idx = BIOS_COUNT;   /* custom */
    }
    b = smssession_bios_info(idx);

    rom_path(s, b, dest, sizeof dest);
    f = fopen(dest, "wb");
    if (!f || fwrite(buf, 1, size, f) != size) {
        if (f)
            fclose(f);
        free(buf);
        session_set_error(s, "Cannot write %s", dest);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "%s", s->last_error);
        return -1;
    }
    fclose(f);

    if (idx == YM2413_INDEX) {
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Imported the YM2413 instrument ROM; the "
                     "FM sound uses it from the next power cycle.");
    } else {
        const int fits = (b->consoles >> console) & 1;
        if (fits)
            smssession_set_console_bios(s, console, b->name);
        if (msg && msgsz > 0) {
            if (idx == BIOS_COUNT)
                snprintf(msg, (size_t)msgsz,
                         "Imported as a custom BIOS, but its CRC-32 (%08X) is not one "
                         "MAME knows. If the console misbehaves, suspect it first.%s",
                         crc32_of(buf, size),
                         fits ? " It boots at the next power cycle." : "");
            else if (fits)
                snprintf(msg, (size_t)msgsz, "Imported %s. It boots at the next "
                         "power cycle (Reset to CONFIG).", b->desc);
            else
                snprintf(msg, (size_t)msgsz, "Imported %s. It fits a different "
                         "console: choose it in Preferences.", b->desc);
        }
    }
    free(buf);
    return idx;
}

const char *smssession_console_bios(smssession *s, int console)
{
    char key[32];
    const char *id = sms_console_id(console);
    const char *v;

    if (!id || !sms_console_has_bios_socket(console))
        return "";
    snprintf(key, sizeof key, "bios.%s", id);
    v = smssession_get_str(s, key, NULL);
    if (v)
        return v;
    /* No choice made yet: a developer build boots the first embedded image
     * that fits; a published build boots the cartridge directly. */
    for (int i = 0; i < YM2413_INDEX; i++)
        if (((s_bios[i].consoles >> console) & 1) && embedded(&s_bios[i]))
            return s_bios[i].name;
    return "";
}

void smssession_set_console_bios(smssession *s, int console, const char *name)
{
    char key[32];
    const char *id = sms_console_id(console);

    if (!id)
        return;
    snprintf(key, sizeof key, "bios.%s", id);
    smssession_set_str(s, key, name ? name : "");
}

const char *smssession_roms_path(const smssession *s)
{
    return s->roms_dir;
}
