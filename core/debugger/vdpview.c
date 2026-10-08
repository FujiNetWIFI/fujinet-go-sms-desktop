/*
 * vdpview.c -- see vdpview.h. The table decoding follows the renderer
 * (core/sms/vdp.c, itself MAME's 315_5124.cpp); the 315-5124's address
 * masks are left out here on purpose -- the views show the tables as the
 * registers name them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "vdpview.h"

#define BIT(x, n) (((x) >> (n)) & 1)

static uint8_t vr(const smsvdp_snap *s, unsigned a)
{
    return s->vram[a & 0x3fff];
}

void smsvdp_get_tables(const smsvdp_snap *s, smsvdp_tables *t)
{
    memset(t, 0, sizeof *t);
    if (s->mode == 4)
    {
        if (s->y_pixels != 192)
        {
            t->name = (uint16_t)(((s->reg[2] & 0x0c) << 10) | 0x0700);
            t->name_rows = 32;
        }
        else
        {
            t->name = (uint16_t)((s->reg[2] << 10) & 0x3800);
            t->name_rows = 28;
        }
        t->sat = (uint16_t)((s->reg[5] << 7) & 0x3f00);
        t->sprite_attr = t->sat;
        t->sprite_pattern = (uint16_t)(BIT(s->reg[6], 2) ? 0x2000 : 0x0000);
        return;
    }
    t->name = (uint16_t)((s->reg[2] & 0x0f) << 10);
    t->name_rows = 24;
    if (s->mode == 2)
    {
        t->color = (uint16_t)((s->reg[3] & 0x80) << 6);
        t->pattern = (uint16_t)((s->reg[4] & 0x04) << 11);
    }
    else
    {
        t->color = (uint16_t)((s->reg[3] << 6) & 0x3fff);
        t->pattern = (uint16_t)((s->reg[4] << 11) & 0x3fff);
    }
    t->sprite_attr = (uint16_t)((s->reg[5] & 0x7f) << 7);
    t->sprite_pattern = (uint16_t)((s->reg[6] & 0x07) << 11);
}

const char *smsvdp_mode_name(int mode, int y_pixels)
{
    switch (mode)
    {
    case 0: return "Graphics I (TMS mode 0)";
    case 1: return "Text (TMS mode 1)";
    case 2: return "Graphics II (TMS mode 2)";
    case 3: return "Multicolor (TMS mode 3)";
    case 4:
        return y_pixels == 224 ? "Mode 4, 224 lines"
             : y_pixels == 240 ? "Mode 4, 240 lines" : "Mode 4";
    default: return "?";
    }
}

uint32_t smsvdp_color(const smsvdp_snap *s, int i)
{
    if (s->mode == 4)
        return s->pens[s->cram[i & 31] & 0x3f];
    return s->pens[64 + (i & 15)];
}

int smsvdp_describe_register(const smsvdp_snap *s, int reg, char *dst, int dstsz)
{
    const uint8_t v = s->reg[reg & 15];
    smsvdp_tables t;

    smsvdp_get_tables(s, &t);
    switch (reg)
    {
    case 0:
        return snprintf(dst, (size_t)dstsz,
                        "R0 %02X: %s%s%s%s%s%s%s", v,
                        BIT(v, 7) ? "vscroll-lock-right " : "",
                        BIT(v, 6) ? "hscroll-lock-top " : "",
                        BIT(v, 5) ? "blank-left-column " : "",
                        BIT(v, 4) ? "line-IRQ " : "",
                        BIT(v, 3) ? "sprite-shift " : "",
                        BIT(v, 2) ? "M4 " : "",
                        BIT(v, 1) ? "M2" : "");
    case 1:
        return snprintf(dst, (size_t)dstsz,
                        "R1 %02X: display %s, VINT %s%s%s%s%s", v,
                        BIT(v, 6) ? "on" : "off", BIT(v, 5) ? "on" : "off",
                        BIT(v, 4) ? ", M1" : "", BIT(v, 3) ? ", M3" : "",
                        BIT(v, 1) ? ", 16-line sprites" : ", 8-line sprites",
                        BIT(v, 0) ? ", zoomed" : "");
    case 2:
        return snprintf(dst, (size_t)dstsz, "R2 %02X: name table at $%04X", v, t.name);
    case 3:
        return snprintf(dst, (size_t)dstsz, s->mode == 4 ? "R3 %02X: color table (unused in mode 4)"
                                                          : "R3 %02X: color table at $%04X",
                        v, t.color);
    case 4:
        return snprintf(dst, (size_t)dstsz, s->mode == 4 ? "R4 %02X: pattern table (unused in mode 4)"
                                                          : "R4 %02X: pattern table at $%04X",
                        v, t.pattern);
    case 5:
        return snprintf(dst, (size_t)dstsz, "R5 %02X: sprite attributes at $%04X", v, t.sprite_attr);
    case 6:
        return snprintf(dst, (size_t)dstsz, "R6 %02X: sprite patterns at $%04X", v, t.sprite_pattern);
    case 7:
        return snprintf(dst, (size_t)dstsz, s->mode == 4 ? "R7 %02X: backdrop sprite colour %d"
                                                          : "R7 %02X: backdrop colour %d",
                        v, v & 15);
    case 8:
        return snprintf(dst, (size_t)dstsz, "R8 %02X: horizontal scroll %d", v, v);
    case 9:
        return snprintf(dst, (size_t)dstsz, "R9 %02X: vertical scroll %d", v, v);
    case 10:
        return snprintf(dst, (size_t)dstsz, "R10 %02X: line interrupt every %d lines", v, v + 1);
    default:
        return snprintf(dst, (size_t)dstsz, "R%d %02X", reg, v);
    }
}

/* An 8x8 mode 4 tile row: 4 planes, 1 byte each. */
static void tile4_row(const smsvdp_snap *s, int tile, int row, uint8_t px[8])
{
    const unsigned base = (unsigned)(tile & 0x1ff) * 32 + (unsigned)(row & 7) * 4;
    const uint8_t p0 = vr(s, base), p1 = vr(s, base + 1), p2 = vr(s, base + 2), p3 = vr(s, base + 3);
    for (int x = 0; x < 8; x++)
        px[x] = (uint8_t)(BIT(p0, 7 - x) | (BIT(p1, 7 - x) << 1) | (BIT(p2, 7 - x) << 2) |
                          (BIT(p3, 7 - x) << 3));
}

