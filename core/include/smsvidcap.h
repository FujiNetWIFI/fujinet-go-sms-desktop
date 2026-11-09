/*
 * smsvidcap -- a finished frame as the machine left it, for the Video tab
 * (smsvidtab.h). Taken once per frame on the machine thread while a tab
 * exists (sms_host_capture_*), so a reader sees one consistent frame while
 * the game runs.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SMSVIDCAP_H
#define SMSVIDCAP_H

#include <stdint.h>

#include "smsvidline.h"
#include "vdpview.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMSVID_FB_W     268     /* SMS_FB_WIDTH: 2 border + 256 active + 10 border */
#define SMSVID_FB_MAXH  240     /* SMS_FB_MAX_HEIGHT */
#define SMSVID_ACTIVE_X 2       /* active display starts at this fb column */

typedef struct smsvid_capture {
    smsvdp_snap snap;                       /* VRAM, CRAM, R0-R10, mode, pens */
    smsvid_line lines[SMSVID_FB_MAXH];      /* per fb row */
    uint32_t fb[SMSVID_FB_W * SMSVID_FB_MAXH];  /* XRGB */
    int fb_height;                          /* 224 NTSC / 240 PAL */
    int is_pal, total_lines;                /* 262 / 313 */
    int beam_row;                           /* fb rows drawn this frame so far (fb_height: all) */
    int vpos, hpos, vcount, hcount;         /* the beam, and what $7E / the H counter read */
    uint8_t line_counter, status;
    uint8_t int_line, vint_pending, hint_pending;
    uint64_t cycles;
    uint64_t frame;                         /* frames the VDP has completed */
    uint64_t serial;                        /* 0: nothing captured yet */
} smsvid_capture;

#ifdef __cplusplus
}
#endif

#endif
