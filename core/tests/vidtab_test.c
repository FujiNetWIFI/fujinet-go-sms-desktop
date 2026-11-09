/*
 * vidtab_test -- the Video tab's controller (core/debugger/vidtab.c) fed
 * synthetic frames: no machine, no ROMs. Mode 4 decoded row by row with a
 * mid-frame horizontal scroll split, a mid-frame palette change, a vertical
 * scroll, a sprite; Display / Beam / Values; the inspector's addresses; the
 * Lines table; register ages; the TMS9918 fallback; and a fuzz over random
 * captures, every view, source, overlay and zoom.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smsvidcap.h"
#include "smsvidtab.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); fails++; } \
                         else printf("ok: %s\n", m); } while (0)

static uint32_t pal_color(int i)
{
    return (uint32_t)((i * 37) & 255) << 16 | (uint32_t)((i * 11 + 5) & 255) << 8 | (uint32_t)(i * 7 + 1 & 255);
}

#define ACT_Y0 16

static smsvid_capture cap;
static uint32_t live[SMSVID_FB_W * SMSVID_FB_MAXH];

static uint32_t expect(const smsvid_capture *c, int row, int idx)
{
    return (c->snap.pens[c->lines[row].cram[idx] & 0x3f]) | 0xff000000u;
}

static void make_cap(smsvid_capture *c, int ypix, int fbh)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 80; i++) c->snap.pens[i] = pal_color(i);
    for (int i = 0; i < 32; i++) c->snap.cram[i] = (uint8_t)i;
    c->snap.mode = 4;
    c->snap.y_pixels = ypix;
    c->snap.kind_5246 = 1;
    c->snap.reg[0] = 0x04; c->snap.reg[1] = 0xE0; c->snap.reg[2] = 0xFF;
    c->snap.reg[5] = 0x7E; c->snap.reg[7] = 0x00; c->snap.reg[10] = 0xFF;
    c->fb_height = fbh;
    c->total_lines = 262;
    c->beam_row = fbh;
    c->vpos = 224;
    c->serial = 1;
    c->frame = 5;
    for (int y = 0; y < fbh; y++) {
        smsvid_line *L = &c->lines[y];
        memcpy(L->reg, c->snap.reg, sizeof L->reg);
        memcpy(L->cram, c->snap.cram, sizeof L->cram);
        L->mode = 4; L->valid = 1; L->y_pixels = (uint16_t)ypix;
        L->line = (int16_t)(y - ACT_Y0);
        L->line_counter = 0xFF;
    }
    for (int i = 0; i < SMSVID_FB_W * fbh; i++) c->fb[i] = 0x00123456u;
    /* tile 1: plane 0 = $F0 on every row; tile 2: plane 1 = $0F */
    for (int r = 0; r < 8; r++) { c->snap.vram[32 + r * 4] = 0xF0; c->snap.vram[64 + r * 4 + 1] = 0x0F; }
    c->snap.vram[0x3800] = 1;                       /* cell 0,0 = tile 1 */
    c->snap.vram[0x3800 + 13 * 64 + 31 * 2] = 2;    /* cell 31,13 = tile 2 */
    c->snap.vram[0x3F00] = 0xD0;                    /* no sprites */
}

static void settle(smsvid_tab *t, int view)
{
    smsvid_tab_set_view(t, view);
    smsvid_tab_refresh(t, 1);
}