static void outline(uint32_t *dst, int w, int h, int x0, int y0, int rw, int rh, uint32_t c)
{
    for (int i = 0; i < rw; i++)
    {
        int x = (x0 + i) % w;
        dst[((y0) % h) * w + x] = c;
        dst[((y0 + rh - 1) % h) * w + x] = c;
    }
    for (int j = 0; j < rh; j++)
    {
        int y = (y0 + j) % h;
        dst[y * w + (x0 % w)] = c;
        dst[y * w + ((x0 + rw - 1) % w)] = c;
    }
}

void smsvdp_render_nametable(const smsvdp_snap *s, uint32_t *dst, int *w, int *h, uint32_t accent)
{
    smsvdp_tables t;
    smsvdp_get_tables(s, &t);

    if (s->mode == 4)
    {
        const int rows = t.name_rows;
        *w = 256;
        *h = rows * 8;
        for (int ty = 0; ty < rows; ty++)
            for (int tx = 0; tx < 32; tx++)
            {
                const unsigned a = t.name + (unsigned)(ty * 64 + tx * 2);
                const uint16_t e = (uint16_t)(vr(s, a) | (vr(s, a + 1) << 8));
                const int tile = e & 0x1ff, hf = BIT(e, 9), vf = BIT(e, 10), pal = BIT(e, 11);
                for (int y = 0; y < 8; y++)
                {
                    uint8_t px[8];
                    tile4_row(s, tile, vf ? 7 - y : y, px);
                    for (int x = 0; x < 8; x++)
                    {
                        const int c = px[hf ? 7 - x : x] | (pal << 4);
                        dst[(ty * 8 + y) * 256 + tx * 8 + x] = smsvdp_color(s, c);
                    }
                }
            }
        /* the window the screen shows: horizontal scroll moves the map right */
        {
            const int xs = s->reg[8], ys = s->reg[9];
            outline(dst, 256, *h, (256 - xs) & 255, ys % (*h), 256, s->y_pixels, accent);
        }
        return;
    }

    /* TMS modes */
    *h = 192;
    if (s->mode == 1)
    {
        *w = 240;
        for (int ty = 0; ty < 24; ty++)
            for (int tx = 0; tx < 40; tx++)
            {
                const uint8_t name = vr(s, t.name + (unsigned)(ty * 40 + tx));
                for (int y = 0; y < 8; y++)
                {
                    const uint8_t pat = vr(s, t.pattern + name * 8u + (unsigned)y);
                    for (int x = 0; x < 6; x++)
                        dst[(ty * 8 + y) * 240 + tx * 6 + x] =
                            smsvdp_color(s, BIT(pat, 7 - x) ? (s->reg[7] >> 4) : (s->reg[7] & 15));
                }
            }
        return;
    }
    *w = 256;
    for (int ty = 0; ty < 24; ty++)
        for (int tx = 0; tx < 32; tx++)
        {
            const uint8_t name = vr(s, t.name + (unsigned)(ty * 32 + tx));
            for (int y = 0; y < 8; y++)
            {
                const int line = ty * 8 + y;
                uint8_t pat, col;
                if (s->mode == 2)
                {
                    const int pattern_mask = ((s->reg[4] & 0x03) << 8) | 0xff;
                    const int color_mask = ((s->reg[3] & 0x7f) << 3) | 0x07;
                    const int off = (line & 0xc0) << 2;
                    pat = vr(s, t.pattern + (unsigned)(((off + name) & pattern_mask) * 8 + (line & 7)));
                    col = vr(s, t.color + (unsigned)(((off + name) & color_mask) * 8 + (line & 7)));
                }
                else if (s->mode == 3)
                {
                    pat = vr(s, t.pattern + name * 8u + (unsigned)(((line >> 3) & 3) << 1) + (unsigned)((line & 4) >> 2));
                    col = 0;
                }
                else
                {
                    pat = vr(s, t.pattern + name * 8u + (unsigned)(line & 7));
                    col = vr(s, t.color + (unsigned)(name >> 3));
                }
                for (int x = 0; x < 8; x++)
                {
                    int c;
                    if (s->mode == 3)
                        c = (pat >> (~x & 4)) & 0x0f;
                    else
                        c = BIT(pat, 7 - x) ? (col >> 4) : (col & 15);
                    if (!c)
                        c = s->reg[7] & 15;
                    dst[line * 256 + tx * 8 + x] = smsvdp_color(s, c);
                }
            }
        }
}

