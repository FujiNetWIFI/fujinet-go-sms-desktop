/*
 * The Win32 Controllers window: the Master System drawn as a player sees it
 * -- the console's black top strip with its PAUSE button (and RESET, or RAPID
 * on the Japanese console, where the model has one) over the two black Control
 * Pads, Player 1 on the left -- from the session's layout
 * (smssession_controls_layout), so every frontend shows the same console, then
 * the gamepads with their assignments and Map mode.
 *
 * Each pad is a D-pad (a round dish with a cross in it) and the square
 * buttons 1 and 2. Mouse: a press on a cross arm holds that direction; a press
 * on the rest of the dish follows the pointer and holds one or two directions
 * (smssession_dpad_from_vector); a press on 1, 2 or a console button holds it.
 * Presses are the raw button-down and button-up, never a click: the console
 * samples its controls once a frame, so a press present only for the instant of
 * a click falls between frames. The mouse is captured on press; letting go
 * anywhere, dragging off the part, losing the capture, hiding or deactivating
 * the window releases whatever the pointer holds (smssession_pointer_set(port,
 * 0), smssession_press(target, 0)).
 *
 * What the console sees held -- keyboard, gamepads and this window together
 * (smssession_buttons_held, smssession_switch_held) -- is polled every 40 ms
 * and painted in the accent, so the window doubles as a gamepad tester.
 *
 * FIXED SIZE: laid out once for the console, the three session actions, three
 * gamepad rows and the Map row, in a caption-only tool window with no thick
 * frame and no maximize box, so neither Map mode, a long gamepad name nor a
 * change of console changes its size.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control to pick the target
 * (the three session actions above the gamepads too), then press a host key OR
 * a gamepad button (Esc cancels; Clear takes the picked target's key and
 * button away). While armed the controls show what drives them, typing does
 * not reach the console, and rebinding steals the key from whatever held it.
 * "Show bindings" shows the same legends outside Map mode.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at
 * the window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include "controllers_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../help/help_window.h"
#include "../key_forward.h"
#include "../help/help_compat.h"

#define PANEL_CLASS   "FujiNetGoSMSControllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; scl() scales them to the window's. */
#define UNIT96      38      /* pixels per layout unit (a D-pad arm's width) */
#define MARGIN      12
#define CASE_PAD    16
#define HEAD_H      22
#define ROW_H       26      /* one gamepad's row */
#define PAD_ROWS     3      /* gamepad rows the window has room for */
#define SEG_W       64      /* Automatic / Player 1 / Player 2 */
#define BTN_H       28
#define MAP_W       60
#define DEFAULTS_W  76
#define CLEAR_W     60
#define SHOW_W     124
#define ACT_W      132
#define HELP_W      64
#define DISC_RADIUS 1.55    /* units: the D-pad dish (the core's DISC_R) */

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

#define EM_DASH "\xe2\x80\x94"

/* The strip's buttons: the session's actions above the gamepads, then the Map
 * row below them. */
enum { SB_ACT_CONFIG, SB_ACT_SOFT, SB_ACT_STOP, SB_MAP, SB_DEFAULTS, SB_CLEAR, SB_SHOW, SB_HELP, SB_COUNT };

typedef struct {
    unsigned generation;
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

static HWND g_panel, g_parent;
static smssession *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static WPARAM g_swallow_vkey;       /* the key Map took: its repeats and release too */
static char g_hint[300];

/* The console's layout (units), and where it sits in the client. */
static const sms_control *g_layout;
static int g_nlayout;
static float g_lay_wid, g_lay_hgt;
static int g_built_console = -1;
static double g_unit, g_orgx, g_orgy;
static struct { double ctrx, ctry; } g_disc[SMS_PLAYERS];      /* per port */

/* What the pointer holds. */
static int g_mouse_part = -1;       /* a pressed arm, button or console button (layout index) */
static int g_mouse_port = -1;       /* a dish drag */
static int g_mouse_action = -1;     /* a pressed session action button (SB_*) */
static int g_hover = -1;            /* the layout index under the pointer */
static int g_tracking;
static unsigned g_pointer[SMS_PLAYERS];     /* the bits given to smssession_pointer_set */

/* What was last painted as held. */
static unsigned g_shown_held[SMS_PLAYERS];
static int g_shown_sw[SMS_SW_COUNT];

static pads_state g_pads_shown;
static int g_total_w, g_total_h;
static RECT g_case_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SB_COUNT], g_hint_rc;

static HBRUSH g_accent_brush;
static HFONT g_font, g_bold, g_num, g_leg, g_tiny, g_word, g_mark, g_capt;

static void refresh_held(void);
static void rebuild(void);

static int scl(int val) { return sms_scale(val, g_dpi); }

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *slot[] = { &g_font, &g_bold, &g_num, &g_leg, &g_tiny, &g_word, &g_mark, &g_capt };
    size_t idx;
    for (idx = 0; idx < sizeof slot / sizeof slot[0]; idx++)
        if (*slot[idx]) { DeleteObject(*slot[idx]); *slot[idx] = NULL; }
}

static void make_fonts(void)
{
    free_fonts();
    g_font = sms_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = sms_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_num = sms_make_font(g_dpi, 20, FW_BOLD, NULL);
    g_leg = sms_make_font(g_dpi, 12, FW_BOLD, NULL);
    g_tiny = sms_make_font(g_dpi, 10, FW_NORMAL, NULL);
    g_word = sms_make_font(g_dpi, 17, FW_BOLD, NULL);
    g_mark = sms_make_font(g_dpi, SMS_PAD_MARK_PX, FW_BOLD, NULL);
    g_capt = sms_make_font(g_dpi, 12, FW_BOLD, NULL);
}

/* ---- colours: the real hardware's black plastic, the accent for held ---------- */

static COLORREF rgb_of(unsigned val)
{
    return RGB((val >> 16) & 0xff, (val >> 8) & 0xff, val & 0xff);
}

#define C_ACCENT    rgb_of(SMSSESSION_ACCENT_RGB)
#define C_ACCENT_D  rgb_of(SMSSESSION_ACCENT_DARK_RGB)
#define C_WHITE     RGB(0xff, 0xff, 0xff)
#define C_CASE_TOP  RGB(0x23, 0x23, 0x26)
#define C_CASE_BOT  RGB(0x13, 0x13, 0x15)
#define C_CASE_EDGE RGB(0x0a, 0x0a, 0x0b)
#define C_CASE_HI   RGB(0x3c, 0x3c, 0x40)
#define C_STRIP     RGB(0x0b, 0x0b, 0x0c)
#define C_STRIP_E   RGB(0x2a, 0x2a, 0x2d)
#define C_CAP_TOP   RGB(0x3a, 0x3a, 0x3e)
#define C_CAP_BOT   RGB(0x1c, 0x1c, 0x1f)
#define C_CAP_EDGE  RGB(0x05, 0x05, 0x06)
#define C_CAP_SHADE RGB(0x07, 0x07, 0x08)
#define C_RECESS    RGB(0x1b, 0x1b, 0x1d)
#define C_CAPTION   RGB(0x9a, 0x9a, 0xa0)
#define C_PAD_TOP   RGB(0x30, 0x30, 0x33)
#define C_PAD_BOT   RGB(0x17, 0x17, 0x19)
#define C_PAD_HI    RGB(0x50, 0x50, 0x55)
#define C_PAD_EDGE  RGB(0x0a, 0x0a, 0x0b)
#define C_PAD_MARK  RGB(0x8e, 0x8e, 0x94)
#define C_DISH      RGB(0x0c, 0x0c, 0x0d)
#define C_DISH_E    RGB(0x2b, 0x2b, 0x2e)
#define C_ARM_TOP   RGB(0x2a, 0x2a, 0x2d)
#define C_ARM_BOT   RGB(0x15, 0x15, 0x17)
#define C_ARM_EDGE  RGB(0x05, 0x05, 0x05)
#define C_ARM_HI    RGB(0x4a, 0x4a, 0x4f)
#define C_ARM_TRI   RGB(0x77, 0x77, 0x80)
#define C_BTN_TOP   RGB(0x2e, 0x2e, 0x32)
#define C_BTN_BOT   RGB(0x15, 0x15, 0x17)
#define C_NUMERAL   RGB(0xe6, 0xe6, 0xe6)
#define C_LIGHT     RGB(0xed, 0xed, 0xed)
#define C_DIM_BG    RGB(0x13, 0x13, 0x15)

