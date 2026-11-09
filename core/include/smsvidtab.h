/*
 * smsvidtab -- the Video tab's controller. All four frontends draw the same
 * tab from it, so what they own is widgets: a view and zoom chooser, overlay
 * toggles, a picture, the inspector and four tables. The lessons of the
 * other debuggers' video tabs live here once:
 *
 *   - the machine is only COPIED: a finished frame (picture, VRAM, registers
 *     and the registers each row was drawn with) is captured once per frame
 *     on the machine thread, and a stopped machine is read while it is
 *     parked. Decoding, scaling and overlays run on the caller's (UI) thread;
 *   - the picture is rebuilt only when something it shows changed, into one
 *     reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the
 *     picture: draw the marks from smsvid_tab_marks() over the cached image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with alpha;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * The VDP can change its scroll registers, its colours and even its mode
 * between rows (from the line interrupt, or code timed to the beam). The
 * Display view is the frame as the TV showed it; Decoded redraws VRAM
 * through each row's own registers and CRAM; Values shows the raw colour
 * indices (cool = background palette, warm = sprite palette). Mode 4 is
 * decoded row by row; in the TMS9918 modes (0-3) the Decoded and Values
 * views fall back to the debugger's name-table picture (vdpview.h), which
 * has no per-row registers.
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SMSVIDTAB_H
#define SMSVIDTAB_H

#include <stdint.h>

#include "smsdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct smsvid_tab smsvid_tab;
struct smsvid_capture;      /* smsvidcap.h; tests feed one */

enum smsvid_view {
    SMSVID_V_DISPLAY = 0,       /* the last finished frame, as the TV showed it */
    SMSVID_V_DECODED,           /* VRAM through each row's registers (see smsvid_source) */
    SMSVID_V_BEAM,              /* this frame above the beam, the last one dimmed below (stopped) */
    SMSVID_V_VALUES,            /* raw colour indices; cool = background palette, warm = sprites */
    SMSVID_V_COUNT
};

/* What Decoded shows. SCREEN is the picture re-rendered from VRAM; the rest
 * are the debugger's old VDP pictures. */
enum smsvid_source {
    SMSVID_D_SCREEN = 0,        /* 256 x 192/224/240, per-row scroll, palette, sprites */
    SMSVID_D_NAMETABLE,         /* the whole name table, the visible window outlined */
    SMSVID_D_TILES_BG,          /* all tiles under the background palette */
    SMSVID_D_TILES_SPR,         /* all tiles under the sprite palette */
    SMSVID_D_SPRITES,           /* every sprite in the table where it sits */
    SMSVID_D_PALETTE,           /* the 32 CRAM entries */
    SMSVID_D_COUNT
};

/* Overlays: one table, bit per entry. */
enum {
    SMSVID_OV_GRID    = 1u << 0,  /* 8x8 tile cells (they follow the scroll); pixels when zoomed in */
    SMSVID_OV_SCROLL  = 1u << 1,  /* scroll window: locked rows/columns, blank column; the visible window per row in Name table */
    SMSVID_OV_HINT    = 1u << 2,  /* rows where the line counter wrapped (a line interrupt) */
    SMSVID_OV_BEAM    = 1u << 3,  /* where the beam is (stopped) */
    SMSVID_OV_HEAT    = 1u << 4,  /* VRAM bytes written lately (fades) */
    SMSVID_OV_CHANGED = 1u << 5,  /* VRAM bytes changed since the last refresh */
    SMSVID_OV_ACTIVE  = 1u << 6,  /* outline of the active display (Display, Beam) */
    SMSVID_OV_SPRITES = 1u << 7,  /* a box and number on every sprite */
    SMSVID_OV_ALL     = 0xFFu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: grid, scroll, ... */
    const char *label;          /* the checkbox text */
} smsvid_overlay_info;
const smsvid_overlay_info *smsvid_overlays(int *count);
const char *smsvid_view_name(int view);            /* "Display", ... */
const char *smsvid_view_key(int view);             /* "display", ... (env) */
const char *smsvid_source_name(int source);        /* "Screen", "Name table", ... */
const char *smsvid_source_key(int source);         /* "screen", "nametable", ... (env) */

/* Right-hand sections. */
enum { SMSVID_S_PIXEL = 0, SMSVID_S_REGISTERS, SMSVID_S_COLOURS,
       SMSVID_S_LINES, SMSVID_S_TIMING, SMSVID_S_COUNT };