void smsvdp_render_tiles(const smsvdp_snap *s, int palette, uint32_t *dst, int *w, int *h)
{
    smsvdp_tables t;
    smsvdp_get_tables(s, &t);
    *w = 256;

    if (s->mode == 4)
    {
        *h = 128;
        for (int tile = 0; tile < 512; tile++)
        {
            const int tx = tile % 32, ty = tile / 32;
            for (int y = 0; y < 8; y++)
            {
                uint8_t px[8];
                tile4_row(s, tile, y, px);
                for (int x = 0; x < 8; x++)
                    dst[(ty * 8 + y) * 256 + tx * 8 + x] = smsvdp_color(s, px[x] | ((palette & 1) << 4));
            }
        }
        return;
    }

    /* TMS: the pattern table, white on black (Graphics II with its colours) */
    {
        const int n = s->mode == 2 ? 768 : 256;
        *h = n / 32 * 8;
        for (int p = 0; p < n; p++)
        {
            const int tx = p % 32, ty = p / 32;
            for (int y = 0; y < 8; y++)
            {
                const uint8_t pat = vr(s, t.pattern + (unsigned)p * 8u + (unsigned)y);
                const uint8_t col = s->mode == 2 ? vr(s, t.color + (unsigned)p * 8u + (unsigned)y) : 0xf1;
                for (int x = 0; x < 8; x++)
                {
                    int c = BIT(pat, 7 - x) ? (col >> 4) : (col & 15);
                    dst[(ty * 8 + y) * 256 + tx * 8 + x] = smsvdp_color(s, c ? c : 1);
                }
            }
        }
    }
}

