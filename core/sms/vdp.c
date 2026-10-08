// license:BSD-3-Clause
// copyright-holders:Wilbert Pol, Enik Land
/* vdp.c -- the Sega 315-5124 (SMS1) and 315-5246 (SMS2) VDPs.
 *
 * Transposed from MAME src/devices/video/315_5124.cpp (BSD-3-Clause,
 * copyright-holders Wilbert Pol, Enik Land; see COMPLIANCE.md). The register
 * file, the status and pending-flag logic, the four TMS9918 modes and mode
 * 4, the sprite engine and the palettes are MAME's, line for line, under
 * MAME's own names. The Game Gear (315-5377) and Mega Drive mode 4
 * (315-5313) variants are not carried over.
 *
 * What changes is the clockwork. MAME drives the chip with emu_timers placed
 * by screen().time_until_pos(): a periodic one at hpos 14 of every line
 * (process_line_timer), one at hpos 341 (eol_flag_check), and one-shots the
 * line processing re-arms -- VINT at 24, HINT at 26, the NMI sample at 28,
 * the left border at 50, the line draw at 63, the right border at 319. Here
 * the same timers are a per-line event table run against the CPU's cycle
 * count, in time order with the CPU's accesses:
 *
 *   - Time is in master clocks (the VDP clock; 3 per T-state, 2 per pixel,
 *     684 per line) since power-on, and power-on is MAME's VBLANK origin:
 *     the beam starts at the line after the visible area, hpos 0.
 *   - An event at master clock E is visible to a CPU access made at cycle
 *     c when c >= floor(E / 3): MAME runs the CPU for floor(delta / cycle)
 *     cycles before the timer fires, and a CPU access is made at the start
 *     of its machine cycle.
 *   - vpos/hpos reads round to the nearest pixel, as screen().vpos() and
 *     hpos() do (half a pixel is one master clock).
 */

#include <string.h>

#include "sms_internal.h"

#define STATUS_VINT   0x80  /* Pending vertical interrupt flag */
#define STATUS_SPROVR 0x40  /* Sprite overflow flag */
#define STATUS_SPRCOL 0x20  /* Object collision flag */

#define PRIORITY_BIT  0x1000
#define BACKDROP_COLOR(v) (((v)->vdp_mode == 4 ? 0x10 : 0x00) + ((v)->reg[0x07] & 0x0f))
#define BIT(x, n) (((x) >> (n)) & 1)

#define VERTICAL_SYNC    0
#define TOP_BLANKING     1
#define TOP_BORDER       2
#define ACTIVE_DISPLAY_V 3
#define BOTTOM_BORDER    4
#define BOTTOM_BLANKING  5

static const uint8_t ntsc_192[6] = { 3, 13, 27, 192, 24, 3 };
static const uint8_t ntsc_224[6] = { 3, 13, 11, 224,  8, 3 };
static const uint8_t ntsc_240[6] = { 3, 13,  3, 240,  0, 3 };
static const uint8_t pal_192[6]  = { 3, 13, 54, 192, 48, 3 };
static const uint8_t pal_224[6]  = { 3, 13, 38, 224, 32, 3 };
static const uint8_t pal_240[6]  = { 3, 13, 30, 240, 24, 3 };

/* line_315_5124[] */
#define VINT_HPOS          24
#define VINT_FLAG_HPOS     24
#define HINT_HPOS          26
#define NMI_HPOS           28
#define XSCROLL_HPOS       21
#define VCOUNT_CHANGE_HPOS 23
#define SPROVR_HPOS        24
#define SPRCOL_BASEHPOS    59

#define DISPLAY_DISABLED_HPOS 24
#define DISPLAY_CB_HPOS       14
#define DRAW_TIME_SMS         63

/* The event table, in hpos order (see sms_vdp_event_t). */
static const int ev_hpos[SMS_EV_COUNT] = {
    DISPLAY_CB_HPOS,                                   /* process_line_timer */
    VINT_HPOS,                                         /* trigger_vint */
    HINT_HPOS,                                         /* trigger_hint */
    NMI_HPOS,                                          /* update_nmi */
    SMS_LBORDER_START,                                 /* draw_lborder */
    DRAW_TIME_SMS,                                     /* draw_scanline */
    SMS_LBORDER_START + SMS_LBORDER_WIDTH + 256,       /* draw_rborder */
    SMS_VDP_WIDTH - 1,                                 /* eol_flag_check */
};

/* The two events that recur on every line; the rest are one-shots. */
#define EV_PERIODIC ((1u << SMS_EV_PROCESS_LINE) | (1u << SMS_EV_EOL))

/* screen().hpos() adds half a pixel before dividing, to round to the
 * nearest; how that lands against the CPU's whole cycles is calibrated
 * against MAME (tools/ab/probes/vhcount.asm). */
#ifndef BEAM_ROUND_MCLK
#define BEAM_ROUND_MCLK 0
#endif

/* An event at master clock E is visible from CPU cycle ev_cycle(E). */
static inline uint64_t ev_cycle(uint64_t e)
{
    return e / SMS_MCLK_PER_CYCLE;
}

/* ---------------------------------------------------------------------- */
/* palettes                                                               */
/* ---------------------------------------------------------------------- */

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* sega315_5124_palette: the blue channel is non-linear (die shot), and
 * modes 0-3 use the fixed TMS9918-style set at 64+. */
static void palette_5124(uint32_t *pens)
{
    static const uint8_t level[4] = { 0, 78, 160, 238 };
    static const uint8_t blue_level[4] = { 0, 98, 160, 238 };
    static const uint8_t tms[16][3] = {
        {0,0,0},{0,0,0},{0,2,0},{0,3,0},{0,0,1},{0,0,3},{1,0,0},{0,3,3},
        {2,0,0},{3,0,0},{1,1,0},{3,3,0},{0,1,0},{3,0,3},{1,1,1},{3,3,3},
    };
    for (int i = 0; i < 64; i++)
        pens[i] = rgb(level[i & 3], level[(i >> 2) & 3], blue_level[(i >> 4) & 3]);
    for (int i = 0; i < 16; i++)
        pens[64 + i] = rgb(level[tms[i][0]], level[tms[i][1]], blue_level[tms[i][2]]);
}

/* sega315_5246_palette: linear levels, a bit different from the 315-5124. */
static void palette_5246(uint32_t *pens)
{
    static const uint8_t level[4] = { 0, 89, 174, 255 };
    static const uint8_t tms[16][3] = {
        {0,0,0},{0,0,0},{0,2,0},{0,3,0},{0,0,1},{0,0,3},{1,0,0},{0,3,3},
        {2,0,0},{3,0,0},{1,1,0},{3,3,0},{0,1,0},{3,0,3},{1,1,1},{3,3,3},
    };
    for (int i = 0; i < 64; i++)
        pens[i] = rgb(level[i & 3], level[(i >> 2) & 3], level[(i >> 4) & 3]);
    for (int i = 0; i < 16; i++)
        pens[64 + i] = rgb(level[tms[i][0]], level[tms[i][1]], level[tms[i][2]]);
}

/* ---------------------------------------------------------------------- */
/* the beam                                                               */
/* ---------------------------------------------------------------------- */

static inline uint8_t vram_r(const sms_vdp_t *v, unsigned a)
{
    return v->vram[a & (SMS_VRAM_SIZE - 1)];
}

static inline uint16_t vram_rw(const sms_vdp_t *v, unsigned a)
{
    return (uint16_t)(vram_r(v, a) | (vram_r(v, a + 1) << 8));
}

/* Beam position at master clock `t`, rounded to the nearest pixel exactly
 * as screen().vpos()/hpos() round (delta += pixeltime / 2). The event
 * engine's (line_mclk, vpos) is the reference point; `t` may be a little
 * before it (the engine moves to a line at its hpos 341 event, which is
 * visible a fraction of a cycle early). */
