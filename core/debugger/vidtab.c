/*
 * vidtab.c -- the Video tab's controller (core/include/smsvidtab.h).
 *
 * The per-row decoder below follows the renderer in core/sms/vdp.c
 * (draw_scanline_mode4 and draw_sprites_mode4) one for one, so the Decoded
 * view of an unchanged frame is the same picture the machine drew.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "smsvidtab.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "host.h"
#include "smssession.h"
#include "smsvidcap.h"
#include "vdpview.h"

#define FW SMSVID_FB_W
#define FH SMSVID_FB_MAXH
#define MAXPIX 6000000L         /* the largest picture we will build */
#define MAX_REGS 16
#define MAX_TIMING 12
#define HEAT_FULL 255
#define VRAM_SIZE 0x4000

static const int k_zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

enum { BK_FRAME = 0, BK_SCREEN, BK_OTHER };   /* what the base picture is */

struct smsvid_tab {
    smsdebug *d;
    int view, source, zoom, vw, vh;
    unsigned ov;
    int env_section, capture_on;

    /* the machine's state as presented */
    smsvid_capture *cap;        /* working frame: snapshot, rows, picture */
    smsvid_capture *last;       /* last captured by the machine thread */
    uint64_t last_serial;
    uint32_t *live;             /* the machine's own picture, stopped only */
    int have, has_fb, has_live, stopped, fed;
    int act_y0, fb_h;
    uint64_t sig;

    /* VRAM trackers */
    uint8_t prev_vram[VRAM_SIZE], heat[VRAM_SIZE], changed[VRAM_SIZE];
    int have_prev, any_heat;

    /* the picture */
    uint32_t *base, *pic, *rbuf;
    size_t base_cap, pic_cap;
    int bw, bh, pw, ph, ez, kind;
    int dirty;
    char why[200];

    /* tables */
    smsvid_reg regs[MAX_REGS];
    char prev_val[MAX_REGS][24];
    int nregs, regs_primed;
    smsvid_swatch sw[32];
    long uses[32];
    int nsw;
    smsvid_line_row rows[FH];
    int nrows;
    char status[400];
    char timing_buf[MAX_TIMING][112];
    const char *timing_ptr[MAX_TIMING];
    int ntiming;

    /* pixels */
    int hx, hy, px, py;         /* -1: none */
    smsvid_pixel hover, pin;
};

/* ---- tables of names ----------------------------------------------------------------- */

