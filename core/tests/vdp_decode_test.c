/*
 * vdp_decode_test -- the debugger's VDP views (core/debugger/vdpview.c) on
 * snapshots built here: the table bases each mode decodes from the
 * registers (mode 4 at 192 and 224 lines, Graphics I and II), mode 4 tile
 * planes, name-table flips and the sprite palette bit, the sprite table's
 * terminator (only in 192-line modes), the sprite shift and the TMS early
 * clock, the register descriptions, and that a sprite shows on the line
 * after its Y.
 *
 * The pens are the identity (pen i is the colour value i), so every pixel
 * names the CRAM entry or TMS colour it came from.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "vdpview.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static smsvdp_snap s;
static uint32_t px[256 * 256];

static void reset(int mode, int y_pixels)
{
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 80; i++)
        s.pens[i] = (uint32_t)i;
    s.mode = mode;
    s.y_pixels = y_pixels;
    s.kind_5246 = 1;
}

static void tables(void)
{
    smsvdp_tables t;
    char line[96];

    reset(4, 192);
    s.reg[2] = 0xff;
    s.reg[5] = 0xff;
    s.reg[6] = 0xff;
    smsvdp_get_tables(&s, &t);
    check(t.name == 0x3800 && t.name_rows == 28, "mode 4, 192 lines: name table $3800, 28 rows");
    check(t.sat == 0x3f00, "sprite attributes $3F00");
    check(t.sprite_pattern == 0x2000, "sprite patterns $2000 (R6 bit 2)");
    smsvdp_describe_register(&s, 2, line, sizeof line);
    check(strcmp(line, "R2 FF: name table at $3800") == 0, "R2 is described with its base");

    reset(4, 224);
    s.reg[2] = 0xff;
    smsvdp_get_tables(&s, &t);
    check(t.name == 0x3700 && t.name_rows == 32, "mode 4, 224 lines: name table $3700, 32 rows");
    check(strcmp(smsvdp_mode_name(4, 224), "Mode 4, 224 lines") == 0, "mode 4 at 224 lines is named");

    reset(2, 192);
    s.reg[2] = 0x0e;
    s.reg[3] = 0xff;
    s.reg[4] = 0x03;
    s.reg[5] = 0x36;
    s.reg[6] = 0x07;
    smsvdp_get_tables(&s, &t);
    check(t.name == 0x3800 && t.name_rows == 24, "Graphics II: name table $3800, 24 rows");
    check(t.color == 0x2000 && t.pattern == 0x0000, "Graphics II: colour $2000, patterns $0000");
    check(t.sprite_attr == 0x1b00 && t.sprite_pattern == 0x3800, "TMS sprite tables $1B00, $3800");

    reset(0, 192);
    s.reg[3] = 0x80;
    s.reg[4] = 0x01;
    smsvdp_get_tables(&s, &t);
    check(t.color == 0x2000 && t.pattern == 0x0800, "Graphics I: colour $2000, patterns $0800");
    smsvdp_describe_register(&s, 4, line, sizeof line);
    check(strcmp(line, "R4 01: pattern table at $0800") == 0, "R4 is described in TMS modes");
}

static void mode4_pixels(void)
{
    int w, h;

    reset(4, 192);
    s.reg[2] = 0xff;   /* name table $3800 */
    /* tile 1 row 0: planes 0, 1 and 3 set in the leftmost pixel -> 11 */
    s.vram[32 + 0] = 0x80;
    s.vram[32 + 1] = 0x80;
    s.vram[32 + 3] = 0x80;
    s.cram[11] = 0x2a;
    s.cram[16 + 11] = 0x15;

    smsvdp_render_tiles(&s, 0, px, &w, &h);
    check(w == 256 && h == 128, "the tile sheet is 512 tiles, 256x128");
    check(px[8] == 0x2a && px[9] == 0, "tile 1's planes decode to colour 11 (palette 0)");
    smsvdp_render_tiles(&s, 1, px, &w, &h);
    check(px[8] == 0x15, "and to colour 27 under the sprite palette");

    /* name entries: (0,0) tile 1 flipped horizontally, (1,0) tile 1 on
     * the sprite palette */
    s.vram[0x3800] = 0x01;
    s.vram[0x3801] = 0x02;
    s.vram[0x3802] = 0x01;
    s.vram[0x3803] = 0x08;
    s.reg[9] = 16;     /* the window starts 16 lines down the map */
    smsvdp_render_nametable(&s, px, &w, &h, 0xffffff);
    check(w == 256 && h == 224, "the name table is 32x28 tiles");
    check(px[7] == 0x2a && px[1] == 0, "bit 9 flips the tile horizontally");
    check(px[8] == 0x15, "bit 11 takes the sprite palette");
    check(px[16 * 256 + 100] == 0xffffff && px[15 * 256 + 100] != 0xffffff,
          "the visible window is outlined where R9 puts it");
}

static void sprites(void)
{
    smsvdp_sprite spr[64];
    int n, w, h;

    reset(4, 192);
    s.reg[5] = 0xff;   /* SAT $3F00 */
    s.vram[0x3f00] = 10;
    s.vram[0x3f01] = 20;
    s.vram[0x3f02] = 0xd0;
    s.vram[0x3f03] = 30;
    s.vram[0x3f80] = 0x40;
    s.vram[0x3f81] = 1;
    n = smsvdp_sprites(&s, spr);
    check(n == 64, "mode 4 lists 64 sprites");
    check(spr[0].x == 0x40 && spr[0].y == 10 && spr[0].tile == 1, "sprite 0's X, Y and tile");
    check(spr[1].visible && !spr[2].visible && !spr[3].visible,
          "$D0 ends the list at 192 lines");

    s.reg[0] = 0x08;   /* shift sprites left 8 */
    s.reg[6] = 0x04;   /* patterns from the upper 256 tiles */
    n = smsvdp_sprites(&s, spr);
    check(spr[0].x == 0x38 && spr[0].tile == 257, "R0 bit 3 shifts X; R6 bit 2 adds 256 to the tile");

    s.y_pixels = 224;
    n = smsvdp_sprites(&s, spr);
    check(spr[2].visible && spr[3].visible, "$D0 is an ordinary Y at 224 lines");

    /* a sprite shows on the line after its Y */
    reset(4, 192);
    s.reg[5] = 0xff;
    s.vram[0x3f00] = 10;
    s.vram[0x3f01] = 0xd0;
    s.vram[0x3f80] = 0x40;
    s.vram[0x3f81] = 1;
    s.vram[32 + 0] = 0x80;   /* tile 1, row 0, leftmost: colour 1 */
    s.cram[16 + 1] = 0x33;
    smsvdp_render_sprites(&s, px, &w, &h, 0);
    check(px[11 * 256 + 0x40] == 0x33 && px[10 * 256 + 0x40] != 0x33,
          "sprite 0 draws from line Y+1 in the sprite palette");

    /* TMS: 32 sprites, $D0 terminates, the early clock bit */
    reset(2, 192);
    s.reg[5] = 0x36;   /* $1B00 */
    s.vram[0x1b00] = 50; s.vram[0x1b01] = 60; s.vram[0x1b02] = 7; s.vram[0x1b03] = 0x8f;
    s.vram[0x1b04] = 0xd0;
    n = smsvdp_sprites(&s, spr);
    check(n == 32, "TMS lists 32 sprites");
    check(spr[0].color == 15 && spr[0].early_clock && spr[0].tile == 7, "colour, early clock and name");
    check(spr[0].visible && !spr[1].visible, "$D0 ends the TMS list");
}

int main(void)
{
    tables();
    mode4_pixels();
    sprites();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