int smsvdp_sprites(const smsvdp_snap *s, smsvdp_sprite out[64])
{
    smsvdp_tables t;
    int visible = 1;

    smsvdp_get_tables(s, &t);
    memset(out, 0, 64 * sizeof *out);
    if (s->mode == 4)
    {
        const unsigned extra = t.sat + 0x80;
        for (int i = 0; i < 64; i++)
        {
            int y = vr(s, t.sat + (unsigned)i);
            if (s->y_pixels == 192 && y == 0xd0)
                visible = 0;
            out[i].y = y;
            out[i].x = vr(s, extra + (unsigned)i * 2u) - (BIT(s->reg[0], 3) ? 8 : 0);
            out[i].tile = vr(s, extra + (unsigned)i * 2u + 1u) + (BIT(s->reg[6], 2) ? 256 : 0);
            out[i].visible = visible;
        }
        return 64;
    }
    for (int i = 0; i < 32; i++)
    {
        const unsigned a = t.sprite_attr + (unsigned)i * 4u;
        int y = vr(s, a);
        if (y == 0xd0)
            visible = 0;
        out[i].y = y;
        out[i].x = vr(s, a + 1);
        out[i].tile = vr(s, a + 2);
        out[i].color = vr(s, a + 3) & 15;
        out[i].early_clock = BIT(vr(s, a + 3), 7);
        out[i].visible = visible;
    }
    return 32;
}

void smsvdp_render_sprites(const smsvdp_snap *s, uint32_t *dst, int *w, int *h, uint32_t accent)
{
    smsvdp_sprite spr[64];
    smsvdp_tables t;
    const int n = smsvdp_sprites(s, spr);
    const int tall = BIT(s->reg[1], 1) ? 16 : 8;
    const int zoom = BIT(s->reg[1], 0) ? 2 : 1;
    const uint32_t back = smsvdp_color(s, s->mode == 4 ? 0x10 + (s->reg[7] & 15) : (s->reg[7] & 15));

    smsvdp_get_tables(s, &t);
    *w = 256;
    *h = 256;
    for (int i = 0; i < 256 * 256; i++)
        dst[i] = back;

    /* back to front, as the VDP draws them (lower index wins) */
    for (int i = n - 1; i >= 0; i--)
    {
        int y0 = spr[i].y;
        if (!spr[i].visible)
            continue;
        if (y0 >= 240)
            y0 -= 256;
        y0 += 1;   /* a sprite shows on the line after its Y */
        if (s->mode == 4)
        {
            for (int row = 0; row < tall * zoom; row++)
            {
                const int line = row / zoom;
                int tile = spr[i].tile;
                uint8_t px[8];
                if (tall == 16)
                    tile = (tile & 0x1fe) + (line >> 3);
                tile4_row(s, tile, line & 7, px);
                for (int x = 0; x < 8 * zoom; x++)
                {
                    const int c = px[x / zoom];
                    const int sx = spr[i].x + x, sy = y0 + row;
                    if (c && sx >= 0 && sx < 256 && sy >= 0 && sy < 256)
                        dst[sy * 256 + sx] = smsvdp_color(s, c | 0x10);
                }
            }
        }
        else
        {
            const int size = tall;   /* 8 or 16 pixels */
            const int x0 = spr[i].x - (spr[i].early_clock ? 32 : 0);
            int tile = spr[i].tile;
            if (size == 16)
                tile &= 0xfc;
            for (int row = 0; row < size * zoom; row++)
            {
                const int line = row / zoom;
                for (int x = 0; x < size * zoom; x++)
                {
                    const int col = x / zoom;
                    const int quad = (col >= 8 ? 2 : 0) + (line >= 8 ? 1 : 0);
                    const uint8_t pat = vr(s, t.sprite_pattern + (unsigned)((tile + (size == 16 ? quad : 0)) * 8 + (line & 7)));
                    const int sx = x0 + x, sy = y0 + row;
                    if (spr[i].color && BIT(pat, 7 - (col & 7)) && sx >= 0 && sx < 256 && sy >= 0 && sy < 256)
                        dst[sy * 256 + sx] = smsvdp_color(s, spr[i].color);
                }
            }
        }
    }
    outline(dst, 256, 256, 0, 0, 256, s->mode == 4 ? s->y_pixels : 192, accent);
}

void smsvdp_render_palette(const smsvdp_snap *s, uint32_t *dst, int *w, int *h)
{
    *w = 256;
    *h = 32;
    for (int i = 0; i < 32; i++)
    {
        uint32_t c;
        if (s->mode == 4)
            c = smsvdp_color(s, i);
        else
            c = i < 16 ? smsvdp_color(s, i) : 0x202020;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                dst[((i / 16) * 16 + y) * 256 + (i % 16) * 16 + x] =
                    (x == 15 || y == 15) ? 0x000000 : c;
    }
}