static void beam_at(const sms_vdp_t *v, uint64_t t, int *vpos, int *hpos)
{
    int64_t d = (int64_t)(t + BEAM_ROUND_MCLK) - (int64_t)v->line_mclk;
    int64_t lines = d >= 0 ? d / SMS_MCLK_PER_LINE
                           : -((-d + SMS_MCLK_PER_LINE - 1) / SMS_MCLK_PER_LINE);
    int64_t within = d - lines * SMS_MCLK_PER_LINE;
    int64_t vp = (v->vpos + lines) % v->lines;

    if (vp < 0)
        vp += v->lines;
    *vpos = (int)vp;
    *hpos = (int)(within / SMS_MCLK_PER_PIXEL);
}

int sms_vdp_beam_vpos(const sms_vdp_t *v, uint64_t cycle)
{
    int vp, hp;
    beam_at(v, cycle * SMS_MCLK_PER_CYCLE, &vp, &hp);
    return vp;
}

int sms_vdp_beam_hpos(const sms_vdp_t *v, uint64_t cycle)
{
    int vp, hp;
    beam_at(v, cycle * SMS_MCLK_PER_CYCLE, &vp, &hp);
    return hp;
}

static int screen_hpos(const sms_vdp_t *v, uint64_t cycle)
{
    return sms_vdp_beam_hpos(v, cycle);
}

/* ---------------------------------------------------------------------- */
/* display modes                                                          */
/* ---------------------------------------------------------------------- */

static void select_extended_res_mode4(sms_vdp_t *v, bool M1, bool M2, bool M3)
{
    if (v->kind != SMS_VDP_5246)
        return;   /* the 315-5124 has no extended resolution */
    if (M2)
    {
        if (M1 && !M3)
            v->y_pixels = 224;   /* 224-line display */
        else if (!M1 && M3)
            v->y_pixels = 240;   /* 240-line display */
    }
}

static void select_display_mode(sms_vdp_t *v)
{
    const bool M1 = BIT(v->reg[0x01], 4);
    const bool M2 = BIT(v->reg[0x00], 1);
    const bool M3 = BIT(v->reg[0x01], 3);
    const bool M4 = BIT(v->reg[0x00], 2);

    if (M4)
    {
        v->vdp_mode = 4;
        select_extended_res_mode4(v, M1, M2, M3);
    }
    else
    {
        if (!M1 && !M2 && !M3)      /* Mode 0 (Graphics I Mode) */
            v->vdp_mode = 0;
        else if (M1 && !M2 && !M3)  /* Mode 1 (Text Mode) */
            v->vdp_mode = 1;
        else if (!M1 && M2 && !M3)  /* Mode 2 (Graphics II Mode) */
            v->vdp_mode = 2;
        else if (!M1 && !M2 && M3)  /* Mode 3 (Multicolor Mode) */
            v->vdp_mode = 3;
        /* else: unknown mode, the previous one stays (MAME only logs) */
    }
}

static void set_frame_timing(sms_vdp_t *v)
{
    switch (v->y_pixels)
    {
    case 192: v->frame_timing = v->is_pal ? pal_192 : ntsc_192; break;
    case 224: v->frame_timing = v->is_pal ? pal_224 : ntsc_224; break;
    case 240: v->frame_timing = v->is_pal ? pal_240 : ntsc_240; break;
    }
}

static void set_display_settings(sms_vdp_t *v)
{
    v->y_pixels = 192;
    select_display_mode(v);
    set_frame_timing(v);
    v->cram_dirty = true;
}

/* ---------------------------------------------------------------------- */
/* counters and flags                                                     */
/* ---------------------------------------------------------------------- */

static uint8_t vcount(const sms_vdp_t *v, uint64_t cycle)
{
    const int active_scr_start = v->frame_timing[VERTICAL_SYNC]
                               + v->frame_timing[TOP_BLANKING]
                               + v->frame_timing[TOP_BORDER];
    int vpos, hpos;

    beam_at(v, cycle * SMS_MCLK_PER_CYCLE, &vpos, &hpos);
    if (hpos < VCOUNT_CHANGE_HPOS)
    {
        vpos--;
        if (vpos < 0)
            vpos += v->lines;
    }
    return (uint8_t)((vpos - active_scr_start) & 0xff);
}

uint8_t sms_vdp_vcount_read(const sms_vdp_t *v, uint64_t cycle)
{
    return vcount(v, cycle);
}

uint8_t sms_vdp_hcount_read(sms_vdp_t *v)
{
    v->hcounter_latched = false;
    return v->hcounter;
}

static uint8_t hcount(const sms_vdp_t *v, uint64_t cycle)
{
    /* The hcount value returned by the VDP seems to be based on the
     * previous hpos */
    int hclock = screen_hpos(v, cycle) - 1;
    if (hclock < 0)
        hclock += SMS_VDP_WIDTH;
    /* MAME's >> 1 on a possibly negative int: an arithmetic (floor) shift
     * on every compiler it builds with */
    int d = hclock - 46;
    return (uint8_t)((d >= 0 ? d / 2 : -((-d + 1) / 2)) & 0xff);
}

void sms_vdp_hcount_latch(sms_vdp_t *v, uint64_t cycle)
{
    v->hcounter = hcount(v, cycle);
    v->hcounter_latched = true;
}

/* `hpos` is screen_hpos() for an access, or WIDTH - 1 from the end-of-line
 * check (MAME passes that when its pending-flags timer is due). */
static void check_pending_flags(sms_vdp_t *v, int hpos)
{
    if (!(v->pending_status & (STATUS_VINT | STATUS_SPROVR | STATUS_SPRCOL)) && !v->pending_hint)
        return;

    if (v->pending_hint && hpos >= HINT_HPOS)
    {
        v->pending_hint = false;
        v->hint_occurred = true;
    }
    if ((v->pending_status & STATUS_VINT) && hpos >= VINT_FLAG_HPOS)
    {
        v->pending_status &= ~STATUS_VINT;
        v->status |= STATUS_VINT;
    }
    if ((v->pending_status & STATUS_SPROVR) && hpos >= SPROVR_HPOS)
    {
        v->pending_status &= ~STATUS_SPROVR;
        v->status |= STATUS_SPROVR;
        /* copy and reset the pending bits that were based on the number of
         * the first sprite that overflowed */
        v->status &= v->pending_status | (STATUS_VINT | STATUS_SPROVR | STATUS_SPRCOL);
        v->pending_status |= (uint8_t)~(STATUS_VINT | STATUS_SPROVR | STATUS_SPRCOL);
    }
    if ((v->pending_status & STATUS_SPRCOL) && hpos >= v->pending_sprcol_x)
    {
        v->pending_status &= ~STATUS_SPRCOL;
        v->status |= STATUS_SPRCOL;
        v->pending_sprcol_x = 0;
    }
}

/* ---------------------------------------------------------------------- */
/* the ports                                                              */
/* ---------------------------------------------------------------------- */

uint8_t sms_vdp_data_read(sms_vdp_t *v, bool commit)
{
    /* Return data buffer contents */
    const uint8_t temp = v->buffer;

    if (commit)
    {
        /* Clear pending write flag */
        v->pending_control_write = false;
        /* Load data buffer */
        v->buffer = vram_r(v, v->addr & 0x3fff);
        /* Bump internal address register */
        v->addr += 1;
    }
    return temp;
}