static const smsvid_overlay_info k_overlays[] = {
    { SMSVID_OV_GRID,    "grid",    "Grid" },
    { SMSVID_OV_SCROLL,  "scroll",  "Scroll window" },
    { SMSVID_OV_HINT,    "hint",    "Line interrupt rows" },
    { SMSVID_OV_BEAM,    "beam",    "Beam" },
    { SMSVID_OV_HEAT,    "heat",    "Written lately" },
    { SMSVID_OV_CHANGED, "changed", "Changed since last refresh" },
    { SMSVID_OV_ACTIVE,  "active",  "Active display outline" },
    { SMSVID_OV_SPRITES, "sprites", "Sprites" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

const smsvid_overlay_info *smsvid_overlays(int *count)
{
    if (count) *count = N_OVERLAYS;
    return k_overlays;
}

static const char *const k_view_names[] = { "Display", "Decoded", "Beam", "Values" };
static const char *const k_view_keys[] = { "display", "decoded", "beam", "values" };
static const char *const k_src_names[] = { "Screen", "Name table", "Tiles (background)",
                                           "Tiles (sprites)", "Sprites", "Palette" };
static const char *const k_src_keys[] = { "screen", "nametable", "tiles", "tiles2", "sprites", "palette" };
static const char *const k_sec_names[] = { "Pixel", "Registers", "Colours", "Lines", "Timing" };
static const char *const k_sec_keys[] = { "pixel", "registers", "colours", "lines", "timing" };

const char *smsvid_view_name(int v) { return v >= 0 && v < SMSVID_V_COUNT ? k_view_names[v] : ""; }
const char *smsvid_view_key(int v) { return v >= 0 && v < SMSVID_V_COUNT ? k_view_keys[v] : ""; }
const char *smsvid_source_name(int s) { return s >= 0 && s < SMSVID_D_COUNT ? k_src_names[s] : ""; }
const char *smsvid_source_key(int s) { return s >= 0 && s < SMSVID_D_COUNT ? k_src_keys[s] : ""; }
const char *smsvid_section_name(int s) { return s >= 0 && s < SMSVID_S_COUNT ? k_sec_names[s] : ""; }
const char *smsvid_section_key(int s) { return s >= 0 && s < SMSVID_S_COUNT ? k_sec_keys[s] : ""; }

const int *smsvid_zooms(int *count)
{
    if (count) *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

/* ---- small helpers ---------------------------------------------------------------------- */

#define BIT(x, n) (((x) >> (n)) & 1)

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

static uint32_t blend(uint32_t dst, uint32_t rgb, int a256)
{
    const uint32_t dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
    const uint32_t sr = (rgb >> 16) & 255, sg = (rgb >> 8) & 255, sb = rgb & 255;
    const uint32_t r = (dr * (uint32_t)(256 - a256) + sr * (uint32_t)a256) >> 8;
    const uint32_t g = (dg * (uint32_t)(256 - a256) + sg * (uint32_t)a256) >> 8;
    const uint32_t b = (db * (uint32_t)(256 - a256) + sb * (uint32_t)a256) >> 8;
    return 0xff000000u | r << 16 | g << 8 | b;
}

static int fits_zoom(int w, int h, int z)
{
    return (long)w * z * (long)h * z <= MAXPIX;
}

static void why_set(smsvid_tab *t, const char *s)
{
    snprintf(t->why, sizeof t->why, "%s", s);
}

static int ensure(uint32_t **buf, size_t *cap, size_t need)
{
    if (need > *cap) {
        uint32_t *n = realloc(*buf, need * sizeof **buf);
        if (!n) return 0;
        *buf = n;
        *cap = need;
    }
    return 1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* the colour of CRAM index i as row R had it */
static uint32_t line_rgb(const smsvid_capture *c, const smsvid_line *R, int i)
{
    if (R->mode != 4)
        return c->snap.pens[64 + (i & 15)] | 0xff000000u;
    return c->snap.pens[R->cram[i & 31] & 0x3f] | 0xff000000u;
}

/* a row the VDP never drew (a fresh machine): what the registers say now */
static void default_line(const smsvid_capture *c, smsvid_line *R)
{
    memset(R, 0, sizeof *R);
    memcpy(R->reg, c->snap.reg, sizeof R->reg);
    R->x_scroll = c->snap.reg[8];
    R->y_scroll = c->snap.reg[9];
    R->mode = (uint8_t)c->snap.mode;
    R->y_pixels = (uint16_t)c->snap.y_pixels;
    R->flags = BIT(c->snap.reg[1], 6) ? 0 : SMSVID_FLAG_BLANK;
    memcpy(R->cram, c->snap.cram, sizeof R->cram);
}

static const smsvid_line *row_line(const smsvid_capture *c, int row, smsvid_line *tmp)
{
    row = clampi(row, 0, c->fb_height > 0 ? c->fb_height - 1 : 0);
    if (c->lines[row].valid)
        return &c->lines[row];
    default_line(c, tmp);
    return tmp;
}

static int find_act_y0(const smsvid_capture *c)
{
    for (int y = 0; y < c->fb_height; y++)
        if (c->lines[y].valid && c->lines[y].line == 0)
            return y;
    return (c->fb_height - (c->snap.y_pixels ? c->snap.y_pixels : 192)) / 2;
}

/* a line interrupt fires on this row: the line counter was 0 on the row above */
static int hint_fires(const smsvid_capture *c, int row)
{
    const smsvid_line *P, *R;
    if (row <= 0 || row >= c->fb_height) return 0;
    P = &c->lines[row - 1];
    R = &c->lines[row];
    if (!P->valid || !R->valid || !(R->reg[0] & 0x10)) return 0;
    if (R->line < 0 || R->line > R->y_pixels) return 0;
    return P->line_counter == 0;
}

/* ---- the mode 4 decoder (core/sms/vdp.c, row by row) ------------------------------------ */

#define VR(a) (c->snap.vram[(a) & (VRAM_SIZE - 1)])

typedef struct {
    int tc, fine;               /* tile column on screen, fine scroll (pixels left of column 0 are fill) */
    unsigned name_addr;
    unsigned entry;
    int tile, t1, t2;           /* tile number, the tile each half of the planes reads */
    int pal, hflip, vflip, prio;
    int ys;                     /* the vertical scroll this column used */
    int tile_line;              /* the tile row after the flip, 0..7 */
    unsigned pat1, pat2;        /* VRAM address of plane 0 / plane 2 of that row */
} bgcell;

static int xscroll_for(const smsvid_line *R, int L)
{
    return ((R->reg[0] & 0x40) && L < 16) ? 0 : R->x_scroll;
}

static void bg_cell(const smsvid_capture *c, const smsvid_line *R, int L, int tc, bgcell *o)
{
    const int k5246 = c->snap.kind_5246;
    const int xs = xscroll_for(R, L);
    const int startc = 32 - (xs >> 3);
    unsigned name_base, nr;
    int mod, tl;

    memset(o, 0, sizeof *o);
    if (R->y_pixels != 192) {
        name_base = (unsigned)(((R->reg[2] & 0x0c) << 10) | 0x0700);
        mod = 256;
    } else {
        name_base = (unsigned)((R->reg[2] << 10) & 0x3800);
        mod = 224;
    }
    o->tc = tc;
    o->fine = xs & 7;
    o->ys = ((R->reg[0] & 0x80) && tc > 23) ? 0 : R->y_scroll;
    nr = (unsigned)((((L + o->ys) % mod) >> 3) << 6);
    if (!k5246)
        nr &= (unsigned)(((R->reg[2] & 1) << 10) | 0x3bff);
    o->name_addr = (name_base + nr + (unsigned)(((tc + startc) & 0x1f) << 1)) & (VRAM_SIZE - 1);
    o->entry = (unsigned)(VR(o->name_addr) | (VR(o->name_addr + 1) << 8));
    o->tile = (int)(o->entry & 0x1ff);
    o->t1 = k5246 ? o->tile : (o->tile & ((R->reg[3] << 1) | 1));
    o->t2 = k5246 ? o->tile : (o->tile & (((R->reg[4] & 7) << 6) | 0x3f));
    o->prio = (int)(o->entry & 0x1000);
    o->pal = BIT(o->entry, 11);
    o->vflip = BIT(o->entry, 10);
    o->hflip = BIT(o->entry, 9);
    tl = L - ((7 - (o->ys & 7)) + 1);
    if (o->vflip) tl = 7 - tl;
    o->tile_line = tl & 7;
    o->pat1 = (unsigned)((o->t1 << 5) + (o->tile_line << 2));
    o->pat2 = (unsigned)((o->t2 << 5) + (o->tile_line << 2));
}

typedef struct { int idx, x, sel, pline; } spr_t;

/* the sprites select_sprites picks for active line L: up to 8 */
static int sprites_on_line(const smsvid_capture *c, const smsvid_line *R, int L, spr_t out[8])
{
    const int k5246 = c->snap.kind_5246;
    const int height = (R->reg[1] & 2) ? 16 : 8;
    const int zoom = (R->reg[1] & 1) ? 2 : 1;
    const unsigned sat = (unsigned)((R->reg[5] << 7) & 0x3f00);
    const unsigned extra = sat + (k5246 ? 0x80u : ((R->reg[5] & 1) ? 0x80u : 0u));
    int n = 0;

    for (int i = 0; i < 64; i++) {
        int pl = L - 1;
        int y = VR(sat + (unsigned)i);
        if (R->y_pixels == 192 && y == 0xd0) break;
        if (y >= 240) y -= 256;
        if (zoom > 1 && n < 8) { pl >>= 1; y >>= 1; }
        if (pl >= y && pl < y + height && n < 8) {
            const int sline = pl - y;
            int x = VR(extra + (unsigned)i * 2u);
            int tile = VR(extra + (unsigned)i * 2u + 1u);
            int sel = k5246 ? tile : (tile & (((R->reg[6] & 3) << 6) | 0x3f));
            if (R->reg[0] & 0x08) x -= 8;
            if (R->reg[6] & 4) sel += 256;
            if (height == 16) sel &= 0x1fe;
            if (sline > 7) sel += 1;
            out[n].idx = i; out[n].x = x; out[n].sel = sel; out[n].pline = (sline & 7) << 2;
            n++;
        }
    }
    return n;
}

typedef struct {
    uint8_t idx[256];           /* CRAM index per pixel */
    int prio[256];
    int8_t spr[256];            /* sprite number that drew it, -1 */
} dline;

static void decode_line(const smsvid_capture *c, const smsvid_line *R, int L, dline *o)
{
    const int backdrop = 0x10 + (R->reg[7] & 15);
    spr_t sp[8];
    int ns;

    memset(o->spr, -1, sizeof o->spr);
    for (int x = 0; x < 256; x++) { o->idx[x] = (uint8_t)backdrop; o->prio[x] = 0; }
    if (R->mode != 4 || (R->flags & SMSVID_FLAG_BLANK) || L < 0 || L >= R->y_pixels)
        return;
    for (int x = 0; x < 256; x++) o->prio[x] = 1;

    for (int tc = 0; tc < 32; tc++) {
        bgcell b;
        bg_cell(c, R, L, tc, &b);
        if (tc == 0 && b.fine > 0) {
            const int pl = b.tile_line - 1;
            const uint8_t p1 = VR((0x100u << 5) + (unsigned)((pl & 7) << 2) + 1u);
            const uint8_t bit = (uint8_t)BIT(p1, 7 - 4);
            for (int x = 0; x < b.fine; x++) { o->idx[x] = bit ? 0x10 : 0; o->prio[x] = 0; }
        }
        {
            const uint8_t p0 = VR(b.pat1), p1 = VR(b.pat1 + 1), p2 = VR(b.pat2 + 2), p3 = VR(b.pat2 + 3);
            for (int px = 0; px < 8; px++) {
                int pen = BIT(p0, 7 - px) | BIT(p1, 7 - px) << 1 | BIT(p2, 7 - px) << 2 | BIT(p3, 7 - px) << 3;
                int x;
                if (b.pal) pen |= 0x10;
                x = b.fine + (tc << 3) + (b.hflip ? 7 - px : px);
                if (x < 256) { o->idx[x] = (uint8_t)pen; o->prio[x] = b.prio | (pen & 0x0f); }
            }
        }
    }

    ns = sprites_on_line(c, R, L, sp);
    for (int s = ns - 1; s >= 0; s--) {
        const int zoom = (s < (c->snap.kind_5246 ? 8 : 4)) ? ((R->reg[1] & 1) ? 2 : 1) : 1;
        const unsigned base = (unsigned)(sp[s].sel << 5) + (unsigned)sp[s].pline;
        const uint8_t p0 = VR(base), p1 = VR(base + 1), p2 = VR(base + 2), p3 = VR(base + 3);
        for (int px = 0; px < 8; px++) {
            const int pen = (BIT(p0, 7 - px) | BIT(p1, 7 - px) << 1 | BIT(p2, 7 - px) << 2 |
                             BIT(p3, 7 - px) << 3) | 0x10;
            int x;
            if (pen == 0x10) continue;
            x = zoom > 1 ? sp[s].x + (px << 1) : sp[s].x + px;
            for (int z = 0; z < zoom; z++) {
                x += z;
                if (x < 0 || x > 255) continue;
                if (!(o->prio[x] & 0x1000) || o->prio[x] == 0x1000) {
                    o->idx[x] = (uint8_t)pen;
                    o->prio[x] = pen;
                    o->spr[x] = (int8_t)sp[s].idx;
                }
            }
        }
    }
    if (R->reg[0] & 0x20)
        for (int x = 0; x < 8; x++) { o->idx[x] = (uint8_t)backdrop; o->spr[x] = -1; }
}

/* ---- new / free / settings -------------------------------------------------------------- */

smsvid_tab *smsvid_tab_new(smsdebug *d)
{
    smsvid_tab *t = calloc(1, sizeof *t);
    const char *e;
    if (!t) return NULL;
    t->d = d;
    t->cap = calloc(1, sizeof *t->cap);
    t->last = calloc(1, sizeof *t->last);
    t->live = calloc((size_t)FW * FH, sizeof *t->live);
    t->rbuf = calloc(256 * 256, sizeof *t->rbuf);
    if (!t->cap || !t->last || !t->live || !t->rbuf) {
        smsvid_tab_free(t);
        return NULL;
    }
    t->view = SMSVID_V_DISPLAY;
    t->source = SMSVID_D_SCREEN;
    t->ov = SMSVID_OV_BEAM | SMSVID_OV_ACTIVE;
    t->zoom = 0;
    t->vw = FW * 2;
    t->vh = FH * 2;
    t->hx = t->hy = t->px = t->py = -1;
    t->dirty = 1;

    if ((e = getenv("SMS_DEBUGGER_VIDEO_VIEW")) && *e) {
        int hit = 0;
        for (int i = 0; i < SMSVID_D_COUNT; i++)
            if (!strcasecmp(e, k_src_keys[i])) { t->view = SMSVID_V_DECODED; t->source = i; hit = 1; break; }
        for (int i = 0; !hit && i < SMSVID_V_COUNT; i++)
            if (!strncasecmp(e, k_view_keys[i], strlen(e))) { t->view = i; break; }
    }
    if ((e = getenv("SMS_DEBUGGER_VIDEO_OVERLAYS"))) {
        char buf[200], *tok, *sp;
        unsigned ov = 0;
        snprintf(buf, sizeof buf, "%s", e);
        for (tok = strtok_r(buf, ", ", &sp); tok; tok = strtok_r(NULL, ", ", &sp)) {
            if (!strcasecmp(tok, "all")) ov |= SMSVID_OV_ALL;
            else if (!strcasecmp(tok, "none")) ov = 0;
            else
                for (int i = 0; i < N_OVERLAYS; i++)
                    if (!strcasecmp(tok, k_overlays[i].name)) ov |= k_overlays[i].bit;
        }
        t->ov = ov;
    }
    if ((e = getenv("SMS_DEBUGGER_VIDEO_ZOOM")) && *e) {
        if (!strcasecmp(e, "fit")) t->zoom = 0;
        else {
            int z = atoi(e);
            for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
                if (k_zooms[i] == z) t->zoom = z;
        }
    }
    t->env_section = SMSVID_S_PIXEL;
    if ((e = getenv("SMS_DEBUGGER_VIDEO_SECTION")) && *e) {
        for (int i = 0; i < SMSVID_S_COUNT; i++)
            if (!strncasecmp(e, k_sec_keys[i], strlen(e))) { t->env_section = i; break; }
    }
    sms_host_capture_enable(1);
    t->capture_on = 1;
    return t;
}

void smsvid_tab_free(smsvid_tab *t)
{
    if (!t) return;
    if (t->capture_on)
        sms_host_capture_enable(0);
    free(t->cap);
    free(t->last);
    free(t->live);
    free(t->rbuf);
    free(t->base);
    free(t->pic);
    free(t);
}

void smsvid_tab_set_view(smsvid_tab *t, int v)
{
    if (t && v >= 0 && v < SMSVID_V_COUNT && v != t->view) { t->view = v; t->dirty = 1; }
}
int smsvid_tab_view(const smsvid_tab *t) { return t ? t->view : 0; }
void smsvid_tab_set_source(smsvid_tab *t, int s)
{
    if (t && s >= 0 && s < SMSVID_D_COUNT && s != t->source) { t->source = s; t->dirty = 1; }
}
int smsvid_tab_source(const smsvid_tab *t) { return t ? t->source : 0; }
void smsvid_tab_set_overlays(smsvid_tab *t, unsigned ov)
{
    if (t && (ov & SMSVID_OV_ALL) != t->ov) { t->ov = ov & SMSVID_OV_ALL; t->dirty = 1; }
}
unsigned smsvid_tab_overlays(const smsvid_tab *t) { return t ? t->ov : 0; }
void smsvid_tab_set_zoom(smsvid_tab *t, int z)
{
    if (t && z >= 0 && z <= 16 && z != t->zoom) { t->zoom = z; t->dirty = 1; }
}
int smsvid_tab_zoom(const smsvid_tab *t) { return t ? t->zoom : 0; }
void smsvid_tab_set_viewport(smsvid_tab *t, int w, int h)
{
    if (!t) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w != t->vw || h != t->vh) {
        t->vw = w; t->vh = h;
        if (t->zoom == 0) t->dirty = 1;
    }
}
int smsvid_tab_env_section(const smsvid_tab *t) { return t ? t->env_section : 0; }

/* ---- feeding ----------------------------------------------------------------------------- */

static void note_vram(smsvid_tab *t)
{
    const uint8_t *v = t->cap->snap.vram;
    t->any_heat = 0;
    for (int i = 0; i < VRAM_SIZE; i++) {
        const int ch = t->have_prev && v[i] != t->prev_vram[i];
        t->changed[i] = (uint8_t)ch;
        t->heat[i] = ch ? HEAT_FULL : (uint8_t)(t->heat[i] * 4 / 5);
        if (t->heat[i]) t->any_heat = 1;
    }
    memcpy(t->prev_vram, v, sizeof t->prev_vram);
    t->have_prev = 1;
}

static void set_geometry(smsvid_tab *t)
{
    t->fb_h = t->cap->fb_height > 0 ? t->cap->fb_height : 224;
    if (t->fb_h > FH) t->fb_h = FH;
    t->act_y0 = find_act_y0(t->cap);
}

void smsvid_tab_feed(smsvid_tab *t, const struct smsvid_capture *cap, const uint32_t *live_fb)
{
    if (!t || !cap) return;
    memcpy(t->cap, cap, sizeof *t->cap);
    t->fed = 1;
    t->have = 1;
    t->has_fb = cap->serial != 0;
    t->has_live = live_fb != NULL;
    t->stopped = live_fb != NULL;
    if (live_fb) memcpy(t->live, live_fb, (size_t)FW * (size_t)cap->fb_height * sizeof *t->live);
    set_geometry(t);
}

/* Take what the machine has. 0 if there is no machine. */
static int acquire(smsvid_tab *t)
{
    sms_machine_t *m;

    if (t->fed)
        return t->have;
    t->have = 0;
    if (!sms_host_is_running())
        return 0;
    sms_host_capture_get(t->last, &t->last_serial);
    t->has_fb = t->last->serial != 0;
    t->stopped = t->d && smsdebug_is_stopped(t->d);
    t->has_live = t->stopped;

    if (t->stopped || !t->has_fb) {
        /* a parked machine is ours to read; before the first capture the frame
         * in progress is all there is */
        sms_host_lock();
        m = sms_host_machine();
        if (!m) {
            sms_host_unlock();
            return 0;
        }
        sms_host_capture_fill(m, t->cap);
        sms_host_unlock();
        t->cap->serial = t->last->serial;
        if (t->stopped) {
            memcpy(t->live, t->cap->fb, (size_t)FW * (size_t)t->cap->fb_height * sizeof *t->live);
            /* below the beam the rows are the last finished frame's */
            if (t->has_fb && t->last->fb_height == t->cap->fb_height)
                for (int y = t->cap->beam_row; y < t->cap->fb_height; y++) {
                    t->cap->lines[y] = t->last->lines[y];
                    memcpy(&t->cap->fb[(size_t)y * FW], &t->last->fb[(size_t)y * FW], FW * sizeof *t->cap->fb);
                }
        } else {
            t->cap->beam_row = t->cap->fb_height;
        }
    } else {
        memcpy(t->cap, t->last, sizeof *t->cap);
    }
    t->have = 1;
    set_geometry(t);
    return 1;
}

static uint64_t signature(const smsvid_tab *t)
{
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, &t->cap->snap, offsetof(smsvdp_snap, pens));
    h = fnv(h, t->cap->lines, sizeof t->cap->lines);
    h = fnv(h, &t->cap->serial, sizeof t->cap->serial);
    h = fnv(h, &t->cap->vpos, sizeof t->cap->vpos);
    h = fnv(h, &t->cap->hpos, sizeof t->cap->hpos);
    h = fnv(h, &t->cap->frame, sizeof t->cap->frame);
    h = fnv(h, &t->cap->beam_row, sizeof t->cap->beam_row);
    h = fnv(h, &t->has_live, sizeof t->has_live);
    h = fnv(h, &t->has_fb, sizeof t->has_fb);
    return h;
}

/* ---- the false colours of the Values view -------------------------------------------------- */

static uint32_t value_colour(int idx)
{
    const int i = idx & 15;
    if (idx & 16)       /* sprite palette: warm */
        return 0xff000000u | (uint32_t)(70 + i * 12) << 16 | (uint32_t)(40 + i * 8) << 8 | (uint32_t)(i * 3);
    return 0xff000000u | (uint32_t)(i * 4) << 16 | (uint32_t)(50 + i * 10) << 8 | (uint32_t)(70 + i * 12);
}

/* ---- the base picture (1x) -------------------------------------------------------------------- */

static int mode4(const smsvid_tab *t) { return t->cap->snap.mode == 4; }

/* Decode every active line. dst (256 x y_pixels) may be NULL when only the
 * uses are wanted. values: false colours instead of the palette. */
static void decode_screen(smsvid_tab *t, uint32_t *dst, int values)
{
    const smsvid_capture *c = t->cap;
    const int ypix = c->snap.y_pixels;
    dline dl;
    smsvid_line tmp;

    memset(t->uses, 0, sizeof t->uses);
    for (int L = 0; L < ypix; L++) {
        const smsvid_line *R = row_line(c, t->act_y0 + L, &tmp);
        decode_line(c, R, L, &dl);
        for (int x = 0; x < 256; x++) {
            t->uses[dl.idx[x] & 31]++;
            if (dst)
                dst[(size_t)L * 256 + (size_t)x] = values ? value_colour(dl.idx[x]) : line_rgb(c, R, dl.idx[x]);
        }
    }
}

static int build_other(smsvid_tab *t)
{
    const smsvdp_snap *s = &t->cap->snap;
    int w = 0, h = 0;

    memset(t->rbuf, 0, 256 * 256 * sizeof *t->rbuf);
    switch (t->source) {
    case SMSVID_D_TILES_BG:  smsvdp_render_tiles(s, 0, t->rbuf, &w, &h); break;
    case SMSVID_D_TILES_SPR: smsvdp_render_tiles(s, 1, t->rbuf, &w, &h); break;
    case SMSVID_D_SPRITES:   smsvdp_render_sprites(s, t->rbuf, &w, &h, SMSSESSION_ACCENT_RGB); break;
    case SMSVID_D_PALETTE:   smsvdp_render_palette(s, t->rbuf, &w, &h); break;
    default:                 smsvdp_render_nametable(s, t->rbuf, &w, &h, SMSSESSION_ACCENT_RGB); break;
    }
    if (w < 1 || h < 1 || w > 256 || h > 256) {
        why_set(t, "This source has nothing to draw.");
        return 0;
    }
    if (!ensure(&t->base, &t->base_cap, (size_t)w * (size_t)h)) return 0;
    for (int i = 0; i < w * h; i++) t->base[i] = t->rbuf[i] | 0xff000000u;
    t->bw = w; t->bh = h;
    t->kind = BK_OTHER;
    return 1;
}

static int build_base(smsvid_tab *t)
{
    const smsvid_capture *c = t->cap;
    const int H = t->fb_h;

    switch (t->view) {
    case SMSVID_V_DISPLAY:
        if (!t->has_fb && !t->has_live) {
            why_set(t, "No frame has finished yet. Let the machine run for a moment.");
            return 0;
        }
        if (!ensure(&t->base, &t->base_cap, (size_t)FW * (size_t)H)) return 0;
        for (int i = 0; i < FW * H; i++) t->base[i] = c->fb[i] | 0xff000000u;
        t->bw = FW; t->bh = H;
        t->kind = BK_FRAME;
        return 1;
    case SMSVID_V_BEAM: {
        const int beam = c->beam_row;
        if (!t->has_live) {
            why_set(t, "The Beam view needs the machine stopped: it shows this frame as far as the beam has drawn it, and the last one dimmed below.");
            return 0;
        }
        if (!ensure(&t->base, &t->base_cap, (size_t)FW * (size_t)H)) return 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < FW; x++) {
                uint32_t px;
                if (y < beam) px = t->live[(size_t)y * FW + (size_t)x] | 0xff000000u;
                else if (t->has_fb) px = blend(c->fb[(size_t)y * FW + (size_t)x], 0x000000, 150);
                else px = 0xff000000u;
                t->base[(size_t)y * FW + (size_t)x] = px;
            }
        t->bw = FW; t->bh = H;
        t->kind = BK_FRAME;
        return 1;
    }
    default:
        if (!mode4(t) || (t->view == SMSVID_V_DECODED && t->source != SMSVID_D_SCREEN))
            return build_other(t);
        {
            const int ypix = c->snap.y_pixels;
            if (!ensure(&t->base, &t->base_cap, (size_t)256 * (size_t)ypix)) return 0;
            decode_screen(t, t->base, t->view == SMSVID_V_VALUES);
            t->bw = 256; t->bh = ypix;
            t->kind = BK_SCREEN;
        }
        return 1;
    }
}

/* ---- overlays ---------------------------------------------------------------------------- */

typedef struct { uint32_t *p; int w, h; } canvas;

static void px_blend(canvas *c, int x, int y, uint32_t rgb, int a)
{
    if (x >= 0 && y >= 0 && x < c->w && y < c->h)
        c->p[(size_t)y * (size_t)c->w + (size_t)x] = blend(c->p[(size_t)y * (size_t)c->w + (size_t)x], rgb, a);
}
static void hline(canvas *c, int y, int x0, int x1, uint32_t rgb, int a)
{
    for (int x = x0; x < x1; x++) px_blend(c, x, y, rgb, a);
}
static void vline(canvas *c, int x, int y0, int y1, uint32_t rgb, int a)
{
    for (int y = y0; y < y1; y++) px_blend(c, x, y, rgb, a);
}
static void fill_rect(canvas *c, int x, int y, int w, int h, uint32_t rgb, int a)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) px_blend(c, x + xx, y + yy, rgb, a);
}
static void box(canvas *c, int x, int y, int w, int h, uint32_t rgb, int a)
{
    hline(c, y, x, x + w, rgb, a);
    hline(c, y + h - 1, x, x + w, rgb, a);
    vline(c, x, y, y + h, rgb, a);
    vline(c, x + w - 1, y, y + h, rgb, a);
}