const char *smsvid_section_name(int section);
const char *smsvid_section_key(int section);

/* d is the debugger the tab asks whether the machine is stopped and adds
 * breakpoints to; NULL (tests) makes every Break on... return -1 and the
 * machine count as running. The tab asks the host to capture frames until
 * it is freed. */
smsvid_tab *smsvid_tab_new(smsdebug *d);
void        smsvid_tab_free(smsvid_tab *t);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     smsvid_tab_set_view(smsvid_tab *t, int view);
int      smsvid_tab_view(const smsvid_tab *t);
void     smsvid_tab_set_source(smsvid_tab *t, int source);   /* of Decoded */
int      smsvid_tab_source(const smsvid_tab *t);
void     smsvid_tab_set_overlays(smsvid_tab *t, unsigned ov);
unsigned smsvid_tab_overlays(const smsvid_tab *t);
/* 1, 2, 3, 4, 6, 8, 12, 16, or 0 for "fit the viewport" */
void     smsvid_tab_set_zoom(smsvid_tab *t, int zoom);
int      smsvid_tab_zoom(const smsvid_tab *t);
/* the room the picture has, for Fit; call when the widget resizes */
void     smsvid_tab_set_viewport(smsvid_tab *t, int w, int h);
/* the zoom steps the chooser offers (0 = Fit first) */
const int *smsvid_zooms(int *count);

/*
 * The scripted-capture variables, read once by smsvid_tab_new:
 *   SMS_DEBUGGER_VIDEO_VIEW      display decoded beam values, or a Decoded source:
 *                                screen nametable tiles tiles2 sprites palette
 *   SMS_DEBUGGER_VIDEO_OVERLAYS  grid,scroll,hint,beam,heat,changed,active,sprites | all | none
 *   SMS_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   SMS_DEBUGGER_VIDEO_SECTION   pixel registers colours lines timing   (names, never indexes)
 * Returns the starting section, SMSVID_S_PIXEL when unset.
 */
int smsvid_tab_env_section(const smsvid_tab *t);

/* ---- refresh ------------------------------------------------------------------------ */
#define SMSVID_TAB_PICTURE 1u  /* repaint the picture (smsvid_tab_picture) */
#define SMSVID_TAB_TABLES  2u  /* refill the status line and the sections */
#define SMSVID_TAB_OFF     4u  /* no machine: show smsvid_tab_why() instead of a picture */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. */
unsigned smsvid_tab_refresh(smsvid_tab *t, int force);

/* For tests: use this frame instead of the machine. live_fb is the frame in
 * progress (SMSVID_FB_W x cap->fb_height; NULL: a running machine, so no
 * Beam view); cap->beam_row says how far the beam has drawn it. */
void smsvid_tab_feed(smsvid_tab *t, const struct smsvid_capture *cap,
                     const uint32_t *live_fb);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*. NULL
 * when this view cannot be drawn: smsvid_tab_why() says why, and the tab
 * should show that text.
 */
const uint32_t *smsvid_tab_picture(smsvid_tab *t, int *w, int *h);
const char     *smsvid_tab_why(const smsvid_tab *t);
int             smsvid_tab_effective_zoom(const smsvid_tab *t);

/* ---- the tables -------------------------------------------------------------------- */
const char *smsvid_tab_status(const smsvid_tab *t);        /* two lines, '\n' between */

typedef struct {
    const char *name;           /* "R0", ..., "STATUS" */
    char value[24];             /* "36" */
    char meaning[112];
    int age;                    /* refreshes since it last changed (SMSVID_REG_FADE+: not recent) */
} smsvid_reg;
#define SMSVID_REG_FADE 12
int smsvid_tab_regs(const smsvid_tab *t, const smsvid_reg **out);

/* The 32 CRAM entries (16 fixed colours in the TMS modes). */
typedef struct {
    int index;                  /* CRAM index 0..31 (TMS: colour 0..15) */
    const char *name;           /* "BG 0" .. "BG 15", "SPR 0" .. "SPR 15" */
    uint8_t cram;               /* --BBGGRR as written */
    uint32_t rgb;               /* 0xFFRRGGBB */
    long uses;                  /* pixels of the Decoded screen that use it */
    int inspected;              /* the inspected pixel uses it */
} smsvid_swatch;
int smsvid_tab_swatches(const smsvid_tab *t, const smsvid_swatch **out);
/* A CRAM-write breakpoint (SMSDEBUG_BP_CRAM) on one entry; its id, or -1 */
int smsvid_tab_break_on_swatch(smsvid_tab *t, int index);