uint8_t sms_vdp_control_read(sms_vdp_t *v, uint64_t cycle, bool commit)
{
    if (!commit)
        return v->status;

    check_pending_flags(v, screen_hpos(v, cycle));
    const uint8_t result = v->status;

    /* Clear pending write flag */
    v->pending_control_write = false;

    /* Clear status flags */
    v->hint_occurred = false;
    v->status = (uint8_t)~(STATUS_VINT | STATUS_SPROVR | STATUS_SPRCOL);

    if (v->n_int_state == 0)
        v->n_int_state = 1;

    return result;
}

static void cram_write(sms_vdp_t *v, uint8_t data)
{
    const uint16_t address = v->addr & (SMS_CRAM_SIZE - 1);
    if (data != v->CRAM[address])
    {
        v->CRAM[address] = data;
        v->cram_dirty = true;
    }
}

static void write_memory(sms_vdp_t *v, uint8_t data)
{
    switch (v->addrmode)
    {
    case 0x00:
    case 0x01:
    case 0x02:
        v->vram[v->addr & 0x3fff] = data;
        break;
    case 0x03:
        cram_write(v, data);
        break;
    }
    /* data written to data port loads the data buffer */
    v->buffer = data;
}

void sms_vdp_data_write(sms_vdp_t *v, uint8_t data)
{
    /* Clear pending write flag */
    v->pending_control_write = false;

    write_memory(v, data);
    v->addr += 1;
}

static void load_vram_addr(sms_vdp_t *v, uint8_t data)
{
    /* Seems like the latched data is passed straight through to the address
     * register when in the middle of doing a command. Cosmic Spacehead needs
     * this, among others */
    if (v->pending_control_write)
        v->addr = (uint16_t)((v->addr & 0xff00) | data);
    else
        v->addr = (uint16_t)((data << 8) | (v->addr & 0xff));
}