/* where the 256 active pixels start in the picture, and picture y -> fb row */
static int off_x(const smsvid_tab *t) { return t->kind == BK_FRAME ? SMSVID_ACTIVE_X : 0; }
static int pic_row(const smsvid_tab *t, int y)
{
    return t->kind == BK_FRAME ? y : t->act_y0 + y;
}
/* active line -> picture y */
static int line_y(const smsvid_tab *t, int L)
{
    return t->kind == BK_FRAME ? t->act_y0 + L : L;
}

static int vram_hot(const smsvid_tab *t, unsigned a, unsigned n, const uint8_t *arr)
{
    int m = 0;
    (void)t;
    for (unsigned i = 0; i < n; i++)
        if (arr[(a + i) & (VRAM_SIZE - 1)] > m) m = arr[(a + i) & (VRAM_SIZE - 1)];
    return m;
}

static void draw_screen_overlays(smsvid_tab *t, canvas *cv, int z)
{
    const smsvid_capture *c = t->cap;
    const int ox = off_x(t);
    const int ypix = c->snap.y_pixels;
    smsvid_line tmp;

    if (!mode4(t)) return;

    if (t->ov & (SMSVID_OV_HEAT | SMSVID_OV_CHANGED)) {
        for (int y = 0; y < t->bh; y++) {
            const smsvid_line *R = row_line(c, pic_row(t, y), &tmp);
            const int L = t->kind == BK_FRAME ? y - t->act_y0 : y;
            if (R->mode != 4 || L < 0 || L >= ypix || (R->flags & SMSVID_FLAG_BLANK)) continue;
            for (int tc = 0; tc < 32; tc++) {
                bgcell b;
                int x0, hot, chg;
                bg_cell(c, R, L, tc, &b);
                hot = vram_hot(t, b.name_addr, 2, t->heat);
                if (b.pat1 == b.pat2) hot = hot > vram_hot(t, b.pat1, 4, t->heat) ? hot : vram_hot(t, b.pat1, 4, t->heat);
                chg = vram_hot(t, b.name_addr, 2, t->changed) || vram_hot(t, b.pat1, 4, t->changed);
                x0 = ox + b.fine + tc * 8;
                if ((t->ov & SMSVID_OV_HEAT) && hot)
                    fill_rect(cv, x0 * z, y * z, 8 * z, z, 0xffff8a00u, hot * 190 / 255);
                if ((t->ov & SMSVID_OV_CHANGED) && chg)
                    fill_rect(cv, x0 * z, y * z, 8 * z, z, 0xff00e0ff, 120);
            }
        }
    }
    if (t->ov & SMSVID_OV_GRID) {
        if (z >= 2)
            for (int y = 0; y < t->bh; y++) {
                const smsvid_line *R = row_line(c, pic_row(t, y), &tmp);
                const int L = t->kind == BK_FRAME ? y - t->act_y0 : y;
                if (L < 0 || L >= ypix) continue;
                const int fine = xscroll_for(R, L) & 7;
                for (int k = 0, x = ox + fine; x < ox + 256; x += 8, k++)
                    vline(cv, x * z, y * z, (y + 1) * z, 0xffffffff, (k % 4 == 0) ? 90 : 50);
                if (((L + R->y_scroll) & 7) == 0 && z >= 2)
                    hline(cv, y * z, ox * z, (ox + 256) * z, 0xffffffff, 60);
            }
    }
    if (t->ov & SMSVID_OV_SCROLL) {
        for (int y = 0; y < t->bh; y++) {
            const smsvid_line *R = row_line(c, pic_row(t, y), &tmp);
            const int L = t->kind == BK_FRAME ? y - t->act_y0 : y;
            if (R->mode != 4 || L < 0 || L >= ypix) continue;
            if ((R->reg[0] & 0x40) && L < 16)
                fill_rect(cv, ox * z, y * z, 256 * z, z, 0xff30d0ff, 70);        /* no horizontal scroll */
            if (R->reg[0] & 0x80)
                fill_rect(cv, (ox + 192) * z, y * z, 64 * z, z, 0xffd060ff, 70);  /* no vertical scroll */
            if (R->reg[0] & 0x20)
                fill_rect(cv, ox * z, y * z, 8 * z, z, 0xff808080, 120);          /* blanked column */
            {
                const int fine = xscroll_for(R, L) & 7;
                if (fine) fill_rect(cv, ox * z, y * z, fine * z, z, 0xffffe060, 120);  /* fill pixels */
            }
        }
    }
    if (t->ov & SMSVID_OV_HINT) {
        for (int y = 0; y < t->bh; y++) {
            const int row = pic_row(t, y);
            if (row >= 0 && row < c->fb_height && hint_fires(c, row))
                hline(cv, y * z, 0, t->bw * z, 0xffff5ad2, 230);
        }
    }
    if ((t->ov & SMSVID_OV_BEAM) && t->has_live) {
        const int row = c->beam_row;
        int y = t->kind == BK_FRAME ? row : row - t->act_y0;
        if (y >= 0 && y < t->bh) hline(cv, y * z, 0, t->bw * z, 0xffff4040, 230);
    }
    if ((t->ov & SMSVID_OV_ACTIVE) && t->kind == BK_FRAME) {
        const int x0 = ox * z, x1 = (ox + 256) * z;
        const int y0 = t->act_y0 * z, y1 = (t->act_y0 + ypix) * z;
        hline(cv, y0, x0, x1, 0xffffffff, 160);
        hline(cv, y1 - 1, x0, x1, 0xffffffff, 160);
        vline(cv, x0, y0, y1, 0xffffffff, 160);
        vline(cv, x1 - 1, y0, y1, 0xffffffff, 160);
    }
    if (t->ov & SMSVID_OV_SPRITES) {
        smsvdp_sprite sp[64];
        const int n = smsvdp_sprites(&c->snap, sp);
        const int tall = (c->snap.reg[1] & 2) ? 16 : 8;
        const int zoom = (c->snap.reg[1] & 1) ? 2 : 1;
        for (int i = 0; i < n; i++) {
            int y = sp[i].y;
            if (!sp[i].visible) continue;
            if (y >= 240) y -= 256;
            box(cv, (ox + sp[i].x) * z, (line_y(t, 0) + y + 1) * z, 8 * zoom * z, tall * zoom * z,
                0xff00ff80u + ((uint32_t)(i * 29 & 0x7f) << 16), 220);
        }
    }
}

