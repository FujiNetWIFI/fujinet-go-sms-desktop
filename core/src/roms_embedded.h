/*
 * roms_embedded -- the table tools/roms/embed-roms.py generates into the
 * build tree: empty (the shipping configuration, WITH_SMS_ROMS=OFF) or the
 * recognised images a developer put in tools/roms/ (NOT redistributable;
 * see COMPLIANCE.md).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMS_ROMS_EMBEDDED_H
#define SMS_ROMS_EMBEDDED_H

#include <stdint.h>

typedef struct {
    const char *file;          /* MAME's file name: "mpr-10052.rom" */
    const uint8_t *data;
    uint32_t size;
} sms_embedded_rom;

extern const sms_embedded_rom sms_embedded_roms[];
extern const unsigned sms_embedded_rom_count;

#endif /* SMS_ROMS_EMBEDDED_H */