void sms_vdp_control_write(sms_vdp_t *v, uint8_t data, uint64_t cycle)
{
    if (!v->pending_control_write)
    {
        v->pending_control_write = true;
        load_vram_addr(v, data);
        return;
    }

    /* Clear pending write flag */
    v->pending_control_write = false;

    v->addrmode = (data >> 6) & 0x03;
    load_vram_addr(v, data);
    switch (v->addrmode)
    {
    case 0:     /* VRAM reading mode */
        v->buffer = vram_r(v, v->addr & 0x3fff);
        v->addr += 1;
        break;

    case 1:     /* VRAM writing mode */
        break;

    case 2:     /* VDP register write */
    {
        const int reg_num = data & 0x0f;   /* m_reg_num_mask */
        v->reg[reg_num] = v->addr & 0xff;

        switch (reg_num)
        {
        case 0:
            set_display_settings(v);
            break;
        case 1:
            set_display_settings(v);
            if (screen_hpos(v, cycle) <= DISPLAY_DISABLED_HPOS)
                v->display_disabled = !BIT(v->reg[0x01], 6);
            break;
        case 8:
            if (screen_hpos(v, cycle) <= XSCROLL_HPOS)
                v->reg8copy = v->reg[0x08];
            break;
        }

        check_pending_flags(v, screen_hpos(v, cycle));

        if ((reg_num == 0 && v->hint_occurred) || (reg_num == 1 && (v->status & STATUS_VINT)))
        {
            /* For HINT disabling through register 00: "Line IRQ VCount" test,
             * of Flubba's VDPTest ROM, disables HINT to wait for next VINT,
             * but HINT occurs when the operation is about to execute.
             * For VINT disabling through register 01: eagles5 on smskr sets
             * register 01 to 0x02 and expects the irq state to be cleared.
             * For register 01 and VINT enabling: robocop3's scoreboard on
             * sms/smspal needs /INT asserted. Same assumed for reg0+HINT. */
            if ((reg_num == 0 && !BIT(v->reg[0x00], 4)) ||
                (reg_num == 1 && !BIT(v->reg[0x01], 5)))
            {
                if (v->n_int_state == 0)
                    v->n_int_state = 1;
            }
            else
            {
                v->n_int_state = 0;
            }
        }
        v->addrmode = 0;
        break;
    }

    case 3:     /* CRAM writing mode */
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* rendering                                                              */
/* ---------------------------------------------------------------------- */

static void update_palette(sms_vdp_t *v)
{
    /* Exit if palette has no changes */
    if (!v->cram_dirty)
        return;
    v->cram_dirty = false;

    if (v->vdp_mode != 4)
    {
        for (int i = 0; i < 16; i++)
            v->current_palette[i] = 64 + i;
        return;
    }
    for (int i = 0; i < 32; i++)
        v->current_palette[i] = v->CRAM[i] & 0x3f;
}

/* A raster row (vpos), clipped to the visible window. */
static uint32_t *fb_row(sms_vdp_t *v, int row)
{
    const int y = row - v->vis_y0;
    if (y < 0 || y >= v->fb_height)
        return NULL;
    return &v->fb[y * SMS_FB_WIDTH];
}

/* Fill raster columns [x0, x1] of `row` with a pen. */
static void fill_row(sms_vdp_t *v, int row, int x0, int x1, uint32_t pen)
{
    uint32_t *p = fb_row(v, row);
    if (!p)
        return;
    if (x0 < SMS_VIS_X0)
        x0 = SMS_VIS_X0;
    if (x1 > SMS_VIS_X1 - 1)
        x1 = SMS_VIS_X1 - 1;
    for (int x = x0; x <= x1; x++)
        p[x - SMS_VIS_X0] = pen;
}

static void draw_lborder(sms_vdp_t *v, int param)
{
    update_palette(v);
    /* Draw left border */
    fill_row(v, param, SMS_LBORDER_START, SMS_LBORDER_START + SMS_LBORDER_WIDTH - 1,
             v->pens[v->current_palette[BACKDROP_COLOR(v)]]);
}

static void draw_rborder(sms_vdp_t *v, int param)
{
    update_palette(v);
    /* Draw right border */
    const int x0 = SMS_LBORDER_START + SMS_LBORDER_WIDTH + 256;
    fill_row(v, param, x0, x0 + SMS_RBORDER_WIDTH - 1,
             v->pens[v->current_palette[BACKDROP_COLOR(v)]]);
}

static void draw_leftmost_pixels_mode4(sms_vdp_t *v, int *line_buffer, int *priority_selected,
                                       int fine_x_scroll, int palette_selected, int tile_line)
{
    /* To draw the leftmost pixels when they aren't part of a tile column due
     * to scrolling, the Master System selects which palette gives the color
     * in entry 0 from the content of tile 0x100 (mimicking Emulicious, per
     * sverx's test ROM): bit 1 of the plane that would select the color for
     * pixel 4 of the current line. */
    const int pixel_x = 4;
    /* The parsing seems to occur before the line counter is incremented to
     * the number of the line to be drawn. */
    const int parse_line = tile_line - 1;
    const int tile_selected = 0x100;
    const int tmp_bit_plane_1 = vram_r(v, (tile_selected << 5) + ((parse_line & 0x07) << 2) + 0x01);
    const uint8_t pen_bit_1 = BIT(tmp_bit_plane_1, 7 - pixel_x);

    (void)palette_selected;
    for (int pixel_plot_x = 0; pixel_plot_x < fine_x_scroll; pixel_plot_x++)
    {
        line_buffer[pixel_plot_x] = v->current_palette[pen_bit_1 ? 0x10 : 0x00];
        priority_selected[pixel_plot_x] = 0;
    }
}

static uint16_t name_row_mode4(const sms_vdp_t *v, uint16_t row)
{
    if (v->kind == SMS_VDP_5246)
        return row;
    return row & (((v->reg[0x02] & 0x01) << 10) | 0x3bff);
}

static uint16_t tile1_select_mode4(const sms_vdp_t *v, uint16_t tile_number)
{
    if (v->kind == SMS_VDP_5246)
        return tile_number;
    return tile_number & ((v->reg[0x03] << 1) | 1);
}

static uint16_t tile2_select_mode4(const sms_vdp_t *v, uint16_t tile_number)
{
    if (v->kind == SMS_VDP_5246)
        return tile_number;
    return tile_number & (((v->reg[0x04] & 0x07) << 6) | 0x03f);
}

static uint8_t sprite_attribute_extra_offset_mode4(const sms_vdp_t *v, uint8_t offset)
{
    if (v->kind == SMS_VDP_5246)
        return offset;
    return offset & ((BIT(v->reg[0x05], 0) << 7) | 0x7f);
}

static uint8_t sprite_tile_select_mode4(const sms_vdp_t *v, uint8_t tile_number)
{
    if (v->kind == SMS_VDP_5246)
        return tile_number;
    return tile_number & (((v->reg[0x06] & 0x03) << 6) | 0x3f);
}

static void draw_scanline_mode4(sms_vdp_t *v, int *line_buffer, int *priority_selected, int line)
{
    /* if top 2 rows of screen not affected by horizontal scrolling, then
     * x_scroll = 0, else x_scroll = reg8copy */
    const int x_scroll = ((BIT(v->reg[0x00], 6) && (line < 16)) ? 0 : v->reg8copy);
    const int x_scroll_start_column = 32 - (x_scroll >> 3);   /* x starting column tile */
    const int fine_x_scroll = (x_scroll & 0x07);
    int scroll_mod;
    uint16_t name_base;

    if (v->y_pixels != 192)
    {
        name_base = (uint16_t)(((v->reg[0x02] & 0x0c) << 10) | 0x0700);
        scroll_mod = 256;
    }
    else
    {
        name_base = (uint16_t)((v->reg[0x02] << 10) & 0x3800);
        scroll_mod = 224;
    }

    /* Draw background layer */
    for (int tile_column = 0; tile_column < 32; tile_column++)
    {
        int tile_line;
        const int table_column = ((tile_column + x_scroll_start_column) & 0x1f) << 1;
        /* Rightmost 8 columns not affected by vertical scrolling when bit 7
         * of reg[0x00] is set */
        const int y_scroll = (BIT(v->reg[0x00], 7) && (tile_column > 23)) ? 0 : v->reg9copy;
        const uint16_t name_row = name_row_mode4(v, (uint16_t)((((line + y_scroll) % scroll_mod) >> 3) << 6));
        const uint16_t tile_data = vram_rw(v, name_base + name_row + table_column);
        const int tile1_selected = tile1_select_mode4(v, tile_data & 0x01ff);
        const int tile2_selected = tile2_select_mode4(v, tile_data & 0x01ff);
        const int priority_select = tile_data & PRIORITY_BIT;
        const int palette_selected = BIT(tile_data, 11);
        const int vert_selected = BIT(tile_data, 10);
        const int horiz_selected = BIT(tile_data, 9);

        tile_line = line - ((0x07 - (y_scroll & 0x07)) + 1);
        if (vert_selected)
            tile_line = 0x07 - tile_line;

        const uint8_t bit_plane_0 = vram_r(v, ((tile1_selected << 5) + ((tile_line & 0x07) << 2)) + 0x00);
        const uint8_t bit_plane_1 = vram_r(v, ((tile1_selected << 5) + ((tile_line & 0x07) << 2)) + 0x01);
        const uint8_t bit_plane_2 = vram_r(v, ((tile2_selected << 5) + ((tile_line & 0x07) << 2)) + 0x02);
        const uint8_t bit_plane_3 = vram_r(v, ((tile2_selected << 5) + ((tile_line & 0x07) << 2)) + 0x03);

        /* Column 0 is the leftmost tile column that completely entered in
         * the screen. If the leftmost pixels aren't part of a complete tile,
         * due to horizontal scrolling, they are drawn only with color #0 of
         * the selected palette. */
        if (tile_column == 0 && fine_x_scroll > 0)
            draw_leftmost_pixels_mode4(v, line_buffer, priority_selected, fine_x_scroll,
                                       palette_selected, tile_line);

        for (int pixel_x = 0; pixel_x < 8; pixel_x++)
        {
            const uint8_t pen_bit_0 = BIT(bit_plane_0, 7 - pixel_x);
            const uint8_t pen_bit_1 = BIT(bit_plane_1, 7 - pixel_x);
            const uint8_t pen_bit_2 = BIT(bit_plane_2, 7 - pixel_x);
            const uint8_t pen_bit_3 = BIT(bit_plane_3, 7 - pixel_x);
            uint8_t pen_selected = (uint8_t)(pen_bit_3 << 3 | pen_bit_2 << 2 | pen_bit_1 << 1 | pen_bit_0);
            if (palette_selected)
                pen_selected |= 0x10;

            int pixel_plot_x = !horiz_selected ? pixel_x : (7 - pixel_x);
            pixel_plot_x = fine_x_scroll + (tile_column << 3) + pixel_plot_x;
            if (pixel_plot_x < 256)
            {
                line_buffer[pixel_plot_x] = v->current_palette[pen_selected];
                priority_selected[pixel_plot_x] = priority_select | (pen_selected & 0x0f);
            }
        }
    }
}

static void sprite_count_overflow(sms_vdp_t *v, int line, int sprite_index)
{
    /* Overflow is flagged only on active display and when VINT isn't
     * active */
    if (!(v->status & STATUS_VINT) && line >= 0 && line < v->frame_timing[ACTIVE_DISPLAY_V])
    {
        uint8_t sprite_number_bits;

        v->pending_status |= STATUS_SPROVR;
        if (sprite_index < 14)
            sprite_number_bits = (uint8_t)((sprite_index + 1) / 2);
        else
            sprite_number_bits = (uint8_t)(sprite_index / 2);
        v->pending_status &= sprite_number_bits | (STATUS_VINT | STATUS_SPROVR | STATUS_SPRCOL);
    }
}

static void select_sprites(sms_vdp_t *v, int line)
{
    v->sprite_count = 0;

    if (v->vdp_mode == 1)
        return;   /* Text mode, no sprite processing */

    /* Check if SI is set */
    v->sprite_height = BIT(v->reg[0x01], 1) ? 16 : 8;
    /* Check if MAG is set */
    v->sprite_zoom_scale = BIT(v->reg[0x01], 0) ? 2 : 1;

    if (v->vdp_mode < 4)
    {
        /* TMS9918 compatibility sprites */
        const int max_sprites = 4;

        v->sprite_attribute_base = (uint16_t)((v->reg[0x05] & 0x7f) << 7);
        for (int sprite_index = 0; sprite_index < 32 * 4; sprite_index += 4)
        {
            /* At this point the VDP vcount still doesn't refer the new line,
             * because the logical start point is slightly shifted on the
             * scanline */
            int parse_line = line - 1;
            int sprite_y = vram_r(v, v->sprite_attribute_base + sprite_index);

            if (sprite_y == 0xd0)
                break;
            if (sprite_y >= 240)
                sprite_y -= 256;   /* wrap from top if y position is >= 240 */

            if (v->sprite_zoom_scale > 1 && v->sprite_count < v->max_sprite_zoom_vcount)
            {
                /* Divide before use the value for comparison, or else an
                 * off-by-one bug could occur */
                parse_line >>= 1;
                sprite_y >>= 1;
            }

            if ((parse_line >= sprite_y) && (parse_line < (sprite_y + v->sprite_height)))
            {
                if (v->sprite_count < max_sprites)
                {
                    const int sprite_x = vram_r(v, v->sprite_attribute_base + sprite_index + 1);
                    int sprite_tile_selected = vram_r(v, v->sprite_attribute_base + sprite_index + 2);
                    const uint8_t flags = vram_r(v, v->sprite_attribute_base + sprite_index + 3);
                    int sprite_line = parse_line - sprite_y;

                    if (v->sprite_height == 16)
                    {
                        sprite_tile_selected &= 0xfc;
                        if (sprite_line > 0x07)
                        {
                            sprite_tile_selected += 1;
                            sprite_line -= 8;
                        }
                    }

                    v->sprite_x[v->sprite_count] = sprite_x;
                    v->sprite_tile_selected[v->sprite_count] = sprite_tile_selected;
                    v->sprite_flags[v->sprite_count] = flags;
                    v->sprite_pattern_line[v->sprite_count] = (uint16_t)(((v->reg[0x06] & 0x07) << 11) + sprite_line);
                    v->sprite_count++;
                }
                else
                {
                    sprite_count_overflow(v, line, sprite_index);
                }
            }
        }
    }
    else
    {
        /* Regular sprites */
        const int max_sprites = 8;

        v->sprite_attribute_base = (uint16_t)((v->reg[0x05] << 7) & 0x3f00);
        const uint16_t sprite_attribute_extra_base =
            (uint16_t)(v->sprite_attribute_base + sprite_attribute_extra_offset_mode4(v, 0x80));

        for (int sprite_index = 0; sprite_index < 64; sprite_index++)
        {
            /* At this point the VDP vcount still doesn't refer the new line,
             * because the logical start point is slightly shifted on the
             * scanline */
            int parse_line = line - 1;
            int sprite_y = vram_r(v, v->sprite_attribute_base + sprite_index);

            if (v->y_pixels == 192 && sprite_y == 0xd0)
                break;
            if (sprite_y >= 240)
                sprite_y -= 256;   /* wrap from top if y position is >= 240 */

            if (v->sprite_zoom_scale > 1 && v->sprite_count < v->max_sprite_zoom_vcount)
            {
                parse_line >>= 1;
                sprite_y >>= 1;
            }

            if ((parse_line >= sprite_y) && (parse_line < (sprite_y + v->sprite_height)))
            {
                if (v->sprite_count < max_sprites)
                {
                    const int sprite_line = parse_line - sprite_y;
                    int sprite_x = vram_r(v, sprite_attribute_extra_base + (sprite_index << 1));
                    int sprite_tile_number = vram_r(v, sprite_attribute_extra_base + (sprite_index << 1) + 1);
                    int sprite_tile_selected = sprite_tile_select_mode4(v, (uint8_t)sprite_tile_number);

                    if (BIT(v->reg[0x00], 3))
                        sprite_x -= 0x08;              /* sprite shift */
                    if (BIT(v->reg[0x06], 2))
                        sprite_tile_selected += 256;   /* pattern table select */
                    if (v->sprite_height == 16)
                        sprite_tile_selected &= 0x01fe; /* force even index */
                    if (sprite_line > 0x07)
                        sprite_tile_selected += 1;

                    v->sprite_x[v->sprite_count] = sprite_x;
                    v->sprite_tile_selected[v->sprite_count] = sprite_tile_selected;
                    v->sprite_pattern_line[v->sprite_count] = (uint16_t)((sprite_line & 0x07) << 2);
                    v->sprite_count++;
                }
                else
                {
                    sprite_count_overflow(v, line, sprite_index);
                }
            }
        }
    }
}

static void sprite_collision(sms_vdp_t *v, int line, int sprite_col_x)
{
    (void)line;
    /* SMS/GG: collisions don't occur on column 0 if it is disabled. */
    if (BIT(v->reg[0x00], 5) && sprite_col_x < 8)
        return;
    v->pending_status |= STATUS_SPRCOL;
    v->pending_sprcol_x = SPRCOL_BASEHPOS + sprite_col_x;
}

static void draw_sprites_mode4(sms_vdp_t *v, int *line_buffer, int *priority_selected, int line)
{
    if (v->display_disabled || v->sprite_count == 0)
        return;

    bool sprite_col_occurred = false;
    int sprite_col_x = 255;
    uint8_t collision_buffer[256] = { 0 };

    /* Draw sprite layer */
    for (int sprite_buffer_index = v->sprite_count - 1; sprite_buffer_index >= 0; sprite_buffer_index--)
    {
        const int sprite_x = v->sprite_x[sprite_buffer_index];
        const int sprite_tile_selected = v->sprite_tile_selected[sprite_buffer_index];
        const uint16_t sprite_pattern_line = v->sprite_pattern_line[sprite_buffer_index];
        const int zoom_scale = sprite_buffer_index < v->max_sprite_zoom_hcount ? v->sprite_zoom_scale : 1;

        const uint8_t bit_plane_0 = vram_r(v, (sprite_tile_selected << 5) + sprite_pattern_line + 0x00);
        const uint8_t bit_plane_1 = vram_r(v, (sprite_tile_selected << 5) + sprite_pattern_line + 0x01);
        const uint8_t bit_plane_2 = vram_r(v, (sprite_tile_selected << 5) + sprite_pattern_line + 0x02);
        const uint8_t bit_plane_3 = vram_r(v, (sprite_tile_selected << 5) + sprite_pattern_line + 0x03);

        for (int pixel_x = 0; pixel_x < 8; pixel_x++)
        {
            const uint8_t pen_bit_0 = BIT(bit_plane_0, 7 - pixel_x);
            const uint8_t pen_bit_1 = BIT(bit_plane_1, 7 - pixel_x);
            const uint8_t pen_bit_2 = BIT(bit_plane_2, 7 - pixel_x);
            const uint8_t pen_bit_3 = BIT(bit_plane_3, 7 - pixel_x);
            const uint8_t pen_selected = (uint8_t)((pen_bit_3 << 3 | pen_bit_2 << 2 | pen_bit_1 << 1 | pen_bit_0) | 0x10);

            if (pen_selected == 0x10)   /* Transparent palette so skip draw */
                continue;

            int pixel_plot_x;
            if (zoom_scale > 1)
                pixel_plot_x = sprite_x + (pixel_x << 1);   /* sprite doubling */
            else
                pixel_plot_x = sprite_x + pixel_x;

            /* Draw at pixel position and, if zoomed, at pixel+1 */
            for (int zoom = 0; zoom < zoom_scale; zoom++)
            {
                pixel_plot_x += zoom;

                /* check to prevent going outside of active display area */
                if (pixel_plot_x < 0 || pixel_plot_x > 255)
                    continue;

                /* Check if the background has lower priority */
                if (!(priority_selected[pixel_plot_x] & PRIORITY_BIT))
                {
                    line_buffer[pixel_plot_x] = v->current_palette[pen_selected];
                    priority_selected[pixel_plot_x] = pen_selected;
                }
                else
                {
                    /* Check if the higher priority background has
                     * transparent pixel */
                    if (priority_selected[pixel_plot_x] == PRIORITY_BIT)
                    {
                        line_buffer[pixel_plot_x] = v->current_palette[pen_selected];
                        priority_selected[pixel_plot_x] = pen_selected;
                    }
                }
                if (collision_buffer[pixel_plot_x] != 1)
                {
                    collision_buffer[pixel_plot_x] = 1;
                }
                else
                {
                    sprite_col_occurred = true;
                    if (pixel_plot_x < sprite_col_x)
                        sprite_col_x = pixel_plot_x;
                }
            }
        }
        if (sprite_col_occurred)
            sprite_collision(v, line, sprite_col_x);
    }
}

static void draw_sprites_tms9918_mode(sms_vdp_t *v, int *line_buffer, int line)
{
    if (v->display_disabled || v->sprite_count == 0)
        return;

    bool sprite_col_occurred = false;
    int sprite_col_x = 255;
    uint8_t collision_buffer[256] = { 0 };

    /* Draw sprite layer */
    for (int sprite_buffer_index = v->sprite_count - 1; sprite_buffer_index >= 0; sprite_buffer_index--)
    {
        int sprite_x = v->sprite_x[sprite_buffer_index];
        int sprite_tile_selected = v->sprite_tile_selected[sprite_buffer_index];
        const uint16_t sprite_pattern_line = v->sprite_pattern_line[sprite_buffer_index];
        const uint8_t flags = v->sprite_flags[sprite_buffer_index];
        const int pen_selected = 0 /* m_palette_offset */ + (flags & 0x0f);
        const int zoom_scale = sprite_buffer_index < v->max_sprite_zoom_hcount ? v->sprite_zoom_scale : 1;

        if (BIT(flags, 7))
            sprite_x -= 32;

        for (int height = 8; height <= v->sprite_height; height += 8)
        {
            if (height == 16)
            {
                sprite_tile_selected += 2;
                sprite_x += (zoom_scale > 1 ? 16 : 8);
            }

            const uint8_t pattern = vram_r(v, sprite_pattern_line + sprite_tile_selected * 8);

            for (int pixel_x = 0; pixel_x < 8; pixel_x++)
            {
                if (pen_selected && BIT(pattern, 7 - pixel_x))
                {
                    int pixel_plot_x;
                    if (zoom_scale > 1)
                        pixel_plot_x = sprite_x + (pixel_x << 1);
                    else
                        pixel_plot_x = sprite_x + pixel_x;

                    /* Draw at pixel position and, if zoomed, at pixel+1 */
                    for (int zoom = 0; zoom < zoom_scale; zoom++)
                    {
                        pixel_plot_x += zoom;

                        /* check to prevent going outside of active display
                         * area */
                        if (pixel_plot_x < 0 || pixel_plot_x > 255)
                            continue;

                        line_buffer[pixel_plot_x] = v->current_palette[pen_selected];

                        if (collision_buffer[pixel_plot_x] != 1)
                        {
                            collision_buffer[pixel_plot_x] = 1;
                        }
                        else
                        {
                            sprite_col_occurred = true;
                            if (pixel_plot_x < sprite_col_x)
                                sprite_col_x = pixel_plot_x;
                        }
                    }
                }
            }
        }
        if (sprite_col_occurred)
            sprite_collision(v, line, sprite_col_x);
    }
}

/* Display mode 2 (Graphics II Mode) */
static void draw_scanline_mode2(sms_vdp_t *v, int *line_buffer, int line)
{
    const uint16_t name_base = (uint16_t)((v->reg[0x02] & 0x0f) << 10);
    const uint16_t color_base = (uint16_t)((v->reg[0x03] & 0x80) << 6);
    const int color_mask = ((v->reg[0x03] & 0x7f) << 3) | 0x07;
    const uint16_t pattern_base = (uint16_t)((v->reg[0x04] & 0x04) << 11);
    const int pattern_mask = ((v->reg[0x04] & 0x03) << 8) | 0xff;
    const int pattern_offset = (line & 0xc0) << 2;
    const uint16_t name_row_base = (uint16_t)(name_base + ((line >> 3) * 32));

    /* Draw background layer */
    for (int tile_column = 0; tile_column < 32; tile_column++)
    {
        const uint8_t name = vram_r(v, name_row_base + tile_column);
        const uint8_t pattern = vram_r(v, pattern_base + (((pattern_offset + name) & pattern_mask) * 8) + (line & 0x07));
        const uint8_t colors = vram_r(v, color_base + (((pattern_offset + name) & color_mask) * 8) + (line & 0x07));

        for (int pixel_x = 0; pixel_x < 8; pixel_x++)
        {
            const int pixel_plot_x = (tile_column << 3) + pixel_x;
            uint8_t pen_selected;

            if (BIT(pattern, 7 - pixel_x))
                pen_selected = colors >> 4;
            else
                pen_selected = colors & 0x0f;

            if (!pen_selected)
                pen_selected = (uint8_t)BACKDROP_COLOR(v);

            line_buffer[pixel_plot_x] = v->current_palette[pen_selected];
        }
    }
}

/* Display mode 0 (Graphics I Mode) */
static void draw_scanline_mode0(sms_vdp_t *v, int *line_buffer, int line)
{
    const uint16_t name_base = (uint16_t)((v->reg[0x02] & 0x0f) << 10);
    const uint16_t color_base = (uint16_t)((v->reg[0x03] << 6) & (SMS_VRAM_SIZE - 1));
    const uint16_t pattern_base = (uint16_t)((v->reg[0x04] << 11) & (SMS_VRAM_SIZE - 1));
    const uint16_t name_row_base = (uint16_t)(name_base + ((line >> 3) * 32));

    for (int tile_column = 0; tile_column < 32; tile_column++)
    {
        const uint8_t name = vram_r(v, name_row_base + tile_column);
        const uint8_t pattern = vram_r(v, pattern_base + (name << 3) + (line & 0x07));
        const uint8_t colors = vram_r(v, color_base + (name >> 3));

        for (int pixel_x = 0; pixel_x < 8; pixel_x++)
        {
            int pen_selected;
            const int pixel_plot_x = (tile_column << 3) + pixel_x;

            if (BIT(pattern, 7 - pixel_x))
                pen_selected = colors >> 4;
            else
                pen_selected = colors & 0x0f;

            if (!pen_selected)
                pen_selected = BACKDROP_COLOR(v);

            line_buffer[pixel_plot_x] = v->current_palette[pen_selected];
        }
    }
}

/* Display mode 1 (Text Mode) */
static void draw_scanline_mode1(sms_vdp_t *v, int *line_buffer, int line)
{
    const uint16_t name_base = (uint16_t)((v->reg[0x02] & 0x0f) << 10);
    const uint16_t pattern_base = (uint16_t)((v->reg[0x04] << 11) & (SMS_VRAM_SIZE - 1));
    const uint16_t name_row_base = (uint16_t)(name_base + ((line >> 3) * 32));

    for (int pixel_plot_x = 0; pixel_plot_x < 8; pixel_plot_x++)
        line_buffer[pixel_plot_x] = v->current_palette[BACKDROP_COLOR(v)];

    for (int tile_column = 0; tile_column < 40; tile_column++)
    {
        const uint8_t name = vram_r(v, name_row_base + tile_column);
        const uint8_t pattern = vram_r(v, pattern_base + (name << 3) + (line & 0x07));

        for (int pixel_x = 0; pixel_x < 6; pixel_x++)
        {
            const int pixel_plot_x = (tile_column * 6) + pixel_x + 8;
            int pen_selected;

            if (BIT(pattern, 7 - pixel_x))
                pen_selected = v->reg[0x07] >> 4;
            else
                pen_selected = v->reg[0x07] & 0x0f;

            if (!pen_selected)
                pen_selected = BACKDROP_COLOR(v);

            line_buffer[pixel_plot_x] = v->current_palette[pen_selected];
        }
    }

    for (int pixel_plot_x = 248; pixel_plot_x < 256; pixel_plot_x++)
        line_buffer[pixel_plot_x] = v->current_palette[BACKDROP_COLOR(v)];
}

/* Display mode 3 (Multicolor Mode) */
static void draw_scanline_mode3(sms_vdp_t *v, int *line_buffer, int line)
{
    const uint16_t name_base = (uint16_t)((v->reg[0x02] & 0x0f) << 10);
    const uint16_t pattern_base = (uint16_t)((v->reg[0x04] << 11) & (SMS_VRAM_SIZE - 1));
    const uint16_t name_row_base = (uint16_t)(name_base + ((line >> 3) * 32));

    for (int tile_column = 0; tile_column < 32; tile_column++)
    {
        const uint8_t name = vram_r(v, name_row_base + tile_column);
        const uint8_t pattern = vram_r(v, pattern_base + (name << 3) + (((line >> 3) & 3) << 1) + ((line & 4) >> 2));

        for (int pixel_x = 0; pixel_x < 8; pixel_x++)
        {
            const int pixel_plot_x = (tile_column << 3) + pixel_x;
            int pen_selected = (pattern >> (~pixel_x & 4)) & 0x0f;

            if (!pen_selected)
                pen_selected = BACKDROP_COLOR(v);

            line_buffer[pixel_plot_x] = v->current_palette[pen_selected];
        }
    }
}

static void blit_scanline(sms_vdp_t *v, const int *line_buffer, int pixel_offset_x, int row)
{
    uint32_t *p = fb_row(v, row);
    if (!p)
        return;
    /* raster column pixel_offset_x is fb column pixel_offset_x - VIS_X0 */
    p += pixel_offset_x - SMS_VIS_X0;

    int x = 0;
    if (v->vdp_mode == 4 && BIT(v->reg[0x00], 5))
    {
        /* Fill column 0 with overscan color from reg[0x07] */
        const uint32_t pen = v->pens[v->current_palette[BACKDROP_COLOR(v)]];
        do
            p[x] = pen;
        while (++x < 8);
    }
    do
        p[x] = v->pens[line_buffer[x]];
    while (++x < 256);
}

static void draw_scanline(sms_vdp_t *v, int param)
{
    const int pixel_offset_x = SMS_LBORDER_START + SMS_LBORDER_WIDTH;
    const int pixel_plot_y = param;
    const int line = v->vpos - param;
    int blitline_buffer[256];
    int priority_selected[256];

    update_palette(v);

    /* Sprite processing is restricted because collisions on top border of
     * extended resolution break the scoreboard of Fantasy Dizzy (SMS) on
     * smspal driver */
    if (line < v->frame_timing[ACTIVE_DISPLAY_V])
    {
        for (int i = 0; i < 256; i++)
            priority_selected[i] = 1;

        switch (v->vdp_mode)
        {
        case 0:
            if (line >= 0)
                draw_scanline_mode0(v, blitline_buffer, line);
            if (line >= 0 || (line >= -13 && v->y_pixels == 192))
                draw_sprites_tms9918_mode(v, blitline_buffer, line);
            break;
        case 1:
            if (line >= 0)
                draw_scanline_mode1(v, blitline_buffer, line);
            /* Text Mode, no sprite drawing. */
            break;
        case 2:
            if (line >= 0)
                draw_scanline_mode2(v, blitline_buffer, line);
            if (line >= 0 || (line >= -13 && v->y_pixels == 192))
                draw_sprites_tms9918_mode(v, blitline_buffer, line);
            break;
        case 3:
            if (line >= 0)
                draw_scanline_mode3(v, blitline_buffer, line);
            if (line >= 0 || (line >= -13 && v->y_pixels == 192))
                draw_sprites_tms9918_mode(v, blitline_buffer, line);
            break;
        case 4:
        default:
            if (line >= 0)
                draw_scanline_mode4(v, blitline_buffer, priority_selected, line);
            if (line >= 0 || (line >= -13 && v->y_pixels == 192))
                draw_sprites_mode4(v, blitline_buffer, priority_selected, line);
            break;
        }
    }

    /* Check if display is disabled or we're below/above active area */
    if (v->display_disabled || line < 0 || line >= v->frame_timing[ACTIVE_DISPLAY_V])
    {
        fill_row(v, pixel_plot_y + line, pixel_offset_x, pixel_offset_x + 255,
                 v->pens[v->current_palette[BACKDROP_COLOR(v)]]);
    }
    else
    {
        blit_scanline(v, blitline_buffer, pixel_offset_x, pixel_plot_y + line);
    }
}

/* ---------------------------------------------------------------------- */
/* the timers                                                             */
/* ---------------------------------------------------------------------- */

static void trigger_hint(sms_vdp_t *v)
{
    if (v->pending_hint || v->hint_occurred)
    {
        if (BIT(v->reg[0x00], 4))
            v->n_int_state = 0;
    }
}

static void trigger_vint(sms_vdp_t *v)
{
    if ((v->pending_status & STATUS_VINT) || (v->status & STATUS_VINT))
    {
        if (BIT(v->reg[0x01], 5))
            v->n_int_state = 0;
    }
}

static void update_nmi(sms_vdp_t *v)
{
    /* the /NMI line follows /NMI-IN once a frame; the CPU edge-detects it */
    v->n_nmi_state = v->n_nmi_in_state;
}

static void arm(sms_vdp_t *v, sms_vdp_event_t ev)
{
    v->ev_armed |= (uint8_t)(1u << ev);
}

/* process_line_timer, at hpos 14 of every line: the line counter, the
 * line's one-shot timers and the sprite selection for the next draw. */
static void process_line(sms_vdp_t *v)
{
    const int vpos = v->vpos;
    const uint8_t *ft = v->frame_timing;
    int vpos_limit = ft[VERTICAL_SYNC] + ft[TOP_BLANKING] + ft[TOP_BORDER]
                   + ft[ACTIVE_DISPLAY_V] + ft[BOTTOM_BORDER] + ft[BOTTOM_BLANKING];

    /* copy current values in case they are not changed until latch time */
    v->display_disabled = !BIT(v->reg[0x01], 6);
    v->reg8copy = v->reg[0x08];

    /* /CSYNC: /HSYNC and /VSYNC ANDed together; low for 28 pixels at the
     * start of every line except in the vertical sync area and the two
     * lines that follow it */
    if (vpos == 0 || vpos > (ft[VERTICAL_SYNC] + 1))
    {
        if (v->csync_cb)
            v->csync_cb(v->cb_user);
    }

    vpos_limit -= ft[BOTTOM_BLANKING];

    /* Check if we're below the bottom border */
    if (vpos >= vpos_limit)
    {
        v->line_counter = v->reg[0x0a];
        return;
    }

    vpos_limit -= ft[BOTTOM_BORDER];

    /* Check if we're in the bottom border area */
    if (vpos >= vpos_limit)
    {
        if (vpos == vpos_limit)
        {
            if (v->line_counter == 0x00)
            {
                v->line_counter = v->reg[0x0a];
                arm(v, SMS_EV_HINT);
                v->pending_hint = true;
            }
            else
            {
                v->line_counter--;
            }
        }
        else
        {
            v->line_counter = v->reg[0x0a];
        }

        /* vpos_limit + 1 because VINT fires at the end of the first logical
         * line of the bottom border. */
        if (vpos == vpos_limit + 1)
        {
            arm(v, SMS_EV_VINT);
            v->pending_status |= STATUS_VINT;
        }

        /* Draw borders */
        v->lborder_param = vpos;
        v->rborder_param = vpos;
        arm(v, SMS_EV_LBORDER);
        arm(v, SMS_EV_RBORDER);

        /* Draw middle of the border through the regular drawing function so
         * sprite collisions can occur on the border. */
        select_sprites(v, vpos - (vpos_limit - ft[ACTIVE_DISPLAY_V]));
        v->draw_param = vpos_limit - ft[ACTIVE_DISPLAY_V];
        arm(v, SMS_EV_DRAW);
        return;
    }

    vpos_limit -= ft[ACTIVE_DISPLAY_V];

    /* Check if we're in the active display area */
    if (vpos >= vpos_limit)
    {
        if (vpos == vpos_limit)
            v->reg9copy = v->reg[0x09];

        if (v->line_counter == 0x00)
        {
            v->line_counter = v->reg[0x0a];
            arm(v, SMS_EV_HINT);
            v->pending_hint = true;
        }
        else
        {
            v->line_counter--;
        }

        /* Draw borders */
        v->lborder_param = vpos;
        v->rborder_param = vpos;
        arm(v, SMS_EV_LBORDER);
        arm(v, SMS_EV_RBORDER);

        /* Draw active display */
        select_sprites(v, vpos - vpos_limit);
        v->draw_param = vpos_limit;
        arm(v, SMS_EV_DRAW);
        return;
    }

    vpos_limit -= ft[TOP_BORDER];

    /* Check if we're in the top border area */
    if (vpos >= vpos_limit)
    {
        v->line_counter = v->reg[0x0a];

        /* Check if we're on the last line of the top border */
        if (vpos == vpos_limit + ft[TOP_BORDER] - 1)
        {
            v->hcounter_latched = false;
            arm(v, SMS_EV_NMI);   /* vblank_end */
        }

        /* Draw borders */
        v->lborder_param = vpos;
        v->rborder_param = vpos;
        arm(v, SMS_EV_LBORDER);
        arm(v, SMS_EV_RBORDER);

        /* Draw middle of the border */
        select_sprites(v, vpos - (vpos_limit + ft[TOP_BORDER]));
        v->draw_param = vpos_limit + ft[TOP_BORDER];
        arm(v, SMS_EV_DRAW);
        return;
    }

    /* we're in the vertical sync or top blanking areas */
    v->line_counter = v->reg[0x0a];
}

static void run_event(sms_vdp_t *v, int ev)
{
    switch (ev)
    {
    case SMS_EV_PROCESS_LINE: process_line(v); break;
    case SMS_EV_VINT:         trigger_vint(v); break;
    case SMS_EV_HINT:         trigger_hint(v); break;
    case SMS_EV_NMI:          update_nmi(v); break;
    case SMS_EV_LBORDER:      draw_lborder(v, v->lborder_param); break;
    case SMS_EV_DRAW:         draw_scanline(v, v->draw_param); break;
    case SMS_EV_RBORDER:      draw_rborder(v, v->rborder_param); break;
    case SMS_EV_EOL:          check_pending_flags(v, SMS_VDP_WIDTH - 1); break;
    }
}

/* The next armed event at or after index `from` on the current line, or
 * SMS_EV_COUNT. */
static int next_armed(const sms_vdp_t *v, int from)
{
    for (int ev = from; ev < SMS_EV_COUNT; ev++)
    {
        const bool periodic = (EV_PERIODIC >> ev) & 1;
        if (periodic ? v->line_engine_on : ((v->ev_armed >> ev) & 1))
            return ev;
    }
    return SMS_EV_COUNT;
}

static void next_line(sms_vdp_t *v)
{
    v->line_mclk += SMS_MCLK_PER_LINE;
    v->vpos++;
    if (v->vpos >= v->lines)
        v->vpos = 0;
    if (v->vpos == 0)
        v->line_engine_on = true;   /* MAME's display timer starts at (0, 14) */
    v->ev_next = 0;
    v->ev_armed = 0;
    if (v->vpos == v->vblank_vpos)
    {
        /* VBLANK begins: MAME's screen update copies the frame here */
        v->frame_count++;
        v->frame_ready = true;
    }
}

void sms_vdp_run_until(sms_vdp_t *v, uint64_t cycle)
{
    for (;;)
    {
        const int ev = next_armed(v, v->ev_next);

        if (ev == SMS_EV_COUNT)
        {
            /* the line is done once its last pixel's time is visible */
            if (ev_cycle(v->line_mclk + SMS_MCLK_PER_LINE) > cycle)
                return;
            next_line(v);
            continue;
        }
        if (ev_cycle(v->line_mclk + (uint64_t)ev_hpos[ev] * SMS_MCLK_PER_PIXEL) > cycle)
            return;
        v->ev_next = ev + 1;
        run_event(v, ev);
    }
}

void sms_vdp_lines_at(const sms_vdp_t *v, uint64_t cycle, bool *irq, bool *nmi)
{
    bool int_asserted = (v->n_int_state == 0);
    bool nmi_asserted = (v->n_nmi_state == 0);

    /* Only the timer events can change the lines without a CPU access, and
     * within the few cycles this looks ahead only those on the engine's
     * current line can come due (the next line's VINT/HINT are armed by its
     * process_line, 10+ pixels before they fire). */
    for (int ev = next_armed(v, v->ev_next); ev < SMS_EV_COUNT; ev = next_armed(v, ev + 1))
    {
        if (ev_cycle(v->line_mclk + (uint64_t)ev_hpos[ev] * SMS_MCLK_PER_PIXEL) > cycle)
            break;
        if (ev == SMS_EV_VINT)
        {
            if (((v->pending_status & STATUS_VINT) || (v->status & STATUS_VINT)) && BIT(v->reg[0x01], 5))
                int_asserted = true;
        }
        else if (ev == SMS_EV_HINT)
        {
            if ((v->pending_hint || v->hint_occurred) && BIT(v->reg[0x00], 4))
                int_asserted = true;
        }
        else if (ev == SMS_EV_NMI)
        {
            nmi_asserted = (v->n_nmi_in_state == 0);
        }
    }
    *irq = int_asserted;
    *nmi = nmi_asserted;
}

/* ---------------------------------------------------------------------- */
/* device                                                                 */
/* ---------------------------------------------------------------------- */

void sms_vdp_init(sms_vdp_t *v, sms_vdp_kind_t kind, bool is_pal)
{
    memset(v, 0, sizeof *v);
    v->kind = kind;
    v->is_pal = is_pal;
    v->lines = is_pal ? SMS_VDP_HEIGHT_PAL : SMS_VDP_HEIGHT_NTSC;
    v->vis_y0 = is_pal ? SMS_VIS_Y0_PAL : SMS_VIS_Y0_NTSC;
    v->fb_height = is_pal ? SMS_FB_HEIGHT_PAL : SMS_FB_HEIGHT_NTSC;
    v->vblank_vpos = v->vis_y0 + v->fb_height;      /* visarea bottom + 1 */
    /* 315-5124: 4 sprites per line can be zoomed horizontally; 315-5246: 8 */
    v->max_sprite_zoom_hcount = (kind == SMS_VDP_5246) ? 8 : 4;
    v->max_sprite_zoom_vcount = 8;
    if (kind == SMS_VDP_5246)
        palette_5246(v->pens);
    else
        palette_5124(v->pens);

    /* the beam: power-on is the start of VBLANK */
    v->line_mclk = 0;
    v->vpos = v->vblank_vpos;
    v->ev_next = 0;
    v->ev_armed = 0;
    v->line_engine_on = false;

    sms_vdp_reset(v);
}

void sms_vdp_reset(sms_vdp_t *v)
{
    /* Most register are 0x00 at power-up */
    memset(v->reg, 0, sizeof v->reg);
    v->reg[0x02] = 0x0e;
    v->reg[0x0a] = 0xff;

    v->status = v->pending_status = (uint8_t)~(STATUS_VINT | STATUS_SPROVR | STATUS_SPRCOL);
    v->pending_sprcol_x = 0;
    v->pending_control_write = false;
    v->pending_hint = false;
    v->hint_occurred = false;
    v->reg8copy = 0;
    v->reg9copy = 0;
    v->addrmode = 0;
    v->addr = 0;
    v->display_disabled = false;
    v->cram_dirty = true;
    v->buffer = 0;
    v->n_int_state = 1;
    v->n_nmi_state = 1;
    v->n_nmi_in_state = 1;
    v->line_counter = 0;
    v->hcounter = 0;
    v->hcounter_latched = false;
    v->draw_time = DRAW_TIME_SMS;
    memset(v->current_palette, 0, sizeof v->current_palette);
    set_display_settings(v);

    /* Clear RAM */
    memset(v->CRAM, 0, sizeof v->CRAM);
}