static void draw_other_overlays(smsvid_tab *t, canvas *cv, int z)
{
    const smsvid_capture *c = t->cap;
    smsvdp_tables tb;
    smsvid_line tmp;

    smsvdp_get_tables(&c->snap, &tb);
    if (t->source == SMSVID_D_NAMETABLE && mode4(t)) {
        if (t->ov & (SMSVID_OV_HEAT | SMSVID_OV_CHANGED))
            for (int ty = 0; ty < tb.name_rows; ty++)
                for (int tx = 0; tx < 32; tx++) {
                    const unsigned a = tb.name + (unsigned)(ty * 64 + tx * 2);
                    const int hot = vram_hot(t, a, 2, t->heat);
                    if ((t->ov & SMSVID_OV_HEAT) && hot)
                        fill_rect(cv, tx * 8 * z, ty * 8 * z, 8 * z, 8 * z, 0xffff8a00u, hot * 190 / 255);
                    if ((t->ov & SMSVID_OV_CHANGED) && vram_hot(t, a, 2, t->changed))
                        fill_rect(cv, tx * 8 * z, ty * 8 * z, 8 * z, 8 * z, 0xff00e0ff, 120);
                }
        if (t->ov & SMSVID_OV_SCROLL) {
            /* the window each active line looked through */
            for (int L = 0; L < c->snap.y_pixels; L++) {
                const smsvid_line *R = row_line(c, t->act_y0 + L, &tmp);
                const int mod = R->y_pixels != 192 ? 256 : 224;
                const int x0 = (256 - xscroll_for(R, L)) & 255;
                const int y = (L + R->y_scroll) % mod;
                fill_rect(cv, x0 * z, y * z, z, z, 0xff30d0ff, 230);
                fill_rect(cv, ((x0 + 255) & 255) * z, y * z, z, z, 0xff30d0ff, 230);
            }
        }
    } else if ((t->source == SMSVID_D_TILES_BG || t->source == SMSVID_D_TILES_SPR) && mode4(t)) {
        if (t->ov & (SMSVID_OV_HEAT | SMSVID_OV_CHANGED))
            for (int n = 0; n < 512; n++) {
                const int hot = vram_hot(t, (unsigned)n * 32, 32, t->heat);
                const int tx = n % 32, ty = n / 32;
                if ((t->ov & SMSVID_OV_HEAT) && hot)
                    fill_rect(cv, tx * 8 * z, ty * 8 * z, 8 * z, 8 * z, 0xffff8a00u, hot * 190 / 255);
                if ((t->ov & SMSVID_OV_CHANGED) && vram_hot(t, (unsigned)n * 32, 32, t->changed))
                    fill_rect(cv, tx * 8 * z, ty * 8 * z, 8 * z, 8 * z, 0xff00e0ff, 120);
            }
    }
    if ((t->ov & SMSVID_OV_GRID) && t->source != SMSVID_D_PALETTE && z >= 2) {
        const int cw = t->source == SMSVID_D_SPRITES ? 8 : 8;
        for (int x = 0; x < t->bw; x += cw) vline(cv, x * z, 0, t->bh * z, 0xffffffff, (x % 64 == 0) ? 90 : 45);
        for (int y = 0; y < t->bh; y += 8) hline(cv, y * z, 0, t->bw * z, 0xffffffff, (y % 64 == 0) ? 90 : 45);
    }
}