int main(void)
{
    smsvid_tab *t;
    const uint32_t *pic;
    int w, h;

    setenv("SMS_DEBUGGER_VIDEO_VIEW", "palette", 1);
    setenv("SMS_DEBUGGER_VIDEO_OVERLAYS", "grid,hint", 1);
    setenv("SMS_DEBUGGER_VIDEO_ZOOM", "3", 1);
    setenv("SMS_DEBUGGER_VIDEO_SECTION", "tim", 1);
    t = smsvid_tab_new(NULL);
    CHECK(t && smsvid_tab_view(t) == SMSVID_V_DECODED && smsvid_tab_source(t) == SMSVID_D_PALETTE &&
          smsvid_tab_zoom(t) == 3 && smsvid_tab_overlays(t) == (SMSVID_OV_GRID | SMSVID_OV_HINT) &&
          smsvid_tab_env_section(t) == SMSVID_S_TIMING, "env variables name the view, overlays, zoom, section");
    smsvid_tab_free(t);
    unsetenv("SMS_DEBUGGER_VIDEO_VIEW"); unsetenv("SMS_DEBUGGER_VIDEO_OVERLAYS");
    unsetenv("SMS_DEBUGGER_VIDEO_ZOOM"); unsetenv("SMS_DEBUGGER_VIDEO_SECTION");

    t = smsvid_tab_new(NULL);
    smsvid_tab_set_overlays(t, 0);
    smsvid_tab_set_zoom(t, 1);

    /* ---- Decoded: tiles, palette, no split ---- */
    make_cap(&cap, 192, 224);
    smsvid_tab_feed(t, &cap, NULL);
    settle(t, SMSVID_V_DECODED);
    pic = smsvid_tab_picture(t, &w, &h);
    CHECK(pic && w == 256 && h == 192, "Decoded is 256x192");
    CHECK(pic && pic[0] == expect(&cap, ACT_Y0, 1) && pic[3] == expect(&cap, ACT_Y0, 1), "tile 1 pixels 0-3 are colour 1");
    CHECK(pic && pic[4] == expect(&cap, ACT_Y0, 0), "tile 1 pixels 4-7 are colour 0");
    {
        int opaque = 1;
        for (int i = 0; pic && i < w * h; i++) if ((pic[i] >> 24) != 0xff) opaque = 0;
        CHECK(opaque, "every pixel opaque");
    }

    /* ---- mid-frame horizontal scroll split ---- */
    for (int y = ACT_Y0 + 100; y < 224; y++) cap.lines[y].x_scroll = 8;
    smsvid_tab_feed(t, &cap, NULL);
    smsvid_tab_refresh(t, 1);
    pic = smsvid_tab_picture(t, &w, &h);
    CHECK(pic && pic[(size_t)96 * w] == expect(&cap, ACT_Y0 + 96, 0), "above the split: name column 0 (empty)");
    {
        /* tile 2: plane1 = $0F -> pixels 4-7 are colour 2; screen column 0 shows name column 31 */
        CHECK(pic && pic[(size_t)104 * w + 5] == expect(&cap, ACT_Y0 + 104, 2), "below the split the map moved one tile right");
    }
    {
        const smsvid_line_row *rows;
        int n = smsvid_tab_lines(t, &rows);
        CHECK(n == 2 && rows[1].row == ACT_Y0 + 100 && (rows[1].changed_mask & SMSVID_LC_XSCROLL) && rows[1].line == 100,
              "the Lines table finds the scroll change at line 100");
    }

    /* ---- mid-frame palette change ---- */
    for (int y = ACT_Y0 + 150; y < 224; y++) cap.lines[y].cram[1] = 20;
    smsvid_tab_feed(t, &cap, NULL);
    smsvid_tab_refresh(t, 1);
    pic = smsvid_tab_picture(t, &w, &h);
    CHECK(pic && pic[(size_t)149 * w + 0 + 0] == expect(&cap, ACT_Y0 + 149, 0), "sanity row");
    {
        /* name cell (0,0) only covers lines 0-7: put tile 1 on a late row too */
        cap.snap.vram[0x3800 + 20 * 64 + 31 * 2] = 1;     /* cell 31,20 -> lines 160-167; scroll 8: screen col 0 */
        smsvid_tab_feed(t, &cap, NULL);
        smsvid_tab_refresh(t, 1);
        pic = smsvid_tab_picture(t, &w, &h);
        CHECK(pic && pic[(size_t)160 * w] == expect(&cap, ACT_Y0 + 160, 1) &&
              pic[(size_t)160 * w] == (pal_color(20) | 0xff000000u), "late rows use the late palette");
        {
            const smsvid_line_row *rows;
            int n = smsvid_tab_lines(t, &rows);
            CHECK(n == 3 && (rows[2].changed_mask & SMSVID_LC_CRAM), "the Lines table finds the palette change");
        }
    }

    /* ---- vertical scroll ---- */
    make_cap(&cap, 192, 224);
    for (int y = 0; y < 224; y++) cap.lines[y].y_scroll = 8;      /* one tile row up */
    cap.snap.vram[0x3800 + 1 * 64] = 1;                            /* cell 0,1 */
    smsvid_tab_feed(t, &cap, NULL);
    smsvid_tab_refresh(t, 1);
    pic = smsvid_tab_picture(t, &w, &h);
    CHECK(pic && pic[0] == expect(&cap, ACT_Y0, 1), "y scroll 8 shows name row 1 at the top");

    /* ---- sprite ---- */
    make_cap(&cap, 192, 224);
    cap.snap.vram[0x3F00] = 49;                     /* sprite 0 Y: lines 50-57 */
    cap.snap.vram[0x3F01] = 0xD0;
    cap.snap.vram[0x3F80] = 100;                    /* X */
    cap.snap.vram[0x3F81] = 1;                      /* tile 1 */
    smsvid_tab_feed(t, &cap, NULL);
    smsvid_tab_refresh(t, 1);
    pic = smsvid_tab_picture(t, &w, &h);
    CHECK(pic && pic[(size_t)50 * w + 100] == expect(&cap, ACT_Y0 + 50, 0x11), "the sprite draws in the sprite palette");
    CHECK(pic && pic[(size_t)50 * w + 104] == expect(&cap, ACT_Y0 + 50, 0), "its transparent half shows the background");
    CHECK(pic && pic[(size_t)49 * w + 100] == expect(&cap, ACT_Y0 + 49, 0), "not on the line above");
    smsvid_tab_hover(t, 101, 51);
    {
        const smsvid_pixel *p = smsvid_tab_pixel(t);
        CHECK(p && p->active && p->sprite == 0 && p->cram_index == 0x11 && p->pattern_addr == 32 + 4,
              "the inspector names the sprite, CRAM entry and pattern address");
        CHECK(p && strstr(p->text, "CRAM[17]") && strstr(p->text, "pattern row at $0024"), "...in its text");
    }

    /* ---- Values, overlays all on (no crash), swatch uses ---- */
    settle(t, SMSVID_V_VALUES);
    pic = smsvid_tab_picture(t, &w, &h);
    CHECK(pic && w == 256 && h == 192, "Values is the screen size");
    smsvid_tab_set_overlays(t, SMSVID_OV_ALL);
    smsvid_tab_set_zoom(t, 4);
    settle(t, SMSVID_V_DECODED);
    pic = smsvid_tab_picture(t, &w, &h);
    CHECK(pic && w == 1024 && h == 768, "zoom 4 with every overlay");
    smsvid_tab_set_overlays(t, 0);
    smsvid_tab_set_zoom(t, 1);
    smsvid_tab_refresh(t, 1);
    {
        const smsvid_swatch *sw;
        long sum = 0;
        int n = smsvid_tab_swatches(t, &sw);
        for (int i = 0; i < n; i++) sum += sw[i].uses;
        CHECK(n == 32 && sum == 256 * 192, "32 swatches whose uses add up to every pixel");
    }

    /* ---- Display / Beam ---- */
    settle(t, SMSVID_V_DISPLAY);
    pic = smsvid_tab_picture(t, &w, &h);
    CHECK(pic && w == 268 && h == 224 && pic[0] == 0xff123456u, "Display is the captured frame");
    smsvid_tab_hover(t, SMSVID_ACTIVE_X + 0, ACT_Y0 + 0);
    {
        const smsvid_pixel *p = smsvid_tab_pixel(t);
        CHECK(p && p->active && p->tile == 1 && p->name_addr == 0x3800 && p->pattern_addr == 32 &&
              p->cram_index == 1 && p->column == 0 && p->line == 0 && p->row == ACT_Y0,
              "Display inspector: tile, name table address, pattern address, CRAM entry");
    }
    smsvid_tab_hover(t, 0, 0);
    CHECK(smsvid_tab_pixel(t) && !smsvid_tab_pixel(t)->active, "a border pixel is not active");
    CHECK(smsvid_tab_break_on_pixel(t) == -1, "no debugger: no breakpoints");
    smsvid_tab_pin(t, 10, 20);
    CHECK(smsvid_tab_pixel(t)->x == 10, "pin");
    smsvid_tab_pin(t, 10, 20);
    smsvid_tab_unpin(t);

    settle(t, SMSVID_V_BEAM);
    CHECK(smsvid_tab_picture(t, &w, &h) == NULL && strlen(smsvid_tab_why(t)) > 10, "Beam needs a stopped machine and says why");
    for (int i = 0; i < SMSVID_FB_W * 224; i++) live[i] = 0x0000ff00u;
    cap.beam_row = 100;
    smsvid_tab_feed(t, &cap, live);
    smsvid_tab_refresh(t, 1);
    pic = smsvid_tab_picture(t, &w, &h);
    CHECK(pic && pic[(size_t)50 * w] == 0xff00ff00u && pic[(size_t)150 * w] != 0xff123456u && (pic[(size_t)150 * w] & 0xff) < 0x56,
          "Beam: live above the beam, the last frame dimmed below");

    /* ---- hint rows, registers, ages, timing, text ---- */
    make_cap(&cap, 192, 224);
    for (int y = 0; y < 224; y++) cap.lines[y].reg[0] = 0x14;
    for (int y = ACT_Y0; y < 224; y++) cap.lines[y].line_counter = (uint8_t)(9 - (y - ACT_Y0) % 10);
    cap.lines[ACT_Y0 - 1].line_counter = 0;       /* wrapped: fires on line 0 */
    smsvid_tab_feed(t, &cap, NULL);
    smsvid_tab_refresh(t, 1);
    {
        const smsvid_reg *r;
        int n = smsvid_tab_regs(t, &r);
        CHECK(n >= 11 && !strcmp(r[0].name, "R0") && !strcmp(r[0].value, "04") && strlen(r[2].meaning) > 5, "registers with meanings");
        cap.snap.reg[8] = 0x20;
        smsvid_tab_feed(t, &cap, NULL);
        smsvid_tab_refresh(t, 1);
        smsvid_tab_regs(t, &r);
        CHECK(r[8].age == 0 && r[0].age > 0, "a changed register has age 0, others older");
        smsvid_tab_refresh(t, 1);
        smsvid_tab_regs(t, &r);
        CHECK(r[8].age == 1, "age counts refreshes");
    }
    {
        char buf[4096];
        const char *const *tl;
        for (int s = 0; s < SMSVID_S_COUNT; s++) {
            int n = smsvid_tab_text(t, s, buf, sizeof buf);
            CHECK(n > 5, "every section has text");
        }
        CHECK(smsvid_tab_timing(t, &tl) >= 6 && strchr(tl[0], '\t'), "timing lines are label<TAB>value");
        CHECK(smsvid_tab_beam_permille(t) == 224 * 1000 / 262, "beam per-mille");
    }

    /* ---- the TMS9918 fallback ---- */
    make_cap(&cap, 192, 224);
    cap.snap.mode = 0;
    for (int y = 0; y < 224; y++) cap.lines[y].mode = 0;
    for (int v = 0; v < SMSVID_V_COUNT; v++)
        for (int s = 0; s < SMSVID_D_COUNT; s++) {
            smsvid_tab_set_source(t, s);
            smsvid_tab_feed(t, &cap, NULL);
            settle(t, v);
            smsvid_tab_hover(t, 5, 5);
        }
    CHECK(1, "TMS modes in every view and source do not crash");
    smsvid_tab_set_source(t, SMSVID_D_SCREEN);

    /* ---- Name table / tiles / palette sources ---- */
    make_cap(&cap, 224, 224);
    smsvid_tab_feed(t, &cap, NULL);
    smsvid_tab_set_source(t, SMSVID_D_NAMETABLE);
    smsvid_tab_set_overlays(t, SMSVID_OV_SCROLL | SMSVID_OV_HEAT | SMSVID_OV_GRID);
    settle(t, SMSVID_V_DECODED);
    pic = smsvid_tab_picture(t, &w, &h);
    CHECK(pic && w == 256 && h == 256, "name table source: 256x256 in a 224-line mode");
    smsvid_tab_hover(t, 2, 2);
    CHECK(smsvid_tab_pixel(t) && smsvid_tab_pixel(t)->name_addr == 0x3700 && smsvid_tab_pixel(t)->tile == 0,
          "name table source: the inspector reads the cell");
    smsvid_tab_set_source(t, SMSVID_D_TILES_SPR);
    smsvid_tab_refresh(t, 1);
    pic = smsvid_tab_picture(t, &w, &h);
    CHECK(pic && w == 256 && h == 128, "tiles source: 256x128");
    smsvid_tab_set_source(t, SMSVID_D_PALETTE);
    smsvid_tab_refresh(t, 1);
    smsvid_tab_hover(t, 17, 17);
    CHECK(smsvid_tab_pixel(t) && smsvid_tab_pixel(t)->cram_index == 17, "palette source: the cell is the CRAM entry");
    smsvid_tab_free(t);

    /* ---- fuzz ---- */
    t = smsvid_tab_new(NULL);
    srand(7);
    for (int iter = 0; iter < 150; iter++) {
        static const int yp[] = { 192, 224, 240 };
        const int ypix = yp[rand() % 3], fbh = (rand() & 1) ? 224 : 240;
        make_cap(&cap, ypix, fbh);
        for (size_t i = 0; i < sizeof cap.snap.vram; i++) cap.snap.vram[i] = (uint8_t)rand();
        for (int i = 0; i < 11; i++) cap.snap.reg[i] = (uint8_t)rand();
        cap.snap.mode = rand() % 5;
        cap.snap.kind_5246 = rand() & 1;
        for (int y = 0; y < fbh; y++) {
            smsvid_line *L = &cap.lines[y];
            for (int i = 0; i < 11; i++) L->reg[i] = (uint8_t)rand();
            L->x_scroll = (uint8_t)rand(); L->y_scroll = (uint8_t)rand();
            L->flags = (uint8_t)(rand() & 3);
            L->mode = (uint8_t)(rand() % 5);
            L->valid = (uint8_t)(rand() % 8 != 0);
            L->y_pixels = (uint16_t)yp[rand() % 3];
            L->line = (int16_t)(rand() % 300 - 40);
            for (int i = 0; i < 32; i++) L->cram[i] = (uint8_t)rand();
        }
        cap.beam_row = rand() % (fbh + 1);
        smsvid_tab_set_view(t, rand() % SMSVID_V_COUNT);
        smsvid_tab_set_source(t, rand() % SMSVID_D_COUNT);
        smsvid_tab_set_overlays(t, (unsigned)rand());
        smsvid_tab_set_zoom(t, (int[]){ 0, 1, 2, 3, 4, 6, 8, 12, 16 }[rand() % 9]);
        smsvid_tab_feed(t, &cap, (rand() & 1) ? live : NULL);
        smsvid_tab_refresh(t, 1);
        pic = smsvid_tab_picture(t, &w, &h);
        for (int k = 0; k < 4 && pic; k++) {
            smsvid_tab_hover(t, rand() % (w + 4) - 2, rand() % (h + 4) - 2);
            if (k == 1) smsvid_tab_pin(t, rand() % w, rand() % h);
        }
        smsvid_tab_refresh(t, 0);
        {
            char buf[2048];
            for (int s = 0; s < SMSVID_S_COUNT; s++) smsvid_tab_text(t, s, buf, sizeof buf);
        }
    }
    CHECK(1, "fuzz: 150 random captures through every view survived");
    smsvid_tab_free(t);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