/* The control being drawn: unpicked controls are dimmed to 40 % in Map mode
 * (the rest of the pad shows through), and the one under the pointer is
 * lightened by about 12 %. */
static int g_dim, g_hot;

static COLORREF tone(COLORREF col)
{
    int red = GetRValue(col), grn = GetGValue(col), blu = GetBValue(col);
    if (g_hot) {
        red += (255 - red) * 12 / 100;
        grn += (255 - grn) * 12 / 100;
        blu += (255 - blu) * 12 / 100;
    }
    if (g_dim) {
        red = (red * 40 + GetRValue(C_DIM_BG) * 60) / 100;
        grn = (grn * 40 + GetGValue(C_DIM_BG) * 60) / 100;
        blu = (blu * 40 + GetBValue(C_DIM_BG) * 60) / 100;
    }
    return RGB(red, grn, blu);
}

static COLORREF mix(COLORREF from, COLORREF target, double frac)
{
    return RGB((int)(GetRValue(from) + (GetRValue(target) - GetRValue(from)) * frac + 0.5),
               (int)(GetGValue(from) + (GetGValue(target) - GetGValue(from)) * frac + 0.5),
               (int)(GetBValue(from) + (GetBValue(target) - GetBValue(from)) * frac + 0.5));
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (legends, key names, gamepad names) through DrawTextW. */
static int text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

static void text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT box = *rect;
    text_out(hdc, utf8, &box, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* A legend: the key over the gamepad button, each on one line and ellipsized,
 * centred both ways. */
static void text_legend(HDC hdc, const char *utf8, const RECT *rect)
{
    char first[128], *newline;
    const int lineh = scl(11);
    int lines = 1, top;
    RECT row;
    snprintf(first, sizeof first, "%s", utf8);
    newline = strchr(first, '\n');
    if (newline) { *newline = '\0'; lines = 2; }
    top = rect->top + ((rect->bottom - rect->top) - lines * lineh) / 2;
    SetRect(&row, rect->left, top, rect->right, top + lineh);
    text_out(hdc, first, &row, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (newline) {
        OffsetRect(&row, 0, lineh);
        text_out(hdc, newline + 1, &row, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

/* ---- art: GDI at a supersampled scale ------------------------------------------- */

/* One art pass draws the client rectangle with its top-left at (g_art_left,
 * g_art_top) into a bitmap g_art_scale times the size. */
static int g_art_scale = 1, g_art_left, g_art_top;

static int axpos(double val) { return (int)floor((val - g_art_left) * g_art_scale + 0.5); }
static int aypos(double val) { return (int)floor((val - g_art_top) * g_art_scale + 0.5); }
static int alen(double val) { return (int)floor(val * g_art_scale + 0.5); }

#define NO_FILL ((COLORREF)0xFFFFFFFF)

static void poly(HDC hdc, const POINT *pnt, int cnt, COLORREF fill, COLORREF line, double linew)
{
    HBRUSH brush = CreateSolidBrush(tone(fill));
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? alen(linew) : 0, tone(line));
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Polygon(hdc, pnt, cnt);
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void ellipse(HDC hdc, double ctrx, double ctry, double radx, double rady, COLORREF fill, COLORREF line,
                    double linew)
{
    HBRUSH brush = CreateSolidBrush(tone(fill));
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? alen(linew) : 0, tone(line));
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, axpos(ctrx - radx), aypos(ctry - rady), axpos(ctrx + radx), aypos(ctry + rady));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    DeleteObject(brush);
}

/* A rounded rectangle: fill NO_FILL leaves the inside alone (an outline). */
static void rrect(HDC hdc, double xpos, double ypos, double wid, double hgt, double rad, COLORREF fill,
                  COLORREF line, double linew)
{
    HBRUSH brush = CreateSolidBrush(fill == NO_FILL ? 0 : tone(fill));
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? alen(linew) : 0, tone(line));
    HGDIOBJ oldb = SelectObject(hdc, fill == NO_FILL ? GetStockObject(NULL_BRUSH) : (HGDIOBJ)brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, axpos(xpos), aypos(ypos), axpos(xpos + wid), aypos(ypos + hgt), alen(rad * 2), alen(rad * 2));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    DeleteObject(brush);
}

/* A rounded rectangle shaded top to bottom: the clip is the rounded shape and
 * the fill a stack of bands, each its own mix of the two colours. */
static void grad_rrect(HDC hdc, double xpos, double ypos, double wid, double hgt, double rad, COLORREF top,
                       COLORREF bot, COLORREF line, double linew)
{
    const int xlo = axpos(xpos), ylo = aypos(ypos), xhi = axpos(xpos + wid) + 1, yhi = aypos(ypos + hgt) + 1;
    int bands = yhi - ylo, band;
    HRGN region;
    if (bands > 48) bands = 48;
    if (bands < 1) bands = 1;
    region = CreateRoundRectRgn(xlo, ylo, xhi, yhi, alen(rad * 2), alen(rad * 2));
    SelectClipRgn(hdc, region);
    for (band = 0; band < bands; band++) {
        RECT strip;
        HBRUSH brush = CreateSolidBrush(tone(mix(top, bot, (band + 0.5) / bands)));
        SetRect(&strip, xlo, ylo + (yhi - ylo) * band / bands, xhi, ylo + (yhi - ylo) * (band + 1) / bands);
        FillRect(hdc, &strip, brush);
        DeleteObject(brush);
    }
    SelectClipRgn(hdc, NULL);
    DeleteObject(region);
    if (linew > 0) rrect(hdc, xpos, ypos, wid, hgt, rad, NO_FILL, line, linew);
}

static void stroke(HDC hdc, const POINT *pnt, int cnt, COLORREF col, double linew)
{
    HPEN pen = CreatePen(PS_SOLID, alen(linew), tone(col));
    HGDIOBJ oldp = SelectObject(hdc, pen);
    Polyline(hdc, pnt, cnt);
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

/* The pixel rectangle of a layout part. */
static void part_box(const sms_control *ctl, double *xpos, double *ypos, double *wid, double *hgt)
{
    *xpos = g_orgx + (double)ctl->left * g_unit;
    *ypos = g_orgy + (double)ctl->top * g_unit;
    *wid = (double)ctl->width * g_unit;
    *hgt = (double)ctl->height * g_unit;
}

static void part_rect(const sms_control *ctl, RECT *rect)
{
    double xpos, ypos, wid, hgt;
    part_box(ctl, &xpos, &ypos, &wid, &hgt);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + wid), (int)ceil(ypos + hgt));
}

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

/* Whether a part is shown held: what the console sees, in the accent. In Map
 * mode nothing is held-lit; the armed target is outlined instead. */
static int part_lit(const sms_control *ctl)
{
    if (g_map_state != -2) return 0;
    switch (ctl->kind) {
    case SMS_CTL_SWITCH:
        return ctl->target >= SMS_TARGET_SWITCH(0) && ctl->target < SMS_TARGET_SWITCH(SMS_SW_COUNT) &&
               g_shown_sw[ctl->target - SMS_TARGET_SWITCH(0)];
    case SMS_CTL_DIRECTION:
    case SMS_CTL_BUTTON:
        return ctl->port >= 0 && (g_shown_held[ctl->port] & ctl->act_mask) != 0;
    case SMS_CTL_DPAD:
        return ctl->port >= 0 && (g_shown_held[ctl->port] & 0xFu) != 0;
    default:
        return 0;
    }
}

static int part_picked(const sms_control *ctl)
{
    return g_map_state >= 0 && ctl->target >= 0 && ctl->target == g_map_state;
}

/* Sets the dim and hover state for drawing the part at `idx`. */
static void part_state(int idx)
{
    const sms_control *ctl = &g_layout[idx];
    g_hot = idx == g_hover && g_mouse_part < 0 && g_mouse_port < 0;
    g_dim = 0;
    if (g_map_state < 0) return;
    if (ctl->kind == SMS_CTL_DPAD)
        g_dim = !(g_map_state < SMS_TARGET_SWITCH(0) && g_map_state / SMS_ACT_PER_PORT == ctl->port);
    else if (ctl->target >= 0)
        g_dim = ctl->target != g_map_state;
}

static void part_state_clear(void)
{
    g_dim = g_hot = 0;
}

/* The small triangle on a cross arm, pointing out. */
static void arm_glyph(HDC hdc, const sms_control *ctl, double xpos, double ypos, double wid, double hgt,
                      COLORREF col)
{
    double dirx = 0, diry = 0;
    const double unit = g_unit;
    double glyphx, glyphy;
    POINT pnt[3];
    if (ctl->act_mask == (1u << SMS_ACT_UP)) diry = -1;
    else if (ctl->act_mask == (1u << SMS_ACT_DOWN)) diry = 1;
    else if (ctl->act_mask == (1u << SMS_ACT_LEFT)) dirx = -1;
    else dirx = 1;
    glyphx = xpos + wid / 2 + dirx * unit * 0.12;
    glyphy = ypos + hgt / 2 + diry * unit * 0.12;
    pnt[0].x = axpos(glyphx + dirx * unit * 0.2);
    pnt[0].y = aypos(glyphy + diry * unit * 0.2);
    pnt[1].x = axpos(glyphx - dirx * unit * 0.14 - diry * unit * 0.2);
    pnt[1].y = aypos(glyphy - diry * unit * 0.14 + dirx * unit * 0.2);
    pnt[2].x = axpos(glyphx - dirx * unit * 0.14 + diry * unit * 0.2);
    pnt[2].y = aypos(glyphy - diry * unit * 0.14 - dirx * unit * 0.2);
    poly(hdc, pnt, 3, col, col, 0.5);
}

/* The dish and the centre of the cross; the arms come after, each its own
 * layout entry. */
static void art_dpad(HDC hdc, const sms_control *ctl)
{
    double xpos, ypos, wid, hgt;
    const int lit = part_lit(ctl);
    part_box(ctl, &xpos, &ypos, &wid, &hgt);
    ellipse(hdc, xpos + wid / 2, ypos + hgt / 2, wid / 2, hgt / 2, C_DISH, lit ? C_ACCENT : C_DISH_E, 1.5);
    grad_rrect(hdc, xpos + wid / 2 - g_unit * 0.5, ypos + hgt / 2 - g_unit * 0.5, g_unit, g_unit, g_unit * 0.05,
               C_ARM_TOP, C_ARM_BOT, C_ARM_EDGE, 1.0);
}

static void art_arm(HDC hdc, const sms_control *ctl)
{
    double xpos, ypos, wid, hgt;
    const int lit = part_lit(ctl);
    part_box(ctl, &xpos, &ypos, &wid, &hgt);
    if (lit) {
        grad_rrect(hdc, xpos, ypos, wid, hgt, g_unit * 0.12, C_ACCENT, C_ACCENT, C_WHITE, 2.0);
    } else {
        POINT edge[2];
        grad_rrect(hdc, xpos, ypos, wid, hgt, g_unit * 0.12, C_ARM_TOP, C_ARM_BOT, C_ARM_EDGE, 1.0);
        edge[0].x = axpos(xpos + g_unit * 0.1);
        edge[0].y = aypos(ypos + g_unit * 0.05);
        edge[1].x = axpos(xpos + wid - g_unit * 0.1);
        edge[1].y = edge[0].y;
        stroke(hdc, edge, 2, C_ARM_HI, 1.0);
    }
    if (!binding_mode()) arm_glyph(hdc, ctl, xpos, ypos, wid, hgt, lit ? C_ACCENT_D : C_ARM_TRI);
    if (part_picked(ctl)) rrect(hdc, xpos, ypos, wid, hgt, g_unit * 0.12, NO_FILL, C_ACCENT, 3.0);
    /* the cross's centre: the dimple, with the thumb-stick screw hole in it */
    if (ctl->act_mask == (1u << SMS_ACT_RIGHT)) {
        const double ctrx = xpos - wid / 2, ctry = ypos + hgt / 2;
        g_hot = 0;                  /* it is the cross's, not this arm's */
        ellipse(hdc, ctrx, ctry, g_unit * 0.34, g_unit * 0.34, C_DISH, C_ARM_EDGE, 1.0);
        ellipse(hdc, ctrx, ctry, g_unit * 0.12, g_unit * 0.12, C_ARM_EDGE, C_ARM_EDGE, 0);
    }
}

static void art_button(HDC hdc, const sms_control *ctl)
{
    double xpos, ypos, wid, hgt;
    const int lit = part_lit(ctl);
    const double drop = lit ? scl(2) : 0;
    part_box(ctl, &xpos, &ypos, &wid, &hgt);
    if (!lit) rrect(hdc, xpos, ypos + scl(2), wid, hgt, g_unit * 0.28, C_CAP_SHADE, C_CAP_SHADE, 0);
    if (lit)
        grad_rrect(hdc, xpos, ypos + drop, wid, hgt, g_unit * 0.28, C_ACCENT, C_ACCENT, C_WHITE, 2.0);
    else
        grad_rrect(hdc, xpos, ypos, wid, hgt, g_unit * 0.28, C_BTN_TOP, C_BTN_BOT, C_ARM_EDGE, 1.0);
    if (part_picked(ctl)) rrect(hdc, xpos, ypos, wid, hgt, g_unit * 0.28, NO_FILL, C_ACCENT, 3.0);
}

/* PAUSE, RESET / RAPID: a rectangular cap with a darker lower side. */
static void art_cap(HDC hdc, const sms_control *ctl)
{
    double xpos, ypos, wid, hgt;
    const int lit = part_lit(ctl);
    const double drop = lit ? scl(2) : 0;
    part_box(ctl, &xpos, &ypos, &wid, &hgt);
    if (!lit) rrect(hdc, xpos, ypos + scl(3), wid, hgt, g_unit * 0.12, C_CAP_SHADE, C_CAP_SHADE, 0);
    if (lit)
        grad_rrect(hdc, xpos, ypos + drop, wid, hgt, g_unit * 0.12, C_ACCENT, C_ACCENT, C_WHITE, 2.0);
    else
        grad_rrect(hdc, xpos, ypos, wid, hgt, g_unit * 0.12, C_CAP_TOP, C_CAP_BOT, C_CAP_EDGE, 1.0);
    if (part_picked(ctl)) rrect(hdc, xpos, ypos, wid, hgt, g_unit * 0.12, NO_FILL, C_ACCENT, 3.0);
}

static void art_console_strip(HDC hdc, const sms_control *ctl)
{
    double xpos, ypos, wid, hgt;
    part_box(ctl, &xpos, &ypos, &wid, &hgt);
    grad_rrect(hdc, xpos, ypos, wid, hgt, g_unit * 0.15, C_STRIP, C_STRIP, C_STRIP_E, 1.0);
    /* the accent stripe along the bottom edge */
    {
        RECT stripe;
        HBRUSH brush = CreateSolidBrush(C_ACCENT);
        SetRect(&stripe, axpos(xpos + 1), aypos(ypos + hgt - scl(3) - 1), axpos(xpos + wid - 1), aypos(ypos + hgt - 1));
        FillRect(hdc, &stripe, brush);
        DeleteObject(brush);
    }
}

/* The recess a pad sits in, from its caption to the bottom of the pad. */
static void art_recess(HDC hdc, const sms_control *ctl)
{
    double xpos, ypos, wid, hgt;
    part_box(ctl, &xpos, &ypos, &wid, &hgt);
    rrect(hdc, xpos - g_unit * 0.2, ypos - g_unit * 0.1, wid + g_unit * 0.4,
          g_orgy + (double)g_lay_hgt * g_unit - ypos + g_unit * 0.2, scl(10), C_RECESS, C_CASE_EDGE, 1.0);
}

static void art_pad_body(HDC hdc, const sms_control *ctl)
{
    double xpos, ypos, wid, hgt;
    POINT edge[2];
    part_box(ctl, &xpos, &ypos, &wid, &hgt);
    grad_rrect(hdc, xpos, ypos, wid, hgt, g_unit * 0.7, C_PAD_TOP, C_PAD_BOT, C_PAD_EDGE, 1.0);
    edge[0].x = axpos(xpos + g_unit * 0.7);
    edge[0].y = aypos(ypos + scl(2));
    edge[1].x = axpos(xpos + wid - g_unit * 0.7);
    edge[1].y = edge[0].y;
    stroke(hdc, edge, 2, C_PAD_HI, 1.5);
}

/* The case with the console and the pads in it, in paint order. */
static void art_case(HDC hdc)
{
    int idx;
    POINT edge[2];
    grad_rrect(hdc, g_case_rc.left, g_case_rc.top, g_case_rc.right - g_case_rc.left,
               g_case_rc.bottom - g_case_rc.top, scl(14), C_CASE_TOP, C_CASE_BOT, C_CASE_EDGE, 1.0);
    edge[0].x = axpos(g_case_rc.left + scl(14));
    edge[0].y = aypos(g_case_rc.top + 1);
    edge[1].x = axpos(g_case_rc.right - scl(14));
    edge[1].y = edge[0].y;
    stroke(hdc, edge, 2, C_CASE_HI, 1.5);

    for (idx = 0; idx < g_nlayout; idx++) {
        const sms_control *ctl = &g_layout[idx];
        part_state(idx);
        switch (ctl->kind) {
        case SMS_CTL_CONSOLE: part_state_clear(); art_console_strip(hdc, ctl); break;
        case SMS_CTL_SWITCH: art_cap(hdc, ctl); break;
        case SMS_CTL_CAPTION: part_state_clear(); art_recess(hdc, ctl); break;
        case SMS_CTL_PAD: part_state_clear(); art_pad_body(hdc, ctl); break;
        case SMS_CTL_DPAD: art_dpad(hdc, ctl); break;
        case SMS_CTL_DIRECTION: art_arm(hdc, ctl); break;
        case SMS_CTL_BUTTON: art_button(hdc, ctl); break;
        default: break;
        }
    }
    part_state_clear();
}

/* In Map mode and with Show bindings a control shows what drives it: the key,
 * then the pad button. A cross arm has room for little: its key is an arrow
 * glyph, and the pad line is left off when it only repeats the direction
 * ("D-pad Left" on a Left arm); a pad button that is not the D-pad stays. */
static void binding_label(int target, char *out, int outsz)
{
    const sms_binding bind = smssession_binding_get(g_session, target);
    const int arm = target < SMS_TARGET_SWITCH(0) && target % SMS_ACT_PER_PORT <= SMS_ACT_RIGHT;
    char key[32];
    const char *pad = NULL;
    key[0] = '\0';
    smssession_keysym_name(bind.keysym, key, sizeof key);
    if (arm) {
        switch (bind.keysym) {
        case SMS_KEYSYM_UP:    snprintf(key, sizeof key, "\xe2\x86\x91"); break;
        case SMS_KEYSYM_DOWN:  snprintf(key, sizeof key, "\xe2\x86\x93"); break;
        case SMS_KEYSYM_LEFT:  snprintf(key, sizeof key, "\xe2\x86\x90"); break;
        case SMS_KEYSYM_RIGHT: snprintf(key, sizeof key, "\xe2\x86\x92"); break;
        default: break;
        }
    }
    if (bind.button != SMS_PAD_BTN_NONE) {
        pad = sms_pad_button_name(bind.button);
        if (arm && strncmp(pad, "D-pad ", 6) == 0) pad = NULL;     /* repeats the arm */
    }
    if (pad)
        snprintf(out, (size_t)outsz, "%s\n%s", key[0] ? key : EM_DASH, pad);
    else
        snprintf(out, (size_t)outsz, "%s", key[0] ? key : EM_DASH);
}

/* The wordmark on the strip: SEGA in white, spaced, then the model in the
 * accent. */
static void text_wordmark(HDC hdc, const sms_control *ctl)
{
    RECT strip, box;
    const char *legend = ctl->legend ? ctl->legend : "";
    const char *space = strchr(legend, ' ');
    char brand[24];
    wchar_t wide[24];
    SIZE ext;
    int left;
    part_rect(ctl, &strip);
    snprintf(brand, sizeof brand, "%.*s", space ? (int)(space - legend) : (int)strlen(legend), legend);
    left = strip.left + (int)(g_unit * 0.45);
    SetRect(&box, left, strip.top, strip.right, strip.bottom - scl(3));
    SelectObject(hdc, g_word);
    SetTextCharacterExtra(hdc, scl(2));
    SetTextColor(hdc, C_LIGHT);
    text_line(hdc, brand, &box, DT_LEFT);
    if (space && MultiByteToWideChar(CP_UTF8, 0, brand, -1, wide, 24) > 0 &&
        GetTextExtentPoint32W(hdc, wide, (int)wcslen(wide), &ext)) {
        box.left = left + ext.cx + scl(10);
        SetTextColor(hdc, C_ACCENT);
        text_line(hdc, space + 1, &box, DT_LEFT);
    }
    SetTextCharacterExtra(hdc, 0);
}

/* The labels and numerals over the art, at the window's own size. */
static void text_parts(HDC hdc)
{
    int idx;
    SetBkMode(hdc, TRANSPARENT);
    for (idx = 0; idx < g_nlayout; idx++) {
        const sms_control *ctl = &g_layout[idx];
        RECT rect;
        const int lit = part_lit(ctl);
        part_rect(ctl, &rect);
        part_state(idx);
        switch (ctl->kind) {
        case SMS_CTL_CONSOLE:
            part_state_clear();
            text_wordmark(hdc, ctl);
            break;
        case SMS_CTL_CAPTION:
            part_state_clear();
            SelectObject(hdc, g_capt);
            SetTextColor(hdc, C_CAPTION);
            text_line(hdc, ctl->label, &rect, DT_LEFT);
            break;
        case SMS_CTL_PAD: {
            RECT mark = rect;
            part_state_clear();
            mark.left += (int)floor(SMS_PAD_MARK_LEFT * g_unit);
            mark.right = rect.left + (int)ceil((SMS_PAD_MARK_LEFT + SMS_PAD_MARK_WIDTH) * g_unit);
            mark.top += (int)floor(SMS_PAD_MARK_TOP * g_unit);
            mark.bottom = rect.top + (int)ceil((SMS_PAD_MARK_TOP + SMS_PAD_MARK_HEIGHT) * g_unit);
            SelectObject(hdc, g_mark);
            SetTextColor(hdc, C_PAD_MARK);
            SetTextCharacterExtra(hdc, scl(SMS_PAD_MARK_SPACING));
            text_line(hdc, "SEGA", &mark, DT_CENTER);
            SetTextCharacterExtra(hdc, 0);
            break;
        }
        case SMS_CTL_SWITCH:
            if (lit) OffsetRect(&rect, 0, scl(2));
            if (binding_mode() && ctl->target >= 0) {
                char legend[128];
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, tone(lit ? C_WHITE : C_LIGHT));
                text_legend(hdc, legend, &rect);
            } else {
                SelectObject(hdc, g_leg);
                SetTextColor(hdc, tone(lit ? C_WHITE : C_LIGHT));
                text_line(hdc, ctl->label, &rect, DT_CENTER);
            }
            break;
        case SMS_CTL_DIRECTION:
            if (binding_mode() && ctl->target >= 0) {
                char legend[128];
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, tone(lit ? C_WHITE : C_LIGHT));
                InflateRect(&rect, (int)(g_unit * 0.08), 0);
                text_legend(hdc, legend, &rect);
            }
            break;
        case SMS_CTL_BUTTON: {
            RECT face = rect;
            if (lit) OffsetRect(&face, 0, scl(2));
            if (binding_mode() && ctl->target >= 0) {
                char legend[128];
                RECT numeral = face, body = face;
                binding_label(ctl->target, legend, sizeof legend);
                numeral.bottom = face.top + scl(13);
                body.top += scl(5);
                SelectObject(hdc, g_mark);
                SetTextColor(hdc, tone(lit ? C_WHITE : C_CAPTION));
                text_line(hdc, ctl->label, &numeral, DT_CENTER);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, tone(lit ? C_WHITE : C_LIGHT));
                text_legend(hdc, legend, &body);
            } else {
                SelectObject(hdc, g_num);
                SetTextColor(hdc, tone(lit ? C_WHITE : C_NUMERAL));
                text_line(hdc, ctl->label, &face, DT_CENTER);
            }
            break;
        }
        default:
            break;
        }
    }
    part_state_clear();
}