static int pick_zoom(const smsvid_tab *t)
{
    int z = t->zoom;
    if (z == 0) {
        z = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (t->bw * k_zooms[i] <= t->vw && t->bh * k_zooms[i] <= t->vh)
                z = k_zooms[i];
    }
    while (z > 1 && !fits_zoom(t->bw, t->bh, z)) {
        int lower = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (k_zooms[i] < z) lower = k_zooms[i];
        z = lower;
    }
    return z;
}

static void build_picture(smsvid_tab *t)
{
    int z;
    canvas cv;

    t->why[0] = '\0';
    t->pw = t->ph = 0;
    if (!build_base(t))
        return;
    z = pick_zoom(t);
    t->ez = z;
    t->pw = t->bw * z;
    t->ph = t->bh * z;
    if (!ensure(&t->pic, &t->pic_cap, (size_t)t->pw * (size_t)t->ph)) {
        t->pw = t->ph = 0;
        why_set(t, "Out of memory.");
        return;
    }
    if (z == 1)
        memcpy(t->pic, t->base, (size_t)t->bw * (size_t)t->bh * sizeof *t->pic);
    else
        for (int y = 0; y < t->ph; y++) {
            const uint32_t *src = &t->base[(size_t)(y / z) * (size_t)t->bw];
            uint32_t *dst = &t->pic[(size_t)y * (size_t)t->pw];
            for (int x = 0; x < t->pw; x++)
                dst[x] = src[x / z];
        }
    cv.p = t->pic; cv.w = t->pw; cv.h = t->ph;
    if (t->kind == BK_OTHER)
        draw_other_overlays(t, &cv, z);
    else
        draw_screen_overlays(t, &cv, z);
}

/* ---- the inspector ----------------------------------------------------------------------- */

