/*
 * vdpview -- the debugger's pictures of the 315-5124/5246: pure functions
 * over a snapshot, so they are testable without a running machine
 * (core/tests/vdp_decode_test.c) and safe to call from the UI thread.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMS_VDPVIEW_H
#define SMS_VDPVIEW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t vram[0x4000];
    uint8_t cram[32];
    uint8_t reg[16];
    int mode;                  /* 0-4 */
    int y_pixels;              /* 192/224/240 */
    int kind_5246;
    uint32_t pens[80];         /* the VDP's palette: 0-63 mode 4, 64-79 TMS */
} smsvdp_snap;

/* The table bases the beam is using, decoded from the registers. */
typedef struct {
    uint16_t name, sat, sprite_pattern;    /* mode 4 */
    uint16_t color, pattern, sprite_attr;  /* TMS modes (sprite_attr also mode 4's) */
    int name_rows;                         /* 28 or 32 (mode 4); 24 (TMS) */
} smsvdp_tables;

void smsvdp_get_tables(const smsvdp_snap *s, smsvdp_tables *out);
const char *smsvdp_mode_name(int mode, int y_pixels);
/* The colour of CRAM entry i (mode 4) or of TMS colour i (modes 0-3). */
uint32_t smsvdp_color(const smsvdp_snap *s, int i);

/* One line describing register reg (0-10); returns length. */
int smsvdp_describe_register(const smsvdp_snap *s, int reg, char *dst, int dstsz);

/* The renderers (see smsdebug.h's SMSDEBUG_VIEW_*). Each writes up to 256x256
 * XRGB pixels and its size; `accent` outlines the visible window. */
void smsvdp_render_nametable(const smsvdp_snap *s, uint32_t *dst, int *w, int *h,
                             uint32_t accent);
void smsvdp_render_tiles(const smsvdp_snap *s, int palette, uint32_t *dst, int *w, int *h);
void smsvdp_render_sprites(const smsvdp_snap *s, uint32_t *dst, int *w, int *h,
                           uint32_t accent);
void smsvdp_render_palette(const smsvdp_snap *s, uint32_t *dst, int *w, int *h);

/* The sprite table as a list. Returns the count (64 mode 4, 32 TMS); an
 * entry past the table's terminator ($D0 in 192-line modes) is not visible. */
typedef struct { int y, x, tile, color, early_clock, visible; } smsvdp_sprite;
int smsvdp_sprites(const smsvdp_snap *s, smsvdp_sprite out[64]);

#ifdef __cplusplus
}
#endif

#endif /* SMS_VDPVIEW_H */