/* ---- the strip: session actions, gamepads, Map ------------------------------------ */

static void draw_face(HDC hdc, const RECT *rect, int pushed, int accent, int inactive)
{
    RECT box = *rect;
    if (accent) {
        FillRect(hdc, &box, g_accent_brush);
        FrameRect(hdc, &box, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(hdc, C_WHITE);
    } else {
        DrawFrameControl(hdc, &box, DFC_BUTTON, DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0) |
                                                    (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static const char *strip_label(int btn)
{
    switch (btn) {
    case SB_ACT_CONFIG: return "Reset to CONFIG";
    case SB_ACT_SOFT: return "Soft Reset";
    case SB_ACT_STOP: return "Debugger Stop";
    case SB_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SB_DEFAULTS: return "Defaults";
    case SB_CLEAR: return "Clear";
    case SB_SHOW: return "Show bindings";
    default: return "Help";
    }
}

/* The target a session-action button maps, or -1. */
static int strip_target(int btn)
{
    return btn == SB_ACT_CONFIG ? SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG)
         : btn == SB_ACT_SOFT ? SMS_TARGET_SYSACT(SMS_SYSACT_SOFT_RESET)
         : btn == SB_ACT_STOP ? SMS_TARGET_SYSACT(SMS_SYSACT_DEBUG_STOP) : -1;
}

static int strip_sysact(int btn)
{
    return strip_target(btn) - SMS_TARGET_SYSACT(0);
}

static void segment_rect(int row, int seg, RECT *rect)
{
    const RECT *rrow = &g_gp_row_rc[row];
    const int xpos = rrow->right - (3 - seg) * (scl(SEG_W) + scl(4)) + scl(4);
    SetRect(rect, xpos, rrow->top + 1, xpos + scl(SEG_W), rrow->bottom - 1);
}

static void gp_name_rect(int row, RECT *rect)
{
    RECT seg0;
    segment_rect(row, 0, &seg0);
    *rect = g_gp_row_rc[row];
    rect->right = seg0.left - scl(110);
}

/* One row per gamepad: its name, the player it drives now, and the Automatic /
 * Player 1 / Player 2 choice, the current one pushed in. */
static void paint_gamepads(HDC hdc)
{
    static const char *const segs[3] = { "Automatic", "Player 1", "Player 2" };
    const int count = smssession_gamepad_count(g_session);
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (count == 0) {
        text_line(hdc, "No gamepads connected", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (count > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", count - PAD_ROWS);
        text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < count && idx < PAD_ROWS; idx++) {
        const int assign = smssession_gamepad_assignment(g_session, idx);
        const int effective = smssession_gamepad_effective_port(g_session, idx);
        const int chosen = assign < 0 || assign >= SMS_PLAYERS ? 0 : assign + 1;
        char name[128], line[32];
        RECT name_rc, eff_rc, seg0;

        gp_name_rect(idx, &name_rc);
        segment_rect(idx, 0, &seg0);
        smssession_gamepad_name(g_session, idx, name, sizeof name);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        text_line(hdc, name, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        if (effective >= 0) snprintf(line, sizeof line, "drives player %d", effective + 1);
        else snprintf(line, sizeof line, "drives nothing");
        SetRect(&eff_rc, name_rc.right + scl(8), name_rc.top, seg0.left - scl(8), name_rc.bottom);
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        text_line(hdc, line, &eff_rc, DT_RIGHT);
        for (seg = 0; seg < 3; seg++) {
            RECT face;
            segment_rect(idx, seg, &face);
            draw_face(hdc, &face, seg == chosen, 0, 0);
            text_line(hdc, segs[seg], &face, DT_CENTER);
        }
    }
}

static void paint_strip(HDC hdc)
{
    int btn;
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SB_COUNT; btn++) {
        const int armed = (btn == SB_MAP && g_map_state != -2) || (btn == SB_SHOW && g_show_bindings) ||
                          (g_map_state >= 0 && strip_target(btn) == g_map_state);
        const int inactive = btn == SB_CLEAR && g_map_state < 0;
        draw_face(hdc, &g_btn_rc[btn], btn == g_mouse_action, armed, inactive);
        text_line(hdc, strip_label(btn), &g_btn_rc[btn], DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    text_line(hdc, g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the console's art supersampled, its text, the strip. Only
 * what the update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    const int factor = 2;
    HDC mem;
    HBITMAP bmp;
    void *bits = NULL;
    BITMAPINFO bmi;
    HGDIOBJ old;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    /* the art: the intersection with the case, at 2x */
    IntersectRect(&art, inval, &g_case_rc);
    if (!IsRectEmpty(&art)) {
        const int wid = art.right - art.left, hgt = art.bottom - art.top;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = wid * factor;
        bmi.bmiHeader.biHeight = -hgt * factor;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        mem = CreateCompatibleDC(hdc);
        bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            RECT all = { 0, 0, wid * factor, hgt * factor };
            old = SelectObject(mem, bmp);
            FillRect(mem, &all, face);
            DeleteObject(face);
            SetBkMode(mem, TRANSPARENT);
            g_art_scale = factor;
            g_art_left = art.left;
            g_art_top = art.top;
            art_case(mem);
            g_art_scale = 1;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchBlt(hdc, art.left, art.top, wid, hgt, mem, 0, 0, wid * factor, hgt * factor, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        SelectObject(hdc, g_font);
        text_parts(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- pressing ----------------------------------------------------------------- */

static void invalidate_part(int idx)
{
    RECT rect;
    if (!g_panel || idx < 0 || idx >= g_nlayout) return;
    part_rect(&g_layout[idx], &rect);
    InflateRect(&rect, scl(14), scl(14));
    InvalidateRect(g_panel, &rect, FALSE);
}

static void invalidate_pad(int port)
{
    int idx;
    if (!g_panel) return;
    for (idx = 0; idx < g_nlayout; idx++)
        if (g_layout[idx].kind == SMS_CTL_PAD && g_layout[idx].port == port)
            invalidate_part(idx);
}

static void set_pointer(int port, unsigned mask)
{
    if (port < 0 || port >= SMS_PLAYERS || g_pointer[port] == mask) return;
    g_pointer[port] = mask;
    smssession_pointer_set(g_session, port, mask);
}

/* Lets go of what the pointer holds in this window (only that: the main window
 * releases its own keys). A session action fires on release inside its
 * button, like any button. */
static void mouse_release(int fire_action)
{
    const int part = g_mouse_part, action = g_mouse_action, port = g_mouse_port;
    g_mouse_part = -1;
    g_mouse_port = -1;
    g_mouse_action = -1;
    set_pointer(0, 0);
    set_pointer(1, 0);
    if (part >= 0 && part < g_nlayout) {
        const sms_control *ctl = &g_layout[part];
        if (ctl->kind == SMS_CTL_SWITCH)
            smssession_press(g_session, ctl->target, 0);
        invalidate_part(part);
    }
    if (action >= 0) {
        if (fire_action) smssession_sysaction_post(g_session, strip_sysact(action));
        if (g_panel) InvalidateRect(g_panel, &g_btn_rc[action], FALSE);
    }
    if (port >= 0) invalidate_pad(port);
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

#define IDLE_HINT "Hold a control with the mouse; drag on the D-pad dish for diagonals. Map rebinds."

static void set_map_state(int state)
{
    g_map_state = state;
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        smssession_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint(IDLE_HINT);
    } else if (state == -1) {
        set_hint("Click a control to remap. Esc finishes.");
    } else {
        char msg[200];
        mouse_release(0);
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", sms_target_name(state));
        set_hint(msg);
        smssession_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(const char *what, const char *stolen)
{
    char msg[260];
    if (stolen[0]) snprintf(msg, sizeof msg, "Bound %s (it was %s's)", what, stolen);
    else snprintf(msg, sizeof msg, "Bound %s", what);
    set_hint(msg);
}

/* What the pointer is over, said in the hint line: the target and what drives it. */
static void hover_hint(void)
{
    char msg[300], key[40];
    if (g_map_state != -2 || g_mouse_part >= 0 || g_mouse_port >= 0 || g_mouse_action >= 0) return;
    if (g_hover >= 0 && g_hover < g_nlayout && g_layout[g_hover].target >= 0) {
        const int target = g_layout[g_hover].target;
        const sms_binding bind = smssession_binding_get(g_session, target);
        char name[64];
        snprintf(name, sizeof name, "%s", sms_target_name(target));      /* a static buffer */
        key[0] = '\0';
        smssession_keysym_name(bind.keysym, key, sizeof key);
        snprintf(msg, sizeof msg, "%s   Key: %s   Gamepad: %s", name, key[0] ? key : "none",
                 bind.button != SMS_PAD_BTN_NONE ? sms_pad_button_name(bind.button) : "none");
        set_hint(msg);
    } else {
        set_hint(IDLE_HINT);
    }
}

/* ---- layout --------------------------------------------------------------------- */

static int console_now(void)
{
    const int console = smssession_console(g_session);
    return console >= 0 && console < SMS_CONSOLE_COUNT ? console : SMS_CONSOLE_SMS1;
}

/* The case with the console in it, the actions, the gamepad rows, the Map
 * row: the window is this size always (the layout's extent is the same for
 * every console). */
static void layout_static(void)
{
    const int left = scl(MARGIN);
    int ypos = scl(MARGIN), idx, xpos, content_w;
    static const int widths[SB_COUNT] = { ACT_W, ACT_W, ACT_W, MAP_W, DEFAULTS_W, CLEAR_W, SHOW_W, HELP_W };

    g_layout = smssession_controls_layout(console_now(), &g_nlayout, &g_lay_wid, &g_lay_hgt);
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    content_w = (int)ceil((double)g_lay_wid * g_unit) + 2 * scl(CASE_PAD);
    g_total_w = 2 * left + content_w;

    SetRect(&g_case_rc, left, ypos, left + content_w, 0);
    g_orgx = g_case_rc.left + scl(CASE_PAD);
    g_orgy = g_case_rc.top + scl(CASE_PAD);
    g_case_rc.bottom = (int)ceil(g_orgy + (double)g_lay_hgt * g_unit) + scl(CASE_PAD);
    ypos = g_case_rc.bottom + scl(10);

    for (idx = 0; idx < g_nlayout; idx++) {
        const sms_control *ctl = &g_layout[idx];
        if (ctl->kind == SMS_CTL_DPAD && ctl->port >= 0 && ctl->port < SMS_PLAYERS) {
            g_disc[ctl->port].ctrx = g_orgx + ((double)ctl->left + ctl->width / 2) * g_unit;
            g_disc[ctl->port].ctry = g_orgy + ((double)ctl->top + ctl->height / 2) * g_unit;
        }
    }

    /* the session's three actions */
    xpos = left;
    for (idx = SB_ACT_CONFIG; idx <= SB_ACT_STOP; idx++) {
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + scl(widths[idx]), ypos + scl(BTN_H));
        xpos += scl(widths[idx]) + scl(6);
    }
    ypos += scl(BTN_H) + scl(10);

    SetRect(&g_gp_head_rc, left, ypos, left + content_w, ypos + scl(HEAD_H));
    ypos += scl(HEAD_H);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, ypos, left + content_w, ypos + scl(ROW_H));
        ypos += scl(ROW_H) + scl(2);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, ypos);
    ypos += scl(8);

    xpos = left;
    for (idx = SB_MAP; idx < SB_COUNT; idx++) {
        if (idx == SB_HELP) xpos = left + content_w - scl(HELP_W);
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + scl(widths[idx]), ypos + scl(BTN_H));
        xpos += scl(widths[idx]) + scl(6);
        if (idx == SB_CLEAR) xpos += scl(8);
    }
    ypos += scl(BTN_H) + scl(6);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + scl(20));
    g_total_h = ypos + scl(20) + scl(MARGIN);
}

static void rebuild(void)
{
    mouse_release(0);
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    g_hover = -1;
    g_built_console = console_now();
    make_fonts();
    layout_static();
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* ---- polling -------------------------------------------------------------------------- */

/* What the console sees held, lit; and the console itself, which Preferences
 * can swap under an open window (RESET comes and goes, RAPID replaces it). */
static void refresh_held(void)
{
    unsigned held[SMS_PLAYERS];
    int port, swit;
    if (!g_panel) return;
    if (g_built_console != console_now()) {
        rebuild();
        return;
    }
    if (g_map_state != -2) return;
    for (port = 0; port < SMS_PLAYERS; port++) {
        held[port] = smssession_buttons_held(g_session, port);
        if (held[port] != g_shown_held[port]) {
            g_shown_held[port] = held[port];
            invalidate_pad(port);
        }
    }
    for (swit = 0; swit < SMS_SW_COUNT; swit++) {
        const int now = smssession_switch_held(g_session, swit) != 0;
        if (now != g_shown_sw[swit]) {
            int idx;
            g_shown_sw[swit] = now;
            for (idx = 0; idx < g_nlayout; idx++)
                if (g_layout[idx].kind == SMS_CTL_SWITCH && g_layout[idx].target == SMS_TARGET_SWITCH(swit))
                    invalidate_part(idx);
        }
    }
}

/* Hot-plugged pads and auto-assignment changes. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    memset(&now, 0, sizeof now);
    now.generation = smssession_gamepad_generation(g_session);
    now.count = smssession_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = smssession_gamepad_assignment(g_session, idx);
        now.effective[idx] = smssession_gamepad_effective_port(g_session, idx);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
    if (g_built_console != console_now())
        rebuild();
}

/* ---- keys ------------------------------------------------------------------------------ */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release(0);
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vkey = 0;
    ShowWindow(g_panel, SW_HIDE);
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F9, F11 or
 * F12, which are the app's). */
int sms_controller_map_key(UINT msg, WPARAM vkey, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (!g_panel) return 0;
    if (g_swallow_vkey && vkey == g_swallow_vkey) {
        if (!down) g_swallow_vkey = 0;
        return 1;
    }
    if (g_map_state == -2) return 0;
    if (vkey == VK_F9 || vkey == VK_F11 || vkey == VK_F12) return 0;
    if (msg == WM_SYSKEYDOWN && vkey == VK_F4) return 0;
    if (!down) {
        /* a key held before Map armed must still let go */
        const uint32_t held = sms_keysym_from_msg(vkey, lparam);
        if (held) smssession_key(g_session, held, 0);
        return 1;
    }
    if (lparam & (1 << 30)) return 1;
    if (vkey == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vkey = vkey;
        return 1;
    }
    if (g_map_state >= 0) {
        char stolen[128], name[32];
        const int target = g_map_state;
        const uint32_t keysym = sms_keysym_from_msg(vkey, lparam);
        if (!keysym) return 1;              /* nothing to call it: keep waiting */
        stolen[0] = '\0';
        smssession_binding_set_key(g_session, target, keysym, stolen, sizeof stolen);
        smssession_keysym_name(keysym, name, sizeof name);
        g_swallow_vkey = vkey;
        set_map_state(-1);                  /* stay armed: remapping several in a row is normal */
        bound_hint(name, stolen);
    }
    return 1;
}

/* A key with the window focused: 1 if taken. */
static int panel_key(UINT msg, WPARAM vkey, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    uint32_t keysym;
    int sysact;

    if (sms_controller_map_key(msg, vkey, lparam)) return 1;
    if (vkey == VK_F9) {
        if (down && !(lparam & (1 << 30))) hide_panel();
        return 1;
    }
    if (sms_app_key(vkey)) {
        /* the main window's shortcuts, as if it had the focus */
        if (g_parent && down) SendMessageA(g_parent, msg, vkey, lparam);
        return 1;
    }
    keysym = sms_keysym_from_msg(vkey, lparam);
    if (msg == WM_KEYDOWN && keysym && (sysact = smssession_key_sysaction(g_session, keysym)) >= 0) {
        /* the main window runs these */
        if (down && !(lparam & (1 << 30))) smssession_sysaction_post(g_session, sysact);
        return 1;
    }
    return sms_forward_key(g_session, msg, vkey, lparam);
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wrect, crect;
    GetWindowRect(g_panel, &wrect);
    GetClientRect(g_panel, &crect);
    if (crect.right == g_total_w && crect.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wrect.right - wrect.left) - crect.right + g_total_w,
                 (wrect.bottom - wrect.top) - crect.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *size)
{
    RECT wrect, crect;
    if (hwnd && GetWindowRect(hwnd, &wrect) && GetClientRect(hwnd, &crect) && crect.right > 0) {
        size->cx = (wrect.right - wrect.left) - crect.right + g_total_w;
        size->cy = (wrect.bottom - wrect.top) - crect.bottom + g_total_h;
    } else {
        SetRect(&wrect, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wrect, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        size->cx = wrect.right - wrect.left;
        size->cy = wrect.bottom - wrect.top;
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- the mouse ------------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT pos)
{
    if (!g_layout) return -1;
    return smssession_controls_hit(console_now(), (float)((pos.x - g_orgx) / g_unit),
                                   (float)((pos.y - g_orgy) / g_unit));
}

/* A dish drag: where the pointer is round the dish decides which one or two
 * directions are held (nothing near the middle). */
static void dish_follow(int port, POINT pos)
{
    const double delx = pos.x - g_disc[port].ctrx, dely = pos.y - g_disc[port].ctry;
    set_pointer(port, smssession_dpad_from_vector((float)delx, (float)dely, (float)(DISC_RADIUS * g_unit)));
    invalidate_pad(port);
}

static void mouse_down(HWND hwnd, POINT pos)
{
    int idx, row, seg, btn;
    SetFocus(hwnd);

    for (btn = 0; btn < SB_COUNT; btn++) {
        if (!PtInRect(&g_btn_rc[btn], pos)) continue;
        switch (btn) {
        case SB_MAP: set_map_state(g_map_state == -2 ? -1 : -2); return;
        case SB_DEFAULTS:
            smssession_bindings_reset(g_session);
            set_hint("Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_CLEAR:
            if (g_map_state >= 0) {
                char stolen[16];
                smssession_binding_set_key(g_session, g_map_state, 0, NULL, 0);
                smssession_binding_set_button(g_session, g_map_state, SMS_PAD_BTN_NONE, stolen, sizeof stolen);
                set_hint("Cleared. Press a key or a gamepad button to bind it again");
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return;
        case SB_SHOW:
            g_show_bindings = !g_show_bindings;
            smssession_set_int(g_session, "controllers_show_bindings", g_show_bindings);
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_HELP: sms_help_show(hwnd, "controllers"); return;
        default:
            if (g_map_state != -2) {                 /* armed: picks the action */
                set_map_state(strip_target(btn));
            } else {
                mouse_release(0);
                g_mouse_action = btn;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &g_btn_rc[btn], FALSE);
            }
            return;
        }
    }
    for (row = 0; row < PAD_ROWS && row < smssession_gamepad_count(g_session); row++)
        for (seg = 0; seg < 3; seg++) {
            RECT face;
            segment_rect(row, seg, &face);
            if (PtInRect(&face, pos)) {
                /* 0 = automatic (connection order), 1-2 = that player */
                smssession_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    idx = part_at(pos);
    if (idx < 0) return;
    {
        const sms_control *ctl = &g_layout[idx];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target */
            if (ctl->target >= 0) set_map_state(ctl->target);
            return;
        }
        mouse_release(0);
        switch (ctl->kind) {
        case SMS_CTL_DPAD:
            g_mouse_port = ctl->port;
            dish_follow(ctl->port, pos);
            break;
        case SMS_CTL_DIRECTION:
        case SMS_CTL_BUTTON:
            g_mouse_part = idx;
            set_pointer(ctl->port, ctl->act_mask);
            invalidate_pad(ctl->port);
            break;
        case SMS_CTL_SWITCH:
            g_mouse_part = idx;
            smssession_press(g_session, ctl->target, 1);
            break;
        default:
            return;
        }
        invalidate_part(idx);
        SetCapture(hwnd);
        refresh_held();
    }
}

static void mouse_move(HWND hwnd, POINT pos)
{
    if (!g_tracking) {
        TRACKMOUSEEVENT track;
        track.cbSize = sizeof track;
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        track.dwHoverTime = 0;
        g_tracking = TrackMouseEvent(&track) != 0;
    }
    if (g_mouse_port >= 0) {
        dish_follow(g_mouse_port, pos);
        return;
    }
    if (g_mouse_part >= 0) {
        /* dragged off what it holds: let go, as a button does */
        if (g_mouse_part >= g_nlayout || part_at(pos) != g_mouse_part) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        return;
    }
    if (g_mouse_action >= 0) {
        if (!PtInRect(&g_btn_rc[g_mouse_action], pos)) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return;
    }
    {
        const int now = part_at(pos);
        if (now != g_hover) {
            invalidate_part(g_hover);
            g_hover = now;
            invalidate_part(g_hover);
            hover_hint();
        }
    }
}

static void mouse_up(HWND hwnd)
{
    if (g_mouse_part >= 0 || g_mouse_port >= 0 || g_mouse_action >= 0) {
        mouse_release(1);
        refresh_held();
    }
    if (GetCapture() == hwnd) ReleaseCapture();
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT inval = paint.rcPaint;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        const int wid = inval.right - inval.left, hgt = inval.bottom - inval.top;
        if (wid <= 0 || hgt <= 0) { EndPaint(hwnd, &paint); return 0; }
        mem = CreateCompatibleDC(hdc);
        bmp = CreateCompatibleBitmap(hdc, wid, hgt);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, g_font);
        SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
        paint_rect(mem, &inval);
        SetViewportOrgEx(mem, 0, 0, NULL);
        BitBlt(hdc, inval.left, inval.top, wid, hgt, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pos;
        pos.x = GET_X_LPARAM(lparam);
        pos.y = GET_Y_LPARAM(lparam);
        mouse_down(hwnd, pos);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pos;
        pos.x = GET_X_LPARAM(lparam);
        pos.y = GET_Y_LPARAM(lparam);
        mouse_move(hwnd, pos);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover >= 0) {
            invalidate_part(g_hover);
            g_hover = -1;
            hover_hint();
        }
        return 0;
    case WM_LBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        mouse_up(hwnd);
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lparam != hwnd) {
            mouse_release(0);
            refresh_held();
        }
        return 0;

    case WM_TIMER:
        if (wparam == IDT_CAPTURE) {
            int button;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (smssession_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_map_state;
                stolen[0] = '\0';
                smssession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(sms_pad_button_name(button), stolen);
            }
        } else if (wparam == IDT_HELD) {
            refresh_held();
        } else if (wparam == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: a bound Alt is the console's */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter: the main window's menu mnemonics, as when it has the focus */
        if (wparam != ' ' && g_parent) { SendMessageA(g_parent, msg, wparam, lparam); return 0; }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            const HWND other = (HWND)lparam;
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vkey = 0;     /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                smssession_release_all(g_session);
            refresh_held();
        }
        break;                      /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            mouse_release(0);
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE size;
            window_size(hwnd, &size);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = size.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = size.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *want = (const RECT *)lparam;
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, want->left, want->top, want->right - want->left, want->bottom - want->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release(0);
        set_map_state(-2);
        g_panel = NULL;
        g_built_console = -1;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;

    g_show_bindings = smssession_get_int(g_session, "controllers_show_bindings", 0) > 0;

    if (!registered) {
        WNDCLASSEXA wclass;
        memset(&wclass, 0, sizeof wclass);
        wclass.cbSize = sizeof wclass;
        wclass.lpfnWndProc = panel_proc;    /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wclass.hInstance = inst;
        wclass.hCursor = LoadCursor(NULL, IDC_ARROW);
        wclass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wclass.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wclass) != 0;
    }
    if (!g_accent_brush)
        g_accent_brush = CreateSolidBrush(rgb_of(SMSSESSION_ACCENT_RGB));
    g_dpi = sms_window_dpi(g_parent);
    make_fonts();
    layout_static();
    g_built_console = console_now();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    sms_help_set_context(g_panel, "controllers", 0);    /* keys here go to the console */
    rescale(sms_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    memset(g_shown_sw, 0, sizeof g_shown_sw);
    memset(g_shown_held, 0, sizeof g_shown_held);
    g_hover = -1;
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    if (g_built_console != console_now())
        rebuild();
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

static int ensure_panel(HWND parent, smssession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    return g_panel || create_panel();
}

void sms_controller_window_toggle(HWND parent, smssession *session)
{
    if (!ensure_panel(parent, session))
        return;
    if (IsWindowVisible(g_panel))
        hide_panel();
    else
        show_panel();
}

void sms_controller_window_show(HWND parent, smssession *session)
{
    if (!ensure_panel(parent, session))
        return;
    if (IsWindowVisible(g_panel))
        SetForegroundWindow(g_panel);
    else
        show_panel();
}

int sms_controller_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void sms_controller_window_gamepads_changed(void)
{
    if (!g_panel)
        return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();             /* rebuilds if the console changed */
        InvalidateRect(g_panel, NULL, FALSE);
    } else {
        g_built_console = -1;       /* rebuilt when next shown */
    }
}

int sms_controller_pretranslate(MSG *msg)
{
    /* The window has no child controls, so its own window proc sees every
     * key; nothing to steal here. Kept for symmetry with the debugger. */
    (void)msg;
    return 0;
}