static void locate_other(smsvid_tab *t, int px, int py, smsvid_pixel *o)
{
    const smsvid_capture *c = t->cap;
    smsvdp_tables tb;
    int n;

    smsvdp_get_tables(&c->snap, &tb);
    o->rgb = t->base ? t->base[(size_t)py * (size_t)t->bw + (size_t)px] : 0;
    n = snprintf(o->text, sizeof o->text, "%s pixel %d,%d\n", smsvid_source_name(t->source), px, py);
    if (!mode4(t)) {
        snprintf(o->text + n, sizeof o->text - (size_t)n, "TMS9918 picture (no per-row registers)\nColour #%06X",
                 o->rgb & 0xffffff);
        return;
    }
    switch (t->source) {
    case SMSVID_D_NAMETABLE: {
        const int tx = px / 8, ty = py / 8;
        const unsigned a = (tb.name + (unsigned)(ty * 64 + tx * 2)) & (VRAM_SIZE - 1);
        const unsigned e = (unsigned)(c->snap.vram[a] | (c->snap.vram[(a + 1) & (VRAM_SIZE - 1)] << 8));
        o->tile_col = tx; o->tile_row = ty;
        o->name_addr = a; o->name_entry = e;
        o->tile = (int)(e & 0x1ff);
        o->palette = BIT(e, 11); o->hflip = BIT(e, 9); o->vflip = BIT(e, 10); o->priority = BIT(e, 12);
        o->tile_x = o->hflip ? 7 - px % 8 : px % 8;
        o->tile_y = o->vflip ? 7 - py % 8 : py % 8;
        o->pattern_addr = (unsigned)(o->tile << 5) + (unsigned)(o->tile_y << 2);
        {
            const uint8_t *v = c->snap.vram;
            const unsigned p = o->pattern_addr;
            o->colour_index = BIT(v[p & (VRAM_SIZE - 1)], 7 - o->tile_x) |
                              BIT(v[(p + 1) & (VRAM_SIZE - 1)], 7 - o->tile_x) << 1 |
                              BIT(v[(p + 2) & (VRAM_SIZE - 1)], 7 - o->tile_x) << 2 |
                              BIT(v[(p + 3) & (VRAM_SIZE - 1)], 7 - o->tile_x) << 3;
        }
        o->cram_index = o->colour_index | (o->palette << 4);
        o->cram = c->snap.cram[o->cram_index];
        snprintf(o->text + n, sizeof o->text - (size_t)n,
                 "Name table cell %d,%d: entry $%04X at $%04X\n"
                 "Tile %d, %s palette%s%s%s\n"
                 "Pattern row at $%04X, colour index %d\n"
                 "CRAM[%d] = $%02X   #%06X",
                 tx, ty, e, a, o->tile, o->palette ? "sprite" : "background",
                 o->hflip ? ", h-flip" : "", o->vflip ? ", v-flip" : "", o->priority ? ", priority" : "",
                 o->pattern_addr, o->colour_index, o->cram_index, o->cram, o->rgb & 0xffffff);
        break;
    }
    case SMSVID_D_TILES_BG:
    case SMSVID_D_TILES_SPR: {
        const int tile = (py / 8) * 32 + px / 8;
        o->tile = tile;
        o->tile_x = px % 8; o->tile_y = py % 8;
        o->palette = t->source == SMSVID_D_TILES_SPR;
        o->pattern_addr = (unsigned)(tile << 5) + (unsigned)(o->tile_y << 2);
        snprintf(o->text + n, sizeof o->text - (size_t)n,
                 "Tile %d at $%04X, row %d at $%04X\nColour #%06X",
                 tile, tile << 5, o->tile_y, o->pattern_addr, o->rgb & 0xffffff);
        break;
    }
    case SMSVID_D_PALETTE: {
        const int i = px / 16 + (py / 16) * 16;
        o->cram_index = i & 31;
        o->cram = c->snap.cram[o->cram_index];
        snprintf(o->text + n, sizeof o->text - (size_t)n,
                 "CRAM[%d] = $%02X (%s %d)   #%06X", o->cram_index, o->cram,
                 o->cram_index < 16 ? "background" : "sprite", o->cram_index & 15, o->rgb & 0xffffff);
        break;
    }
    default:
        snprintf(o->text + n, sizeof o->text - (size_t)n, "Colour #%06X", o->rgb & 0xffffff);
        break;
    }
}

static void locate(smsvid_tab *t, int x, int y, smsvid_pixel *o)
{
    const smsvid_capture *c = t->cap;
    const int z = t->ez ? t->ez : 1;
    smsvid_line tmp;
    const smsvid_line *R;
    int px, py, sx, L, row, n;
    dline dl;

    memset(o, 0, sizeof *o);
    o->row = -1; o->column = -1; o->tile_col = -1; o->tile_row = -1; o->tile = -1; o->sprite = -1;
    if (!t->pw || x < 0 || y < 0 || x >= t->pw || y >= t->ph)
        return;
    px = x / z; py = y / z;
    o->valid = 1;
    o->x = px; o->y = py;
    if (t->kind == BK_OTHER) {
        locate_other(t, px, py, o);
        return;
    }
    if (t->kind == BK_FRAME) { sx = px - SMSVID_ACTIVE_X; row = py; }
    else { sx = px; row = t->act_y0 + py; }
    row = clampi(row, 0, t->fb_h - 1);
    R = row_line(c, row, &tmp);
    L = R->valid ? R->line : (t->kind == BK_FRAME ? py - t->act_y0 : py);
    o->row = row;
    o->line = L;
    o->x_scroll = xscroll_for(R, L);
    o->y_scroll = R->y_scroll;
    o->rgb = t->base ? t->base[(size_t)py * (size_t)t->bw + (size_t)px] : 0xff000000u;
    n = snprintf(o->text, sizeof o->text, "Pixel %d,%d   row %d, line %d, column %d\n", px, py, row, L, sx);

    if (R->mode != 4) {
        snprintf(o->text + n, sizeof o->text - (size_t)n,
                 "TMS9918 mode %d: no per-row decode (see Decoded > Name table)\nColour #%06X",
                 R->mode, o->rgb & 0xffffff);
        return;
    }
    if (sx < 0 || sx > 255 || L < 0 || L >= R->y_pixels || (R->flags & SMSVID_FLAG_BLANK)) {
        o->cram_index = 0x10 + (R->reg[7] & 15);
        o->cram = R->cram[o->cram_index & 31];
        snprintf(o->text + n, sizeof o->text - (size_t)n,
                 "%s: backdrop colour (CRAM[%d] = $%02X)   #%06X\nThis row: R0 %02X R1 %02X  scroll %d,%d",
                 (R->flags & SMSVID_FLAG_BLANK) ? "Display off" : "Border",
                 o->cram_index, o->cram, line_rgb(c, R, o->cram_index) & 0xffffff,
                 R->reg[0], R->reg[1], R->x_scroll, R->y_scroll);
        return;
    }
    o->active = 1;
    o->column = sx;
    decode_line(c, R, L, &dl);
    o->cram_index = dl.idx[sx];
    o->cram = R->cram[o->cram_index & 31];
    o->colour_index = o->cram_index & 15;
    o->palette = o->cram_index >> 4;
    {
        bgcell b;
        const int fine = o->x_scroll & 7;
        int tc = sx < fine ? 0 : (sx - fine) >> 3;
        int pxl = sx < fine ? 0 : (sx - fine) & 7;
        bg_cell(c, R, L, tc, &b);
        o->tile_col = tc;
        o->tile_row = (int)(((b.name_addr - ((R->y_pixels != 192) ? (((R->reg[2] & 0x0c) << 10) | 0x0700)
                                                                  : ((R->reg[2] << 10) & 0x3800))) >> 6) & 31);
        o->name_addr = b.name_addr;
        o->name_entry = b.entry;
        o->tile = b.tile;
        o->hflip = b.hflip; o->vflip = b.vflip; o->priority = b.prio != 0;
        o->tile_x = b.hflip ? 7 - pxl : pxl;
        o->tile_y = b.tile_line;
        o->pattern_addr = b.pat1;
        o->y_scroll = b.ys;
        o->sprite = dl.spr[sx];
        if (sx < fine)
            n += snprintf(o->text + n, sizeof o->text - (size_t)n,
                          "Scroll fill: the %d pixel%s left of the first tile (colour %d of the palette tile $100 selects)\n",
                          fine, fine == 1 ? "" : "s", o->colour_index);
        else
            n += snprintf(o->text + n, sizeof o->text - (size_t)n,
                          "Name table cell %d,%d: entry $%04X at $%04X\n"
                          "Tile %d, %s palette%s%s%s\n"
                          "Pattern row at $%04X (planes %02X %02X %02X %02X), pixel %d,%d\n",
                          o->tile_col, o->tile_row, b.entry, b.name_addr, b.tile,
                          b.pal ? "sprite" : "background",
                          b.hflip ? ", h-flip" : "", b.vflip ? ", v-flip" : "", b.prio ? ", priority" : "",
                          b.pat1, c->snap.vram[b.pat1 & (VRAM_SIZE - 1)], c->snap.vram[(b.pat1 + 1) & (VRAM_SIZE - 1)],
                          c->snap.vram[(b.pat2 + 2) & (VRAM_SIZE - 1)], c->snap.vram[(b.pat2 + 3) & (VRAM_SIZE - 1)],
                          o->tile_x, o->tile_y);
        if (o->sprite >= 0) {
            spr_t sp[8];
            const int ns = sprites_on_line(c, R, L, sp);
            const unsigned sat = (unsigned)((R->reg[5] << 7) & 0x3f00);
            o->sprite_addr = sat + (unsigned)o->sprite;
            for (int i = 0; i < ns; i++)
                if (sp[i].idx == o->sprite) {
                    o->tile = sp[i].sel;
                    o->pattern_addr = (unsigned)(sp[i].sel << 5) + (unsigned)sp[i].pline;
                    o->tile_x = sx - sp[i].x;
                    o->tile_y = sp[i].pline >> 2;
                }
            n += snprintf(o->text + n, sizeof o->text - (size_t)n,
                          "Sprite %d (attributes at $%04X): tile %d, pattern row at $%04X\n",
                          o->sprite, o->sprite_addr, o->tile, o->pattern_addr);
        }
    }
    snprintf(o->text + n, sizeof o->text - (size_t)n,
             "Colour index %d of the %s palette -> CRAM[%d] = $%02X   #%06X\n"
             "Scroll this row: X %d (R8 %02X)  Y %d   R0 %02X R1 %02X",
             o->colour_index, o->palette ? "sprite" : "background", o->cram_index, o->cram,
             line_rgb(c, R, o->cram_index) & 0xffffff, o->x_scroll, R->x_scroll, o->y_scroll,
             R->reg[0], R->reg[1]);
}