/* The per-row table: one entry for row 0 and for each row whose registers,
 * scroll or CRAM differ from the row above (the line counter does not count). */
enum { SMSVID_LC_XSCROLL = 1, SMSVID_LC_YSCROLL = 2, SMSVID_LC_REGS = 4,
       SMSVID_LC_MODE = 8, SMSVID_LC_CRAM = 16 };
typedef struct {
    int row;                    /* fb row 0..fb_height-1 */
    int line;                   /* active display line, <0 above, >= y_pixels below */
    uint8_t reg[11];
    uint8_t x_scroll, y_scroll; /* as latched for the row */
    uint8_t mode;
    int y_pixels;
    int blank;                  /* display off */
    int hint;                   /* a line interrupt fires on this row */
    int changed_mask;           /* SMSVID_LC_* */
} smsvid_line_row;
int smsvid_tab_lines(const smsvid_tab *t, const smsvid_line_row **out);

/* the timing section, as ready-made "label\tvalue" lines */
int  smsvid_tab_timing(const smsvid_tab *t, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  smsvid_tab_beam_permille(const smsvid_tab *t);

/* A section as plain text (the console's `video` command shares it). */
int  smsvid_tab_text(smsvid_tab *t, int section, char *dst, int dstsz);

/* ---- hover, pin ---------------------------------------------------------------------- */
typedef struct {
    int valid;
    int active;                 /* a mode 4 pixel of the active display: it reads VRAM */
    int x, y;                   /* picture pixel */
    int row;                    /* fb row, -1 in a picture that is not the frame */
    int line;                   /* active display line */
    int column;                 /* screen x within the 256 active pixels, -1 if outside */
    int tile_col, tile_row;     /* name table cell, -1 if none */
    unsigned name_addr;         /* VRAM address of the name table entry */
    unsigned name_entry;        /* the 16-bit entry */
    int tile;                   /* tile number 0..511 (sprite tile for a sprite pixel) */
    int palette;                /* 0 background, 1 sprite palette */
    int hflip, vflip, priority;
    int tile_x, tile_y;         /* the pixel inside the tile, after flips */
    unsigned pattern_addr;      /* VRAM address of the tile row's bit plane 0 (planes 1-3 follow) */
    int colour_index;           /* 0..15 inside the palette */
    int cram_index;             /* 0..31 */
    uint8_t cram;               /* the CRAM entry */
    uint32_t rgb;               /* 0xFFRRGGBB */
    int sprite;                 /* sprite number whose pixel it is, -1 for the background */
    unsigned sprite_addr;       /* VRAM address of that sprite's Y byte */
    int x_scroll, y_scroll;     /* the scroll this pixel was drawn with */
    char text[768];             /* ready-made inspector text */
} smsvid_pixel;

/* x, y in picture pixels at the current zoom; -1,-1 when the pointer left.
 * Returns 1 when the inspector changed. */
int  smsvid_tab_hover(smsvid_tab *t, int x, int y);
void smsvid_tab_pin(smsvid_tab *t, int x, int y);      /* a click; the same spot unpins */
void smsvid_tab_unpin(smsvid_tab *t);
/* the pinned pixel if any, else the hovered one, else NULL */
const smsvid_pixel *smsvid_tab_pixel(const smsvid_tab *t);
/* rectangles x,y,w,h in picture pixels for the marks drawn over the picture;
 * bit0 of the result: hover valid, bit1: pin valid */
int  smsvid_tab_marks(const smsvid_tab *t, int hover[4], int pin[4]);
/* VRAM write breakpoints (SMSDEBUG_BP_VRAM) on the inspected pixel's tile
 * pattern bytes and on its name table entry (a sprite pixel: the pattern and
 * the sprite's attribute bytes). Not added twice. The first id, or -1. */
int  smsvid_tab_break_on_pixel(smsvid_tab *t);
/* the CRAM index the inspected pixel uses, or -1 */
int  smsvid_tab_pixel_palette_index(const smsvid_tab *t);

#ifdef __cplusplus
}
#endif

#endif
