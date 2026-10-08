/*
 * Tiny Master System images built in memory, so no test depends on a ROM
 * file.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SMS_TEST_ROM_H
#define SMS_TEST_ROM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What test_rom_write builds. */
enum {
    TEST_ROM_COUNTER = 0,   /* 32K, Sega header: the counter program below */
    TEST_ROM_TOO_BIG,       /* 1 MB + 16K: more than the cartridge's SRAM */
};

/* The counter program:
 *     $0000  DI / LD SP,$DFF0 / IM 1
 *     $0005  loop: IN A,($DC) / LD ($C012),A     joypad 1 as the console reads it
 *                  LD HL,$C010 / INC (HL)        a counter that always moves
 *                  JR loop
 *     $0066  NMI (Pause): PUSH HL / LD HL,$C011 / INC (HL) / POP HL / RETN
 * Returns the file size written, or 0. */
static inline size_t test_rom_write(const char *path, int kind)
{
    static const uint8_t code[] = {
        0xF3, 0x31, 0xF0, 0xDF, 0xED, 0x56,
        0xDB, 0xDC, 0x32, 0x12, 0xC0,
        0x21, 0x10, 0xC0, 0x34,
        0x18, 0xF5,
    };
    static const uint8_t nmi[] = {
        0xE5, 0x21, 0x11, 0xC0, 0x34, 0xE1, 0xED, 0x45,
    };
    const size_t size = kind == TEST_ROM_TOO_BIG ? 0x100000 + 0x4000 : 0x8000;
    uint8_t *img = (uint8_t *)malloc(size);
    FILE *f;
    size_t n = 0;

    if (!img)
        return 0;
    memset(img, 0xff, size);
    memcpy(img, code, sizeof code);
    memcpy(img + 0x66, nmi, sizeof nmi);
    memcpy(img + 0x7ff0, "TMR SEGA", 8);
    memset(img + 0x7ff8, 0, 8);
    f = fopen(path, "wb");
    if (f) {
        n = fwrite(img, 1, size, f);
        fclose(f);
    }
    free(img);
    return n == size ? n : 0;
}

#endif /* SMS_TEST_ROM_H */