int smsvid_tab_hover(smsvid_tab *t, int x, int y)
{
    char old[sizeof t->hover.text];
    if (!t) return 0;
    snprintf(old, sizeof old, "%s", t->hover.valid ? t->hover.text : "");
    t->hx = x; t->hy = y;
    locate(t, x, y, &t->hover);
    return strcmp(old, t->hover.valid ? t->hover.text : "") != 0;
}

void smsvid_tab_pin(smsvid_tab *t, int x, int y)
{
    int z;
    if (!t) return;
    z = t->ez ? t->ez : 1;
    if (t->px >= 0 && x / z == t->px / z && y / z == t->py / z) {
        smsvid_tab_unpin(t);
        return;
    }
    t->px = x; t->py = y;
    locate(t, x, y, &t->pin);
    if (!t->pin.valid) smsvid_tab_unpin(t);
}

void smsvid_tab_unpin(smsvid_tab *t)
{
    if (!t) return;
    t->px = t->py = -1;
    memset(&t->pin, 0, sizeof t->pin);
}

const smsvid_pixel *smsvid_tab_pixel(const smsvid_tab *t)
{
    if (!t) return NULL;
    if (t->pin.valid) return &t->pin;
    if (t->hover.valid) return &t->hover;
    return NULL;
}

int smsvid_tab_marks(const smsvid_tab *t, int hover[4], int pin[4])
{
    int r = 0, z;
    if (!t) return 0;
    z = t->ez ? t->ez : 1;
    if (t->hover.valid) {
        hover[0] = t->hover.x * z; hover[1] = t->hover.y * z; hover[2] = z; hover[3] = z;
        r |= 1;
    }
    if (t->pin.valid) {
        pin[0] = t->pin.x * z; pin[1] = t->pin.y * z; pin[2] = z; pin[3] = z;
        r |= 2;
    }
    return r;
}

/* a VRAM write breakpoint on [lo, hi], unless one is there; the id or -1 */
static int add_vram_bp(smsdebug *d, unsigned lo, unsigned hi)
{
    smsdebug_breakpoint bl[128];
    const int n = smsdebug_breakpoint_list(d, bl, 128);
    for (int i = 0; i < n; i++)
        if ((bl[i].type & SMSDEBUG_BP_VRAM) && bl[i].start == lo && bl[i].end == hi)
            return bl[i].id;
    return smsdebug_breakpoint_add(d, SMSDEBUG_BP_VRAM, (uint16_t)lo, (uint16_t)hi, NULL);
}

int smsvid_tab_break_on_pixel(smsvid_tab *t)
{
    const smsvid_pixel *p = smsvid_tab_pixel(t);
    int id, id2;

    if (!p || !t->d || p->tile < 0) return -1;
    if (!p->active && t->kind != BK_OTHER) return -1;
    id = add_vram_bp(t->d, p->pattern_addr & (VRAM_SIZE - 1), (p->pattern_addr + 3) & (VRAM_SIZE - 1));
    if (p->sprite >= 0) {
        const unsigned sat = p->sprite_addr - (unsigned)p->sprite;
        const unsigned extra = sat + 0x80u + (unsigned)p->sprite * 2u;
        id2 = add_vram_bp(t->d, extra & (VRAM_SIZE - 1), (extra + 1) & (VRAM_SIZE - 1));
        (void)id2;
        add_vram_bp(t->d, p->sprite_addr & (VRAM_SIZE - 1), p->sprite_addr & (VRAM_SIZE - 1));
    } else if (p->name_addr || p->tile_col >= 0) {
        id2 = add_vram_bp(t->d, p->name_addr & (VRAM_SIZE - 1), (p->name_addr + 1) & (VRAM_SIZE - 1));
        (void)id2;
    }
    return id;
}

int smsvid_tab_pixel_palette_index(const smsvid_tab *t)
{
    const smsvid_pixel *p = smsvid_tab_pixel(t);
    return p && p->valid && (p->active || t->kind == BK_OTHER) && p->cram_index >= 0 ? p->cram_index : -1;
}

int smsvid_tab_break_on_swatch(smsvid_tab *t, int index)
{
    smsdebug_breakpoint bl[128];
    int n;
    if (!t || !t->d || index < 0 || index > 31) return -1;
    n = smsdebug_breakpoint_list(t->d, bl, 128);
    for (int i = 0; i < n; i++)
        if ((bl[i].type & SMSDEBUG_BP_CRAM) && bl[i].start == index && bl[i].end == index)
            return bl[i].id;
    return smsdebug_breakpoint_add(t->d, SMSDEBUG_BP_CRAM, (uint16_t)index, (uint16_t)index, NULL);
}

/* ---- the tables ---------------------------------------------------------------------------- */

static void reg_set(smsvid_tab *t, int *n, const char *name, const char *value,
                    const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void reg_set(smsvid_tab *t, int *n, const char *name, const char *value,
                    const char *fmt, ...)
{
    smsvid_reg *r;
    va_list ap;
    int prev_age;
    if (*n >= MAX_REGS) return;
    r = &t->regs[*n];
    prev_age = r->age;
    r->name = name;
    snprintf(r->value, sizeof r->value, "%s", value);
    va_start(ap, fmt);
    vsnprintf(r->meaning, sizeof r->meaning, fmt, ap);
    va_end(ap);
    if (!t->regs_primed || strcmp(t->prev_val[*n], r->value) != 0)
        r->age = t->regs_primed ? 0 : 99;
    else
        r->age = prev_age < 99 ? prev_age + 1 : 99;
    memcpy(t->prev_val[*n], r->value, sizeof t->prev_val[*n]);
    (*n)++;
}

static void build_tables(smsvid_tab *t)
{
    const smsvid_capture *c = t->cap;
    const smsvdp_snap *s = &c->snap;
    static const char *const rn[11] = { "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "R10" };
    static const char *const bgn[16] = { "BG 0", "BG 1", "BG 2", "BG 3", "BG 4", "BG 5", "BG 6", "BG 7",
                                         "BG 8", "BG 9", "BG 10", "BG 11", "BG 12", "BG 13", "BG 14", "BG 15" };
    static const char *const sprn[16] = { "SPR 0", "SPR 1", "SPR 2", "SPR 3", "SPR 4", "SPR 5", "SPR 6", "SPR 7",
                                          "SPR 8", "SPR 9", "SPR 10", "SPR 11", "SPR 12", "SPR 13", "SPR 14", "SPR 15" };
    int n = 0;
    char v[24], m[112];

    /* registers (ages carry over from the previous build) */
    for (int i = 0; i < 11; i++) {
        const char *colon;
        snprintf(v, sizeof v, "%02X", s->reg[i]);
        smsvdp_describe_register(s, i, m, sizeof m);
        colon = strstr(m, ": ");
        reg_set(t, &n, rn[i], v, "%s", colon ? colon + 2 : m);
    }
    snprintf(v, sizeof v, "%d", s->mode);
    reg_set(t, &n, "MODE", v, "%s, %s", smsvdp_mode_name(s->mode, s->y_pixels), s->kind_5246 ? "315-5246" : "315-5124");
    snprintf(v, sizeof v, "%02X", c->status);
    reg_set(t, &n, "STATUS", v, "%s%s%s", (c->status & 0x80) ? "VINT " : "", (c->status & 0x40) ? "sprite overflow " : "",
            (c->status & 0x20) ? "sprite collision" : "");
    t->nregs = n;
    t->regs_primed = 1;

    /* colours: uses need a decode pass when the picture is not the Decoded screen / Values */
    if (mode4(t) && !(t->kind == BK_SCREEN))
        decode_screen(t, NULL, 0);
    else if (!mode4(t))
        memset(t->uses, 0, sizeof t->uses);
    {
        const smsvid_pixel *p = smsvid_tab_pixel(t);
        t->nsw = mode4(t) ? 32 : 16;
        for (int i = 0; i < t->nsw; i++) {
            t->sw[i].index = i;
            t->sw[i].name = i < 16 ? bgn[i] : sprn[i - 16];
            t->sw[i].cram = mode4(t) ? s->cram[i] : 0;
            t->sw[i].rgb = (mode4(t) ? s->pens[s->cram[i] & 0x3f] : s->pens[64 + i]) | 0xff000000u;
            t->sw[i].uses = t->uses[i];
            t->sw[i].inspected = p && p->valid && p->cram_index == i && (p->active || t->kind == BK_OTHER);
        }
    }

    /* rows: where the registers, scroll or palette change */
    t->nrows = 0;
    for (int y = 0; y < t->fb_h; y++) {
        const smsvid_line *R = &c->lines[y];
        int mask = 0;
        if (!R->valid) continue;
        if (y > 0 && c->lines[y - 1].valid) {
            const smsvid_line *P = &c->lines[y - 1];
            if (R->x_scroll != P->x_scroll) mask |= SMSVID_LC_XSCROLL;
            if (R->y_scroll != P->y_scroll) mask |= SMSVID_LC_YSCROLL;
            for (int i = 0; i < 11; i++)
                if (i != 8 && i != 9 && R->reg[i] != P->reg[i]) { mask |= SMSVID_LC_REGS; break; }
            if (R->mode != P->mode || R->y_pixels != P->y_pixels ||
                (R->flags & SMSVID_FLAG_BLANK) != (P->flags & SMSVID_FLAG_BLANK))
                mask |= SMSVID_LC_MODE;
            if (memcmp(R->cram, P->cram, sizeof R->cram)) mask |= SMSVID_LC_CRAM;
        }
        if (t->nrows == 0 || mask) {
            smsvid_line_row *r = &t->rows[t->nrows++];
            r->row = y;
            r->line = R->line;
            memcpy(r->reg, R->reg, sizeof r->reg);
            r->x_scroll = R->x_scroll; r->y_scroll = R->y_scroll;
            r->mode = R->mode; r->y_pixels = R->y_pixels;
            r->blank = (R->flags & SMSVID_FLAG_BLANK) != 0;
            r->hint = hint_fires(c, y);
            r->changed_mask = mask;
        }
    }

    /* timing */
    t->ntiming = 0;
#define TL(...) do { if (t->ntiming < MAX_TIMING) { \
        snprintf(t->timing_buf[t->ntiming], sizeof t->timing_buf[0], __VA_ARGS__); \
        t->timing_ptr[t->ntiming] = t->timing_buf[t->ntiming]; t->ntiming++; } } while (0)
    TL("Beam\tline %d of %d, pixel %d", c->vpos, c->total_lines, c->hpos);
    TL("V counter\t$%02X (%d)", c->vcount, c->vcount);
    TL("H counter\t$%02X (latched)", c->hcount);
    TL("Frame\t%llu", (unsigned long long)c->frame);
    TL("Standard\t%s, %d lines", c->is_pal ? "PAL" : "NTSC", c->total_lines);
    TL("Line counter\t%u, reload R10 = %u (every %u line%s)", c->line_counter, s->reg[10],
       s->reg[10] + 1u, s->reg[10] == 0 ? "" : "s");
    TL("Line interrupts\t%s, %s", (s->reg[0] & 0x10) ? "enabled" : "disabled", c->hint_pending ? "pending" : "none pending");
    TL("Frame interrupt\t%s, %s", (s->reg[1] & 0x20) ? "enabled" : "disabled", c->vint_pending ? "pending" : "none pending");
    TL("/INT line\t%s", c->int_line ? "asserted" : "released");
    TL("Changes this frame\t%d row%s", t->nrows > 0 ? t->nrows - 1 : 0, t->nrows == 2 ? "" : "s");
    TL("Machine\t%s", t->stopped ? "stopped" : "running");
#undef TL

    snprintf(t->status, sizeof t->status,
             "%s view%s, %s, %d change%s in the frame\n%s%s",
             k_view_names[t->view],
             t->view == SMSVID_V_DECODED ? (t->source == SMSVID_D_SCREEN ? " (screen)" : " (other)") : "",
             smsvdp_mode_name(s->mode, s->y_pixels), t->nrows > 0 ? t->nrows - 1 : 0, t->nrows == 2 ? "" : "s",
             t->stopped ? "Stopped" : "Running: showing the last finished frame",
             t->stopped ? (t->has_fb ? "" : " (no frame has finished yet)") : "");
}

