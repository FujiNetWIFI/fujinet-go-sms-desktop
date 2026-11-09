/*
 * smsvidline -- what the VDP drew one screen row with. The VDP records one
 * per row as it draws it (core/sms/vdp.c), so the debugger's Video tab can
 * show the frame the way the TV showed it when a game changes the scroll
 * registers, the palette or the mode between lines. No dependencies.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SMSVIDLINE_H
#define SMSVIDLINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMSVID_FLAG_BLANK   1u  /* the display was off for this row */
#define SMSVID_FLAG_INT     2u  /* /INT was asserted when the row was drawn */

typedef struct {
    uint8_t reg[11];            /* R0-R10 when the row was drawn */
    uint8_t x_scroll, y_scroll; /* the scroll values latched for the row (R8 / R9 as used) */
    uint8_t flags;              /* SMSVID_FLAG_* */
    uint8_t mode;               /* 0-3 TMS9918, 4 */
    uint8_t line_counter;       /* the line counter after this row's count */
    uint8_t valid;              /* the VDP has drawn this row since power-on */
    uint16_t y_pixels;          /* 192, 224, 240 */
    int16_t line;               /* active display line this row is (0..y_pixels-1); <0 above, >= below */
    uint8_t cram[32];           /* colour RAM when the row was drawn */
} smsvid_line;

#ifdef __cplusplus
}
#endif

#endif