/* ---- refresh -------------------------------------------------------------------------------- */

unsigned smsvid_tab_refresh(smsvid_tab *t, int force)
{
    uint64_t sig;
    unsigned r = 0;
    int fresh;

    if (!t) return 0;
    if (!acquire(t)) {
        why_set(t, "The machine is not running.");
        t->pw = t->ph = 0;
        return SMSVID_TAB_OFF | SMSVID_TAB_PICTURE | SMSVID_TAB_TABLES;
    }
    sig = signature(t);
    fresh = force || sig != t->sig;
    if (fresh) {
        t->sig = sig;
        note_vram(t);
    } else if (t->any_heat) {
        note_vram(t);               /* the fade goes on while nothing happens */
    }
    if (fresh || t->dirty || (t->any_heat && (t->ov & SMSVID_OV_HEAT))) {
        build_picture(t);
        t->dirty = 0;
        r |= SMSVID_TAB_PICTURE;
    }
    if (fresh) {
        if (t->hx >= 0) locate(t, t->hx, t->hy, &t->hover);
        if (t->px >= 0) locate(t, t->px, t->py, &t->pin);
        build_tables(t);
        r |= SMSVID_TAB_TABLES;
    }
    return r;
}

const uint32_t *smsvid_tab_picture(smsvid_tab *t, int *w, int *h)
{
    if (!t || !t->pw) return NULL;
    if (w) *w = t->pw;
    if (h) *h = t->ph;
    return t->pic;
}
const char *smsvid_tab_why(const smsvid_tab *t) { return t ? t->why : ""; }
int smsvid_tab_effective_zoom(const smsvid_tab *t) { return t ? t->ez : 1; }
const char *smsvid_tab_status(const smsvid_tab *t) { return t ? t->status : ""; }
int smsvid_tab_regs(const smsvid_tab *t, const smsvid_reg **out)
{
    if (out) *out = t->regs;
    return t->nregs;
}
int smsvid_tab_swatches(const smsvid_tab *t, const smsvid_swatch **out)
{
    /* the inspected pixel moves between refreshes */
    const smsvid_pixel *p = smsvid_tab_pixel(t);
    smsvid_swatch *sw = ((smsvid_tab *)t)->sw;
    for (int i = 0; i < t->nsw; i++)
        sw[i].inspected = p && p->valid && p->cram_index == i && (p->active || t->kind == BK_OTHER);
    if (out) *out = t->sw;
    return t->nsw;
}
int smsvid_tab_lines(const smsvid_tab *t, const smsvid_line_row **out)
{
    if (out) *out = t->rows;
    return t->nrows;
}
int smsvid_tab_timing(const smsvid_tab *t, const char *const **lines)
{
    if (lines) *lines = (const char *const *)t->timing_ptr;
    return t->ntiming;
}
int smsvid_tab_beam_permille(const smsvid_tab *t)
{
    return t && t->have && t->cap->total_lines > 0 ? t->cap->vpos * 1000 / t->cap->total_lines : 0;
}

int smsvid_tab_text(smsvid_tab *t, int section, char *dst, int dstsz)
{
    int n = 0;
#define P(...) do { if (n < dstsz) n += snprintf(dst + n, (size_t)(dstsz - n), __VA_ARGS__); } while (0)
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!t || !t->have) { P("The machine is not running.\n"); return n < dstsz ? n : dstsz - 1; }
    switch (section) {
    case SMSVID_S_REGISTERS:
        for (int i = 0; i < t->nregs; i++)
            P("%-7s %-4s %s\n", t->regs[i].name, t->regs[i].value, t->regs[i].meaning);
        break;
    case SMSVID_S_COLOURS:
        for (int i = 0; i < t->nsw; i++)
            P("%-7s CRAM %02X  #%06X  %ld pixels\n", t->sw[i].name, t->sw[i].cram,
              t->sw[i].rgb & 0xffffff, t->sw[i].uses);
        break;
    case SMSVID_S_LINES:
        for (int i = 0; i < t->nrows; i++) {
            const smsvid_line_row *r = &t->rows[i];
            P("row %3d line %4d  scroll %3u,%3u  R0 %02X R1 %02X R2 %02X R5 %02X R6 %02X R7 %02X R10 %02X  %s%s%s%s%s%s\n",
              r->row, r->line, r->x_scroll, r->y_scroll, r->reg[0], r->reg[1], r->reg[2], r->reg[5], r->reg[6],
              r->reg[7], r->reg[10], r->blank ? "off " : "", r->hint ? "hint " : "",
              (r->changed_mask & SMSVID_LC_XSCROLL) ? "xscroll " : "", (r->changed_mask & SMSVID_LC_YSCROLL) ? "yscroll " : "",
              (r->changed_mask & SMSVID_LC_REGS) ? "regs " : "", (r->changed_mask & SMSVID_LC_CRAM) ? "cram" : "");
        }
        break;
    case SMSVID_S_TIMING:
        for (int i = 0; i < t->ntiming; i++) P("%s\n", t->timing_buf[i]);
        break;
    default:
        P("%s\n", t->status);
        if (smsvid_tab_pixel(t)) P("%s\n", smsvid_tab_pixel(t)->text);
        break;
    }
#undef P
    return n < dstsz ? n : dstsz - 1;
}
