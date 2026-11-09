/*
 * The Controllers window -- the Master System as a player sees it: the
 * console's top strip across the head of a charcoal case (the SEGA wordmark,
 * the model in the Sega red, and the buttons this model has: PAUSE, and RESET
 * or RAPID) and the two Control Pads below it, each a black pad with a black
 * D-pad in its dish and the two square buttons 1 and 2. Everything is drawn
 * from the session's layout (smssession_controls_layout), so every frontend
 * shows the same console, and the layout follows the console that is running:
 * RESET comes and goes, RAPID takes its place on the Japanese model, the
 * wordmark changes and the window keeps its size.
 *
 * Mouse: a press on a cross arm or on button 1 or 2 holds it, a press on the
 * D-pad's dish (the centre, the diagonal corners or just past an arm) starts
 * a drag that holds one or two directions as the pointer leaves the centre
 * (smssession_dpad_from_vector -> smssession_pointer_set), and a press on a
 * console button holds it. Every press is the raw press and release (a
 * GtkGestureDrag, not a click: a click gives up when the pointer wobbles),
 * because the machine latches its inputs once per frame -- a press present
 * only for the instant of a click falls between frames. Losing the focus,
 * hiding and the pointer leaving a held arm or button let go of whatever the
 * mouse holds here.
 *
 * What the console sees held -- keyboard, gamepads and this window together
 * (smssession_buttons_held, smssession_switch_held) -- is polled while the
 * window is on screen and painted in the accent, so the window doubles as a
 * gamepad tester.
 *
 * Keys typed with the window active behave exactly as in the main window:
 * a system action first, on its leading edge, then the machine's keys.
 *
 * FIXED SIZE, FLOATING: the console is a drawing area of one size, the labels
 * ellipsize rather than grow, the pad rows sit in a fixed-height area and the
 * window is not resizable, so neither Map mode, a long gamepad name nor a
 * console change alters its size and tiling compositors float it. It is
 * hidden at startup (F9 or the menu shows it).
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 * Its timers run only while it is on screen.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a control to pick
 * the target, then press a keyboard key OR a gamepad button. While armed the
 * same gestures pick the target instead of pressing, every control shows the
 * key bound to it, and typed keys are not typed into the console; once a
 * target is picked the other controls are greyed. Rebinding steals the key
 * from whatever held it. Escape backs out one level. Clear takes the picked
 * target's key and button away, Defaults puts every binding back. F9, F11 and
 * F12 are the app's own and are never Map's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../help/help_window.h"
#include "../window.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define UNIT 40.0               /* pixels per layout unit */
#define CASE_PAD 16             /* the charcoal case around everything */
#define MAX_PARTS 32
#define STRIP_BW 188
#define STRIP_GAP 10
#define STRIP_H 34
#define STRIP_COUNT 3
#define MAX_PADS 8              /* pads listed (SDL opens few more) */
#define PAD_ROWS 3              /* pad rows the window reserves; more scroll */
#define PAD_ROW_H 44            /* one row's height, px */
#define PAD_GAP 4
#define DISH_RADIUS 1.55        /* the D-pad's dish, units (the core's disc) */
#define ARM_HALF 0.5            /* half the cross arms' width, units */
#define ARM_REACH 1.5           /* the cross's reach from its centre, units */
#define DIR_MASK ((1u << SMS_ACT_UP) | (1u << SMS_ACT_DOWN) | \
                  (1u << SMS_ACT_LEFT) | (1u << SMS_ACT_RIGHT))
#define BOUND_CHARS 5           /* a bound key's legend, ellipsized to this */

typedef struct {
    double xpos, ypos, wid, hei;
    int target;
    const char *label;
} StripItem;

typedef struct {
    GtkWidget *area;
    StripItem items[STRIP_COUNT];
    int count, pressed, hover;
} Strip;

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the SMSWindow: system actions go through it */
static smssession *g_session;
static AdwWindowTitle *g_title;
static GtkWidget *g_area;       /* the console's drawing area */
static Strip g_strip;

static const sms_control *g_layout;     /* smssession_controls_layout: static */
static int g_part_count;
static float g_lay_wid, g_lay_hei;
static int g_console = -1;              /* the console the layout is for */
static gboolean g_lit[MAX_PARTS];       /* what the console sees held */
static unsigned g_held[SMS_PLAYERS];    /* each pad's merged contacts */
static int g_hover = -1;                /* the part under the pointer */
static int g_pressed = -1;              /* the arm, button or cap the mouse holds */
static gboolean g_sysact_down[SMS_SYSACT_COUNT];

/* The D-pad dish the mouse is dragging. */
static struct {
    gboolean active;
    int port;
    double centx, centy;        /* the dish's centre, pixels */
} g_drag;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button;
static GtkWidget *g_map_hint;
static GtkWidget *g_show_bindings;
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_eff[MAX_PADS];  /* each pad's "drives player N" */
static int g_pad_row_count;
static unsigned g_pad_generation = (unsigned)-1;
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_info_timer;
static guint g_held_timer;

static void refresh_all(void);

/* "controllers_show_bindings": the frontends' own key (the core never reads it). */
static gboolean show_bindings_saved(void)
{
    return smssession_get_int(g_session, "controllers_show_bindings", 0) > 0;
}

/* ---- small helpers ---------------------------------------------------------- */

static void rgb(cairo_t *ctx, guint32 colour)
{
    cairo_set_source_rgb(ctx, ((colour >> 16) & 0xFF) / 255.0, ((colour >> 8) & 0xFF) / 255.0,
                         (colour & 0xFF) / 255.0);
}

static guint32 mix(guint32 first, guint32 second, double frac)
{
    guint32 out = 0;
    int shift;
    for (shift = 16; shift >= 0; shift -= 8) {
        double chan1 = (first >> shift) & 0xFF, chan2 = (second >> shift) & 0xFF;
        out |= (guint32)(chan1 + (chan2 - chan1) * frac + 0.5) << shift;
    }
    return out;
}

static void stop_rgb(cairo_pattern_t *pat, double offset, guint32 colour)
{
    cairo_pattern_add_color_stop_rgb(pat, offset, ((colour >> 16) & 0xFF) / 255.0,
                                     ((colour >> 8) & 0xFF) / 255.0, (colour & 0xFF) / 255.0);
}

/* A vertical two-stop gradient as the current source. */
static void gradient(cairo_t *ctx, double ypos, double hei, guint32 top, guint32 bottom)
{
    cairo_pattern_t *pat = cairo_pattern_create_linear(0, ypos, 0, ypos + hei);
    stop_rgb(pat, 0, top);
    stop_rgb(pat, 1, bottom);
    cairo_set_source(ctx, pat);
    cairo_pattern_destroy(pat);
}

static void rrect(cairo_t *ctx, double xpos, double ypos, double wid, double hei, double rad)
{
    if (rad > wid / 2) rad = wid / 2;
    if (rad > hei / 2) rad = hei / 2;
    cairo_new_sub_path(ctx);
    cairo_arc(ctx, xpos + wid - rad, ypos + rad, rad, -G_PI / 2, 0);
    cairo_arc(ctx, xpos + wid - rad, ypos + hei - rad, rad, 0, G_PI / 2);
    cairo_arc(ctx, xpos + rad, ypos + hei - rad, rad, G_PI / 2, G_PI);
    cairo_arc(ctx, xpos + rad, ypos + rad, rad, G_PI, 3 * G_PI / 2);
    cairo_close_path(ctx);
}

/* Text centred in a box, in an absolute pixel size, ellipsized to the box. */
static void legend(cairo_t *ctx, GtkWidget *widget, const char *text, double pixels,
                   gboolean bold, guint32 colour, double xpos, double ypos,
                   double boxw, double boxh)
{
    PangoLayout *lay = gtk_widget_create_pango_layout(widget, text);
    PangoFontDescription *fontd = pango_font_description_new();
    int txtw, txth;

    pango_font_description_set_family(fontd, "Sans");
    pango_font_description_set_weight(fontd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(fontd, pixels * PANGO_SCALE);
    pango_layout_set_font_description(lay, fontd);
    pango_layout_set_width(lay, (int)(boxw * PANGO_SCALE));
    pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(lay, PANGO_ALIGN_CENTER);
    pango_layout_get_pixel_size(lay, &txtw, &txth);
    rgb(ctx, colour);
    cairo_move_to(ctx, xpos, ypos + (boxh - txth) / 2.0);
    pango_cairo_show_layout(ctx, lay);
    /* the move_to above is still the current point, and the next arc would
     * join it with a line */
    cairo_new_path(ctx);
    pango_font_description_free(fontd);
    g_object_unref(lay);
}

/* The console's wordmark: "SEGA" light and letter-spaced, then the model in
 * the accent, left aligned in the box. */
static void wordmark(cairo_t *ctx, GtkWidget *widget, const char *text, double xpos,
                     double ypos, double boxw, double boxh)
{
    PangoLayout *lay = gtk_widget_create_pango_layout(widget, NULL);
    PangoFontDescription *fontd = pango_font_description_new();
    const char *model = g_str_has_prefix(text, "SEGA ") ? text + 5 : "";
    char *escaped = g_markup_escape_text(model, -1);
    char *markup = g_strdup_printf(
        "<span foreground=\"#F4F4F4\" weight=\"bold\" letter_spacing=\"2048\">SEGA</span>"
        "  <span foreground=\"#%06X\" weight=\"bold\" letter_spacing=\"1024\">%s</span>",
        (unsigned)SMSSESSION_ACCENT_RGB, escaped);
    int txtw, txth;

    pango_font_description_set_family(fontd, "Sans");
    pango_font_description_set_absolute_size(fontd, 16 * PANGO_SCALE);
    pango_layout_set_font_description(lay, fontd);
    pango_layout_set_markup(lay, markup, -1);
    pango_layout_set_width(lay, (int)(boxw * PANGO_SCALE));
    pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);
    pango_layout_get_pixel_size(lay, &txtw, &txth);
    cairo_move_to(ctx, xpos, ypos + (boxh - txth) / 2.0);
    pango_cairo_show_layout(ctx, lay);
    cairo_new_path(ctx);
    pango_font_description_free(fontd);
    g_free(markup);
    g_free(escaped);
    g_object_unref(lay);
}

/* A host key's name as a small legend: arrows as glyphs, the long names
 * shortened, anything over BOUND_CHARS ellipsized. */
static void legend_name(guint32 keysym, char *dst, int dstsz)
{
    char name[32];
    glong len;

    switch (keysym) {
    case SMS_KEYSYM_UP:    g_snprintf(dst, dstsz, "\xe2\x86\x91"); return;
    case SMS_KEYSYM_DOWN:  g_snprintf(dst, dstsz, "\xe2\x86\x93"); return;
    case SMS_KEYSYM_LEFT:  g_snprintf(dst, dstsz, "\xe2\x86\x90"); return;
    case SMS_KEYSYM_RIGHT: g_snprintf(dst, dstsz, "\xe2\x86\x92"); return;
    default: break;
    }
    smssession_keysym_name(keysym, name, sizeof name);
    if (g_str_has_prefix(name, "Keypad "))
        g_snprintf(dst, dstsz, "KP %s", name + 7);
    else if (g_str_has_prefix(name, "Left "))
        g_snprintf(dst, dstsz, "L %s", name + 5);
    else if (g_str_has_prefix(name, "Right "))
        g_snprintf(dst, dstsz, "R %s", name + 6);
    else if (!strcmp(name, "Backspace"))
        g_snprintf(dst, dstsz, "BkSp");
    else if (!strcmp(name, "Escape"))
        g_snprintf(dst, dstsz, "Esc");
    else if (!strcmp(name, "Return"))
        g_snprintf(dst, dstsz, "Enter");
    else
        g_snprintf(dst, dstsz, "%s", name);
    len = g_utf8_strlen(dst, -1);
    if (len > BOUND_CHARS) {
        char *cut = g_utf8_offset_to_pointer(dst, BOUND_CHARS - 1);
        *cut = '\0';
        g_strlcat(dst, "\xe2\x80\xa6", dstsz);
    }
}

static gboolean is_sysact(int target)
{
    return target >= SMS_TARGET_SYSACT(0);
}

static gboolean showing_bindings(void)
{
    return g_map_state != -2 ||
           (g_show_bindings && gtk_check_button_get_active(GTK_CHECK_BUTTON(g_show_bindings)));
}

/* The binding legend for a target, or "" when none or not shown. */
static void bound_text(int target, char *dst, int dstsz)
{
    sms_binding bind;
    dst[0] = '\0';
    if (target < 0 || !showing_bindings()) return;
    bind = smssession_binding_get(g_session, target);
    if (bind.keysym)
        legend_name(bind.keysym, dst, dstsz);
    else
        g_snprintf(dst, dstsz, "\xe2\x80\x94");
}

/* "Player 1: Left\nKey: Left Arrow    Gamepad: D-pad Left" */
static void target_tip(int target, char *dst, int dstsz)
{
    const sms_binding bind = smssession_binding_get(g_session, target);
    char key[32];
    smssession_keysym_name(bind.keysym, key, sizeof key);
    g_snprintf(dst, dstsz, "%s\nKey: %s    Gamepad: %s", sms_target_name(target),
               key[0] ? key : "none",
               bind.button != SMS_PAD_BTN_NONE ? sms_pad_button_name(bind.button) : "none");
}

/* ---- geometry ---------------------------------------------------------------- */

static double px_x(double unitx) { return CASE_PAD + unitx * UNIT; }
static double px_y(double unity) { return CASE_PAD + unity * UNIT; }

static int area_width(void)
{
    return (int)(2 * CASE_PAD + g_lay_wid * UNIT + 0.5);
}

static int area_height(void)
{
    return (int)(2 * CASE_PAD + g_lay_hei * UNIT + 0.5);
}

/* The D-pad dish of a layout part: centre in pixels. */
static void dish_centre(const sms_control *ctl, double *centx, double *centy)
{
    *centx = px_x(ctl->left + ctl->width / 2);
    *centy = px_y(ctl->top + ctl->height / 2);
}

/* ---- what is held --------------------------------------------------------------- */

static gboolean part_held(const sms_control *ctl)
{
    switch (ctl->kind) {
    case SMS_CTL_SWITCH:
        return smssession_switch_held(g_session, ctl->target - SMS_TARGET_SWITCH(0)) != 0;
    case SMS_CTL_DIRECTION:
    case SMS_CTL_BUTTON:
        return ctl->port >= 0 && ctl->port < SMS_PLAYERS &&
               (g_held[ctl->port] & ctl->act_mask) != 0;
    default:
        return FALSE;
    }
}

/* ---- drawing ------------------------------------------------------------------- */

#define COL_CASE_TOP 0x232326
#define COL_CASE_BOT 0x131315
#define COL_CASE_EDGE 0x3C3C40
#define COL_STRIP 0x0B0B0C
#define COL_STRIP_EDGE 0x2A2A2D
#define COL_CAP_TOP 0x3A3A3E
#define COL_CAP_BOT 0x1C1C1F
#define COL_CAP_EDGE 0x050506
#define COL_CAP_SIDE 0x070708
#define COL_CAP_INK 0xEDEDED
#define COL_RECESS 0x1B1B1D
#define COL_CAPTION 0x9A9AA0
#define COL_PAD_TOP 0x303033
#define COL_PAD_BOT 0x171719
#define COL_PAD_HIGH 0x505055
#define COL_PAD_EDGE 0x0A0A0B
#define COL_PAD_MARK 0x8E8E94
#define COL_DISH 0x0C0C0D
#define COL_DISH_RING 0x2B2B2E
#define COL_ARM_TOP 0x2A2A2D
#define COL_ARM_BOT 0x151517
#define COL_ARM_EDGE 0x050505
#define COL_ARM_HIGH 0x4A4A4F
#define COL_ARM_MARK 0x777780
#define COL_KEY_TOP 0x2E2E32
#define COL_KEY_BOT 0x151517
#define COL_KEY_INK 0xE6E6E6
#define COL_ACCENT SMSSESSION_ACCENT_RGB
#define COL_ACCENT_DARK SMSSESSION_ACCENT_DARK_RGB
#define COL_WHITE 0xFFFFFF

typedef struct {
    gboolean held, selected, dim, hot;
} PartState;

static PartState part_state(int idx)
{
    const sms_control *ctl = &g_layout[idx];
    PartState state;
    const gboolean armed = g_map_state != -2;
    state.held = g_lit[idx] || g_pressed == idx;
    state.selected = g_map_state >= 0 && ctl->target >= 0 && g_map_state == ctl->target;
    state.dim = armed && (ctl->target < 0 || (g_map_state >= 0 && !state.selected));
    state.hot = g_hover == idx && !state.held;
    return state;
}

static void dim_begin(cairo_t *ctx, gboolean dim)
{
    if (dim) cairo_push_group(ctx);
}

static void dim_end(cairo_t *ctx, gboolean dim)
{
    if (!dim) return;
    cairo_pop_group_to_source(ctx);
    cairo_paint_with_alpha(ctx, 0.4);
}

/* The case, with its top highlight. */
static void draw_case(cairo_t *ctx, int wid, int hei)
{
    gradient(ctx, 0, hei, COL_CASE_TOP, COL_CASE_BOT);
    rrect(ctx, 0.5, 0.5, wid - 1, hei - 1, 14);
    cairo_fill(ctx);
    cairo_move_to(ctx, 14, 1.5);
    cairo_line_to(ctx, wid - 14, 1.5);
    rgb(ctx, COL_CASE_EDGE);
    cairo_set_line_width(ctx, 1.5);
    cairo_stroke(ctx);
}

/* The console's top strip: black, the accent stripe along its lower edge, the
 * wordmark at the left. */
static void draw_head(cairo_t *ctx, GtkWidget *widget, const sms_control *ctl)
{
    const double xpos = px_x(ctl->left), ypos = px_y(ctl->top);
    const double wid = ctl->width * UNIT, hei = ctl->height * UNIT;

    rrect(ctx, xpos, ypos, wid, hei, 7);
    rgb(ctx, COL_STRIP);
    cairo_fill_preserve(ctx);
    cairo_save(ctx);
    cairo_clip(ctx);
    rgb(ctx, COL_ACCENT);
    cairo_rectangle(ctx, xpos, ypos + hei - 3, wid, 3);
    cairo_fill(ctx);
    cairo_restore(ctx);
    rrect(ctx, xpos + 0.5, ypos + 0.5, wid - 1, hei - 1, 7);
    rgb(ctx, COL_STRIP_EDGE);
    cairo_set_line_width(ctx, 1);
    cairo_stroke(ctx);
    if (ctl->legend)
        wordmark(ctx, widget, ctl->legend, xpos + 0.4 * UNIT, ypos, 10.0 * UNIT, hei - 3);
}

/* A console button: PAUSE, RESET or RAPID, a rectangular cap with its name. */
static void draw_cap(cairo_t *ctx, GtkWidget *widget, int idx)
{
    const sms_control *ctl = &g_layout[idx];
    const PartState state = part_state(idx);
    const double xpos = px_x(ctl->left), ypos = px_y(ctl->top);
    const double wid = ctl->width * UNIT, hei = ctl->height * UNIT - 3;
    const gboolean lit = state.held || state.selected;
    const double drop = state.held ? 2 : 0;
    char bound[40];
    guint32 top = COL_CAP_TOP, bot = COL_CAP_BOT, ink = COL_CAP_INK;

    if (lit) {
        top = bot = COL_ACCENT;
        ink = COL_WHITE;
    } else if (state.hot) {
        top = mix(top, COL_WHITE, 0.12);
        bot = mix(bot, COL_WHITE, 0.12);
    }
    dim_begin(ctx, state.dim);
    rrect(ctx, xpos, ypos + 3, wid, hei, 3);
    rgb(ctx, lit ? COL_ACCENT_DARK : COL_CAP_SIDE);
    cairo_fill(ctx);
    rrect(ctx, xpos, ypos + drop, wid, hei, 3);
    gradient(ctx, ypos + drop, hei, top, bot);
    cairo_fill_preserve(ctx);
    rgb(ctx, lit ? COL_WHITE : COL_CAP_EDGE);
    cairo_set_line_width(ctx, lit ? 2 : 1);
    cairo_stroke(ctx);

    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0]) {
        legend(ctx, widget, ctl->label, 10, TRUE, ink, xpos, ypos + drop + 2, wid, hei * 0.5);
        legend(ctx, widget, bound, 9, FALSE, mix(ink, top, 0.25), xpos,
               ypos + drop + hei * 0.5, wid, hei * 0.5 - 2);
    } else {
        legend(ctx, widget, ctl->label, 10, TRUE, ink, xpos, ypos + drop, wid, hei);
    }
    dim_end(ctx, state.dim);
}

/* The recess behind a pad, with "Player N" on it (the caption is the top of
 * that block). */
static void draw_caption(cairo_t *ctx, GtkWidget *widget, const sms_control *ctl)
{
    const double xpos = px_x(ctl->left), ypos = px_y(ctl->top);

    rrect(ctx, xpos - 6, ypos - 4, ctl->width * UNIT + 12,
          (g_lay_hei - ctl->top) * UNIT + 4 + 6, 10);
    rgb(ctx, COL_RECESS);
    cairo_fill(ctx);
    legend(ctx, widget, ctl->label, 12, TRUE, COL_CAPTION, xpos, ypos, ctl->width * UNIT,
           ctl->height * UNIT);
}

/* "SEGA" letter-spaced and centred in the pad's mark box. */
static void pad_mark(cairo_t *ctx, GtkWidget *widget, double xpos, double ypos,
                     double boxw, double boxh)
{
    PangoLayout *lay = gtk_widget_create_pango_layout(widget, NULL);
    PangoFontDescription *fontd = pango_font_description_new();
    char *markup = g_strdup_printf("<span letter_spacing=\"%d\">SEGA</span>",
                                   SMS_PAD_MARK_SPACING * PANGO_SCALE);
    int txtw, txth;

    pango_font_description_set_family(fontd, "Sans");
    pango_font_description_set_weight(fontd, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(fontd, SMS_PAD_MARK_PX * PANGO_SCALE);
    pango_layout_set_font_description(lay, fontd);
    pango_layout_set_markup(lay, markup, -1);
    pango_layout_set_width(lay, (int)(boxw * PANGO_SCALE));
    pango_layout_set_alignment(lay, PANGO_ALIGN_CENTER);
    pango_layout_get_pixel_size(lay, &txtw, &txth);
    rgb(ctx, COL_PAD_MARK);
    cairo_move_to(ctx, xpos, ypos + (boxh - txth) / 2.0);
    pango_cairo_show_layout(ctx, lay);
    cairo_new_path(ctx);
    pango_font_description_free(fontd);
    g_free(markup);
    g_object_unref(lay);
}

/* The pad's black body with its SEGA mark. */
static void draw_pad(cairo_t *ctx, GtkWidget *widget, const sms_control *ctl)
{
    const double xpos = px_x(ctl->left), ypos = px_y(ctl->top);
    const double wid = ctl->width * UNIT, hei = ctl->height * UNIT, rad = 0.7 * UNIT;

    rrect(ctx, xpos, ypos, wid, hei, rad);
    gradient(ctx, ypos, hei, COL_PAD_TOP, COL_PAD_BOT);
    cairo_fill_preserve(ctx);
    rgb(ctx, COL_PAD_EDGE);
    cairo_set_line_width(ctx, 1);
    cairo_stroke(ctx);
    cairo_move_to(ctx, xpos + rad, ypos + 1.5);
    cairo_line_to(ctx, xpos + wid - rad, ypos + 1.5);
    rgb(ctx, COL_PAD_HIGH);
    cairo_set_line_width(ctx, 1.5);
    cairo_stroke(ctx);
    pad_mark(ctx, widget, xpos + SMS_PAD_MARK_LEFT * UNIT, ypos + SMS_PAD_MARK_TOP * UNIT,
             SMS_PAD_MARK_WIDTH * UNIT, SMS_PAD_MARK_HEIGHT * UNIT);
}

/* The D-pad's dish and the cross in it. The arms are drawn over the cross
 * (draw_arm) when held, picked or labelled; the ring lights while a direction
 * is held. */
static void draw_dpad(cairo_t *ctx, const sms_control *ctl)
{
    const gboolean armed = g_map_state != -2;
    const double rad = DISH_RADIUS * UNIT;
    const double half = ARM_HALF * UNIT, reach = ARM_REACH * UNIT;
    double centx, centy;
    const gboolean lit = ctl->port >= 0 && ctl->port < SMS_PLAYERS &&
                         (g_held[ctl->port] & DIR_MASK) != 0;

    dish_centre(ctl, &centx, &centy);
    cairo_arc(ctx, centx, centy, rad, 0, 2 * G_PI);
    rgb(ctx, COL_DISH);
    cairo_fill_preserve(ctx);
    rgb(ctx, lit ? COL_ACCENT : COL_DISH_RING);
    cairo_set_line_width(ctx, lit ? 3 : 1.5);
    cairo_stroke(ctx);

    dim_begin(ctx, armed);
    /* the cross as one outline: the four arms and the centre cell */
    cairo_move_to(ctx, centx - half, centy - reach);
    cairo_line_to(ctx, centx + half, centy - reach);
    cairo_line_to(ctx, centx + half, centy - half);
    cairo_line_to(ctx, centx + reach, centy - half);
    cairo_line_to(ctx, centx + reach, centy + half);
    cairo_line_to(ctx, centx + half, centy + half);
    cairo_line_to(ctx, centx + half, centy + reach);
    cairo_line_to(ctx, centx - half, centy + reach);
    cairo_line_to(ctx, centx - half, centy + half);
    cairo_line_to(ctx, centx - reach, centy + half);
    cairo_line_to(ctx, centx - reach, centy - half);
    cairo_line_to(ctx, centx - half, centy - half);
    cairo_close_path(ctx);
    gradient(ctx, centy - reach, 2 * reach, COL_ARM_TOP, COL_ARM_BOT);
    cairo_fill_preserve(ctx);
    rgb(ctx, COL_ARM_EDGE);
    cairo_set_line_width(ctx, 1.5);
    cairo_set_line_join(ctx, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(ctx);
    /* the lit top edges */
    rgb(ctx, COL_ARM_HIGH);
    cairo_set_line_width(ctx, 1);
    cairo_move_to(ctx, centx - half + 2, centy - reach + 1.5);
    cairo_line_to(ctx, centx + half - 2, centy - reach + 1.5);
    cairo_move_to(ctx, centx - reach + 2, centy - half + 1.5);
    cairo_line_to(ctx, centx - half - 2, centy - half + 1.5);
    cairo_move_to(ctx, centx + half + 2, centy - half + 1.5);
    cairo_line_to(ctx, centx + reach - 2, centy - half + 1.5);
    cairo_stroke(ctx);
    /* the dimple in the middle, and the thumb-stick's screw hole */
    cairo_arc(ctx, centx, centy, 0.35 * UNIT, 0, 2 * G_PI);
    rgb(ctx, COL_DISH);
    cairo_fill(ctx);
    cairo_arc(ctx, centx, centy, 0.12 * UNIT, 0, 2 * G_PI);
    rgb(ctx, COL_ARM_EDGE);
    cairo_fill(ctx);
    dim_end(ctx, armed);
}

/* One cross arm over the cross: the accent when held or picked, lightened
 * under the pointer, and a triangle pointing its way (or the key bound to it). */
static void draw_arm(cairo_t *ctx, GtkWidget *widget, int idx)
{
    const sms_control *ctl = &g_layout[idx];
    const PartState state = part_state(idx);
    const double xpos = px_x(ctl->left), ypos = px_y(ctl->top);
    const double wid = ctl->width * UNIT, hei = ctl->height * UNIT;
    const double midx = xpos + wid / 2, midy = ypos + hei / 2;
    const gboolean lit = state.held || state.selected;
    double dirx = 0, diry = 0;
    char bound[40];

    if (ctl->act_mask == (1u << SMS_ACT_UP)) diry = -1;
    else if (ctl->act_mask == (1u << SMS_ACT_DOWN)) diry = 1;
    else if (ctl->act_mask == (1u << SMS_ACT_LEFT)) dirx = -1;
    else dirx = 1;

    dim_begin(ctx, state.dim);
    if (lit) {
        rrect(ctx, xpos + 1, ypos + 1, wid - 2, hei - 2, 4);
        rgb(ctx, COL_ACCENT);
        cairo_fill_preserve(ctx);
        rgb(ctx, COL_WHITE);
        cairo_set_line_width(ctx, 2);
        cairo_stroke(ctx);
    } else if (state.hot) {
        rrect(ctx, xpos + 1, ypos + 1, wid - 2, hei - 2, 4);
        cairo_set_source_rgba(ctx, 1, 1, 1, 0.12);
        cairo_fill(ctx);
    }
    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0]) {
        legend(ctx, widget, bound, 11, TRUE, lit ? COL_WHITE : COL_CAP_INK, xpos, ypos, wid,
               hei);
    } else {
        /* the triangle on the arm, pointing outward */
        cairo_move_to(ctx, midx + dirx * 8, midy + diry * 8);
        cairo_line_to(ctx, midx - dirx * 5 - diry * 7, midy - diry * 5 + dirx * 7);
        cairo_line_to(ctx, midx - dirx * 5 + diry * 7, midy - diry * 5 - dirx * 7);
        cairo_close_path(ctx);
        rgb(ctx, lit ? COL_WHITE : COL_ARM_MARK);
        cairo_fill(ctx);
    }
    dim_end(ctx, state.dim);
}

/* Button 1 or 2: a black rounded square with its numeral; down 2 px and lit
 * while held. */
static void draw_button(cairo_t *ctx, GtkWidget *widget, int idx)
{
    const sms_control *ctl = &g_layout[idx];
    const PartState state = part_state(idx);
    const double xpos = px_x(ctl->left), ypos = px_y(ctl->top);
    const double wid = ctl->width * UNIT, hei = ctl->height * UNIT;
    const gboolean lit = state.held || state.selected;
    const double drop = state.held ? 2 : 0;
    guint32 top = COL_KEY_TOP, bot = COL_KEY_BOT, ink = COL_KEY_INK;
    char bound[40];
    double numh = hei;

    if (lit) {
        top = bot = COL_ACCENT;
        ink = COL_WHITE;
    } else if (state.hot) {
        top = mix(top, COL_WHITE, 0.12);
        bot = mix(bot, COL_WHITE, 0.12);
    }
    dim_begin(ctx, state.dim);
    rrect(ctx, xpos, ypos + 3, wid, hei - 3, 0.28 * UNIT);
    rgb(ctx, lit ? COL_ACCENT_DARK : COL_CAP_SIDE);
    cairo_fill(ctx);
    rrect(ctx, xpos, ypos + drop, wid, hei - 3, 0.28 * UNIT);
    gradient(ctx, ypos + drop, hei - 3, top, bot);
    cairo_fill_preserve(ctx);
    rgb(ctx, lit ? COL_WHITE : COL_ARM_EDGE);
    cairo_set_line_width(ctx, lit ? 2 : 1);
    cairo_stroke(ctx);

    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0]) {
        numh = hei * 0.62;
        legend(ctx, widget, bound, 9, FALSE, mix(ink, top, 0.25), xpos,
               ypos + drop + numh - 4, wid, hei - numh - 3);
    }
    legend(ctx, widget, ctl->label, 20, TRUE, ink, xpos, ypos + drop, wid, numh - 3);
    dim_end(ctx, state.dim);
}

static void draw_console(GtkDrawingArea *area, cairo_t *ctx, int wid, int hei, gpointer data)
{
    GtkWidget *widget = GTK_WIDGET(area);
    int idx;
    (void)data;

    draw_case(ctx, wid, hei);
    if (!g_layout)
        return;
    /* the layout is in paint order: strip, console buttons, then each pad's
     * block with its cross arms and buttons last */
    for (idx = 0; idx < g_part_count; idx++) {
        const sms_control *ctl = &g_layout[idx];
        switch (ctl->kind) {
        case SMS_CTL_CONSOLE:   draw_head(ctx, widget, ctl); break;
        case SMS_CTL_SWITCH:    draw_cap(ctx, widget, idx); break;
        case SMS_CTL_CAPTION:   draw_caption(ctx, widget, ctl); break;
        case SMS_CTL_PAD:       draw_pad(ctx, widget, ctl); break;
        case SMS_CTL_DPAD:      draw_dpad(ctx, ctl); break;
        case SMS_CTL_DIRECTION: draw_arm(ctx, widget, idx); break;
        case SMS_CTL_BUTTON:    draw_button(ctx, widget, idx); break;
        default: break;
        }
    }
}

/* The system actions under the case. */
static void draw_strip(GtkDrawingArea *area, cairo_t *ctx, int wid, int hei, gpointer data)
{
    Strip *strip = data;
    int idx;
    (void)wid; (void)hei;

    for (idx = 0; idx < strip->count; idx++) {
        const StripItem *item = &strip->items[idx];
        const gboolean held = strip->pressed == idx;
        const gboolean sel = g_map_state >= 0 && g_map_state == item->target;
        const gboolean dim = g_map_state >= 0 && !sel;
        guint32 face = held || sel ? COL_ACCENT : strip->hover == idx ? 0x4A4A52 : COL_CAP_TOP;
        char bound[40], text[120];

        dim_begin(ctx, dim);
        rrect(ctx, item->xpos, item->ypos + 3, item->wid, item->hei - 3, 4);
        rgb(ctx, held || sel ? COL_ACCENT_DARK : COL_CAP_SIDE);
        cairo_fill(ctx);
        rrect(ctx, item->xpos, item->ypos, item->wid, item->hei - 3, 4);
        rgb(ctx, face);
        cairo_fill(ctx);
        bound_text(item->target, bound, sizeof bound);
        if (bound[0])
            g_snprintf(text, sizeof text, "%s  \xc2\xb7  %s", item->label, bound);
        else
            g_snprintf(text, sizeof text, "%s", item->label);
        legend(ctx, GTK_WIDGET(area), text, 12, FALSE, held || sel ? COL_WHITE : COL_CAP_INK,
               item->xpos + 4, item->ypos, item->wid - 8, item->hei - 3);
        dim_end(ctx, dim);
    }
}

/* ---- pointer ---------------------------------------------------------------------- */

static int console_hit(double xpos, double ypos)
{
    return smssession_controls_hit(g_console, (float)((xpos - CASE_PAD) / UNIT),
                                   (float)((ypos - CASE_PAD) / UNIT));
}

static void run_sysaction(int sysact)
{
    if (g_parent)
        sms_window_run_sysaction(SMS_WINDOW(g_parent), sysact);
    else
        smssession_sysaction(g_session, sysact);
}

/* Lets go of everything the mouse holds on the console. */
static void mouse_release(void)
{
    int pressed = g_pressed;

    if (g_drag.active) {
        smssession_pointer_set(g_session, g_drag.port, 0);
        g_drag.active = FALSE;
    }
    g_pressed = -1;
    if (pressed >= 0 && g_layout && pressed < g_part_count) {
        const sms_control *ctl = &g_layout[pressed];
        if (ctl->kind == SMS_CTL_SWITCH)
            smssession_press(g_session, ctl->target, 0);
        else if (ctl->port >= 0)
            smssession_pointer_set(g_session, ctl->port, 0);
    }
    if (g_strip.pressed >= 0) {
        g_strip.pressed = -1;
        if (g_strip.area) gtk_widget_queue_draw(g_strip.area);
    }
    if (g_area) gtk_widget_queue_draw(g_area);
}

/* Everything this window holds down: the mouse's and the typed keys'. */
static void let_go(void)
{
    mouse_release();
    smssession_release_all(g_session);
    memset(g_sysact_down, 0, sizeof g_sysact_down);
    g_swallow_code = 0;     /* its release went elsewhere */
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    smssession_gamepad_capture_cancel(g_session);
}

static void say_bound(const char *what, const char *stolen)
{
    char msg[220];
    if (stolen[0])
        g_snprintf(msg, sizeof msg, "Bound %s (was %s)", what, stolen);
    else
        g_snprintf(msg, sizeof msg, "Bound %s", what);
    gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
}

static void set_map_state(int state);

static gboolean capture_tick(gpointer data)
{
    int button;
    (void)data;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (smssession_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        smssession_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        g_capture_timer = 0;
        set_map_state(-1);     /* stay armed: remapping several in a row is normal */
        say_bound(sms_pad_button_name(button), stolen);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    gtk_widget_set_sensitive(g_clear_button, state >= 0);
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "sms-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Hold a control with the mouse. Map rebinds one.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "sms-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a control to remap it");
    } else {
        char msg[160];
        mouse_release();
        g_snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s (Esc cancels)",
                   sms_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        smssession_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_all();
}

/* The directions under a drag on the dish: the pointer's vector from its centre. */
static void drag_follow(double xpos, double ypos)
{
    const unsigned mask = smssession_dpad_from_vector((float)(xpos - g_drag.centx),
                                                      (float)(ypos - g_drag.centy),
                                                      (float)(DISH_RADIUS * UNIT));
    smssession_pointer_set(g_session, g_drag.port, mask);
}

static void on_console_begin(GtkGestureDrag *gesture, double xpos, double ypos, gpointer udata)
{
    const int idx = console_hit(xpos, ypos);
    const sms_control *ctl;
    (void)gesture; (void)udata;

    if (idx < 0)
        return;
    ctl = &g_layout[idx];
    if (g_map_state != -2) {
        /* armed: picks (or changes) the target; the dish is no single control */
        if (ctl->target >= 0)
            set_map_state(ctl->target);
        return;
    }
    mouse_release();
    switch (ctl->kind) {
    case SMS_CTL_DPAD:
        g_drag.active = TRUE;
        g_drag.port = ctl->port;
        dish_centre(ctl, &g_drag.centx, &g_drag.centy);
        drag_follow(xpos, ypos);
        break;
    case SMS_CTL_DIRECTION:
    case SMS_CTL_BUTTON:
        g_pressed = idx;
        smssession_pointer_set(g_session, ctl->port, ctl->act_mask);
        break;
    case SMS_CTL_SWITCH:
        g_pressed = idx;
        smssession_press(g_session, ctl->target, 1);
        break;
    default:
        return;
    }
    gtk_widget_queue_draw(g_area);
}

static void on_console_update(GtkGestureDrag *gesture, double offx, double offy, gpointer udata)
{
    double startx, starty;
    (void)udata;
    if (!g_drag.active) return;
    gtk_gesture_drag_get_start_point(gesture, &startx, &starty);
    drag_follow(startx + offx, starty + offy);
}

static void on_console_end(GtkGestureDrag *gesture, double offx, double offy, gpointer udata)
{
    (void)gesture; (void)offx; (void)offy; (void)udata;
    mouse_release();
}

static void on_console_cancel(GtkGesture *gesture, GdkEventSequence *seq, gpointer udata)
{
    (void)gesture; (void)seq; (void)udata;
    mouse_release();
}

/* Dragging off a held arm, button or cap lets it go. */
static void on_console_motion(GtkEventControllerMotion *ctrl, double xpos, double ypos,
                              gpointer udata)
{
    const int idx = console_hit(xpos, ypos);
    (void)ctrl; (void)udata;
    if (g_pressed >= 0 && idx != g_pressed)
        mouse_release();
    if (idx != g_hover) {
        g_hover = idx;
        gtk_widget_queue_draw(g_area);
    }
}

static void on_console_leave(GtkEventControllerMotion *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    if (g_pressed >= 0)
        mouse_release();
    if (g_hover >= 0) {
        g_hover = -1;
        gtk_widget_queue_draw(g_area);
    }
}

static gboolean on_console_tooltip(GtkWidget *widget, int xpos, int ypos, gboolean kbd,
                                   GtkTooltip *tip, gpointer udata)
{
    const int idx = kbd ? -1 : console_hit(xpos, ypos);
    const sms_control *ctl;
    char text[300];
    (void)widget; (void)udata;
    if (idx < 0) return FALSE;
    ctl = &g_layout[idx];
    if (ctl->target >= 0)
        target_tip(ctl->target, text, sizeof text);
    else if (ctl->kind == SMS_CTL_DPAD)
        g_snprintf(text, sizeof text, "Drag to hold a direction, or two for a diagonal");
    else
        return FALSE;
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

/* ---- the strip ------------------------------------------------------------------------ */

static int strip_hit(const Strip *strip, double xpos, double ypos)
{
    int idx;
    for (idx = strip->count - 1; idx >= 0; idx--)
        if (xpos >= strip->items[idx].xpos &&
            xpos < strip->items[idx].xpos + strip->items[idx].wid &&
            ypos >= strip->items[idx].ypos &&
            ypos < strip->items[idx].ypos + strip->items[idx].hei)
            return idx;
    return -1;
}

static void strip_release(gboolean activate)
{
    const int idx = g_strip.pressed;
    if (idx < 0) return;
    g_strip.pressed = -1;
    /* a system action fires on release, like a real button: pressing and
     * dragging off should not power-cycle the cartridge */
    if (activate && is_sysact(g_strip.items[idx].target))
        run_sysaction(g_strip.items[idx].target - SMS_TARGET_SYSACT(0));
    gtk_widget_queue_draw(g_strip.area);
}

static void on_strip_begin(GtkGestureDrag *gesture, double xpos, double ypos, gpointer udata)
{
    const int idx = strip_hit(&g_strip, xpos, ypos);
    (void)gesture; (void)udata;
    if (idx < 0) return;
    if (g_map_state != -2) {
        set_map_state(g_strip.items[idx].target);
        return;
    }
    g_strip.pressed = idx;
    gtk_widget_queue_draw(g_strip.area);
}

static void on_strip_end(GtkGestureDrag *gesture, double offx, double offy, gpointer udata)
{
    (void)gesture; (void)offx; (void)offy; (void)udata;
    strip_release(g_strip.hover == g_strip.pressed);
}

static void on_strip_cancel(GtkGesture *gesture, GdkEventSequence *seq, gpointer udata)
{
    (void)gesture; (void)seq; (void)udata;
    strip_release(FALSE);
}

static void on_strip_motion(GtkEventControllerMotion *ctrl, double xpos, double ypos,
                            gpointer udata)
{
    const int idx = strip_hit(&g_strip, xpos, ypos);
    (void)ctrl; (void)udata;
    if (idx != g_strip.hover) {
        g_strip.hover = idx;
        gtk_widget_queue_draw(g_strip.area);
    }
}

static void on_strip_leave(GtkEventControllerMotion *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    g_strip.hover = -1;
    gtk_widget_queue_draw(g_strip.area);
}

static gboolean on_strip_tooltip(GtkWidget *widget, int xpos, int ypos, gboolean kbd,
                                 GtkTooltip *tip, gpointer udata)
{
    const int idx = kbd ? -1 : strip_hit(&g_strip, xpos, ypos);
    char text[260];
    (void)widget; (void)udata;
    if (idx < 0) return FALSE;
    target_tip(g_strip.items[idx].target, text, sizeof text);
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

static void build_strip(void)
{
    static const struct { int target; const char *label; } items[STRIP_COUNT] = {
        { SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG), "Reset to CONFIG" },
        { SMS_TARGET_SYSACT(SMS_SYSACT_SOFT_RESET), "Soft Reset" },
        { SMS_TARGET_SYSACT(SMS_SYSACT_DEBUG_STOP), "Stop in Debugger" },
    };
    int idx;

    g_strip.count = STRIP_COUNT;
    g_strip.pressed = g_strip.hover = -1;
    for (idx = 0; idx < STRIP_COUNT; idx++)
        g_strip.items[idx] = (StripItem){ idx * (STRIP_BW + STRIP_GAP), 0, STRIP_BW, STRIP_H,
                                          items[idx].target, items[idx].label };
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_strip.area),
                                       STRIP_COUNT * STRIP_BW + (STRIP_COUNT - 1) * STRIP_GAP);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_strip.area), STRIP_H);
}

/* ---- refresh --------------------------------------------------------------------------- */

static void refresh_all(void)
{
    if (g_area) gtk_widget_queue_draw(g_area);
    if (g_strip.area) gtk_widget_queue_draw(g_strip.area);
}

/* The layout of the console that is running. The extent is the same for every
 * console, so the window never resizes; RESET, RAPID and the wordmark change. */
static void load_layout(int console)
{
    mouse_release();
    g_console = console;
    g_layout = smssession_controls_layout(console, &g_part_count, &g_lay_wid, &g_lay_hei);
    if (g_part_count > MAX_PARTS) g_part_count = MAX_PARTS;
    g_hover = -1;
    memset(g_lit, 0, sizeof g_lit);
    if (g_title)
        adw_window_title_set_subtitle(g_title, sms_console_name(console));
    refresh_all();
}

/* While the window is on screen: what the console sees held, painted in the
 * accent. Redraws only when something changed. */
static gboolean held_tick(gpointer data)
{
    gboolean changed = FALSE;
    int idx, port;
    (void)data;

    for (port = 0; port < SMS_PLAYERS; port++) {
        const unsigned now = smssession_buttons_held(g_session, port);
        if (now != g_held[port]) {
            g_held[port] = now;
            changed = TRUE;
        }
    }
    for (idx = 0; idx < g_part_count; idx++) {
        const gboolean now = part_held(&g_layout[idx]);
        if (now != g_lit[idx]) {
            g_lit[idx] = now;
            changed = TRUE;
        }
    }
    if (changed)
        gtk_widget_queue_draw(g_area);
    return G_SOURCE_CONTINUE;
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int idx;
    for (idx = 0; idx < g_pad_row_count; idx++) {
        const int eff = smssession_gamepad_effective_port(g_session, idx);
        char text[32];
        if (eff >= 0) g_snprintf(text, sizeof text, "drives player %d", eff + 1);
        else g_snprintf(text, sizeof text, "drives nothing");
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_eff[idx])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_eff[idx]), text);
    }
}

static void on_assign_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    const int idx = GPOINTER_TO_INT(udata);
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    (void)pspec;
    /* 0 = automatic (connection order), 1-2 = that player */
    smssession_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name, the player it drives and the one it is set to.
 * The area is PAD_ROWS rows tall whatever is plugged in (more pads scroll),
 * so the window never changes size. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Player 1", "Player 2", NULL };
    GtkWidget *child;
    int count, idx;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_pad_row_count = 0;

    count = smssession_gamepad_count(g_session);
    if (count == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_widget_set_size_request(none, -1, PAD_ROW_H);
        gtk_widget_set_tooltip_text(none, "Plug one in: it is picked up as it appears");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PADS; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *drop, *eff;
        char text[128];
        const int assign = smssession_gamepad_assignment(g_session, idx);

        smssession_gamepad_name(g_session, idx, text, sizeof text);
        gtk_widget_set_size_request(row, -1, PAD_ROW_H);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        drop = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drop),
                                   assign < 0 || assign >= SMS_PLAYERS ? 0 : (guint)assign + 1);
        gtk_widget_set_focus_on_click(drop, FALSE);
        gtk_widget_set_valign(drop, GTK_ALIGN_CENTER);
        g_signal_connect(drop, "notify::selected", G_CALLBACK(on_assign_changed),
                         GINT_TO_POINTER(idx));
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), drop);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        g_pad_eff[g_pad_row_count++] = eff;
    }
    refresh_pad_ports();
}

/* The pad rows (hot plug, automatic assignment changes) and the console (a
 * power cycle out of Preferences can change it under an open window). */
static gboolean info_tick(gpointer data)
{
    unsigned gen;
    (void)data;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;

    gen = smssession_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    if (smssession_console(g_session) != g_console)
        load_layout(smssession_console(g_session));
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key(guint keyval, guint code, GdkModifierType state, gboolean down)
{
    /* an open popover (a gamepad's player) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    guint32 keysym;
    int sysact;

    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    if (g_swallow_code && code == g_swallow_code) {
        if (!down) g_swallow_code = 0;
        return TRUE;
    }
    /* The window's own keys and the shortcuts are never Map's. */
    if (g_map_state != -2 && keyval != GDK_KEY_F9 && keyval != GDK_KEY_F11 &&
        keyval != GDK_KEY_F12 && !(state & GDK_CONTROL_MASK)) {
        char stolen[128], name[32];
        if (!down) {
            /* a key held before Map armed must still let go */
            keysym = code >= 8 ? smssession_keysym_from_evdev(code - 8) : 0;
            if (!keysym) keysym = keyval;
            smssession_key(g_session, keysym, 0);
            return TRUE;
        }
        if (keyval == GDK_KEY_Escape) {
            g_swallow_code = code;
            set_map_state(g_map_state >= 0 ? -1 : -2);
            return TRUE;
        }
        if (g_map_state < 0)
            return TRUE;          /* armed, no target yet: typing is not typed */
        keysym = code >= 8 ? smssession_keysym_from_evdev(code - 8) : 0;
        if (!keysym) keysym = keyval;
        smssession_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
        smssession_keysym_name(keysym, name, sizeof name);
        g_swallow_code = code;
        set_map_state(-1);        /* stay armed: remapping several in a row is normal */
        say_bound(name, stolen);
        return TRUE;
    }

    /* Otherwise exactly what the main window does with it, so playing works
     * whichever window has focus. */
    keysym = code >= 8 ? smssession_keysym_from_evdev(code - 8) : 0;
    if (!keysym) keysym = keyval;
    if (!down) {
        if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12)
            return TRUE;
        sysact = smssession_key_sysaction(g_session, keysym);
        if (sysact >= 0) {
            g_sysact_down[sysact] = FALSE;
            return TRUE;
        }
        return smssession_key(g_session, keysym, 0) ? TRUE : FALSE;
    }
    if (keyval == GDK_KEY_F9) {
        sms_controllers_window_toggle(NULL, g_session);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) || (state & GDK_ALT_MASK))
        return FALSE;
    /* A system action first, on its leading edge only (GTK4 has no repeat
     * flag). */
    sysact = smssession_key_sysaction(g_session, keysym);
    if (sysact >= 0) {
        if (!g_sysact_down[sysact]) {
            g_sysact_down[sysact] = TRUE;
            run_sysaction(sysact);
        }
        return TRUE;
    }
    return smssession_key(g_session, keysym, 1) ? TRUE : FALSE;
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint code, GdkModifierType state, gpointer udata)
{
    (void)ctrl; (void)udata;
    return on_key(keyval, code, state, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                            guint code, GdkModifierType state, gpointer udata)
{
    (void)ctrl; (void)udata;
    on_key(keyval, code, state, FALSE);
}

/* Keys held into a focus change must not stay down, nor the mouse's holds. */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    let_go();
}

static void on_active_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    (void)pspec; (void)udata;
    if (!gtk_window_is_active(GTK_WINDOW(obj)))
        let_go();
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    smssession_bindings_reset(g_session);
    gtk_label_set_text(GTK_LABEL(g_map_hint), "Bindings back to their defaults");
    refresh_all();
}

/* Takes the picked control's key and gamepad button away. */
static void on_clear_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    if (g_map_state < 0) return;
    smssession_binding_set_key(g_session, g_map_state, 0, NULL, 0);
    smssession_binding_set_button(g_session, g_map_state, SMS_PAD_BTN_NONE, NULL, 0);
    gtk_label_set_text(GTK_LABEL(g_map_hint),
                       "Cleared. Press a key or a gamepad button to bind it again");
    refresh_all();
}

static void on_show_bindings(GtkCheckButton *button, gpointer udata)
{
    (void)udata;
    smssession_set_int(g_session, "controllers_show_bindings",
                       gtk_check_button_get_active(button) ? 1 : 0);
    refresh_all();
}

static void hide_window(void)
{
    set_map_state(-2);
    let_go();
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *win, gpointer udata)
{
    (void)win; (void)udata;
    hide_window();
    return TRUE;
}

/* The timers run only while the window is on screen: closing only hides it,
 * and a hidden window has nobody to update. */
static void on_window_map(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (!g_info_timer) g_info_timer = g_timeout_add(250, info_tick, NULL);
    if (!g_held_timer) g_held_timer = g_timeout_add(33, held_tick, NULL);
    info_tick(NULL);
    held_tick(NULL);
}

static void on_window_unmap(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_info_timer) g_source_remove(g_info_timer);
    if (g_held_timer) g_source_remove(g_held_timer);
    g_info_timer = g_held_timer = 0;
}

static void on_destroy(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_info_timer) g_source_remove(g_info_timer);
    if (g_held_timer) g_source_remove(g_held_timer);
    g_capture_timer = g_info_timer = g_held_timer = 0;
    memset(&g_strip, 0, sizeof g_strip);
    g_area = NULL;
    g_window = NULL;
    g_pad_box = NULL;
    g_title = NULL;
    g_show_bindings = NULL;
    g_pad_row_count = 0;
    g_console = -1;
    g_layout = NULL;
    g_map_state = -2;
    g_hover = g_pressed = -1;
    g_drag.active = FALSE;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *maprow, *toolbar, *header, *defaults, *padscroll;
    GtkEventController *keys, *focus, *motion;
    GtkGesture *drag;

    sms_install_accent_css();
    memset(g_held, 0, sizeof g_held);
    g_hover = g_pressed = -1;
    g_title = NULL;
    load_layout(smssession_console(g_session));

    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    /* min == max: Wayland compositors float it */
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    /* the console */
    g_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_area), area_width());
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_area), area_height());
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_area), draw_console, NULL, NULL);
    gtk_widget_set_halign(g_area, GTK_ALIGN_CENTER);
    /* Not focusable: clicking a control must not steal focus. */
    gtk_widget_set_focusable(g_area, FALSE);
    gtk_widget_set_has_tooltip(g_area, TRUE);
    g_signal_connect(g_area, "query-tooltip", G_CALLBACK(on_console_tooltip), NULL);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_console_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_console_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_console_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_console_cancel), NULL);
    gtk_widget_add_controller(g_area, GTK_EVENT_CONTROLLER(drag));
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_console_motion), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_console_leave), NULL);
    gtk_widget_add_controller(g_area, motion);

    /* the system actions */
    memset(&g_strip, 0, sizeof g_strip);
    g_strip.area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_strip.area), draw_strip, &g_strip, NULL);
    gtk_widget_set_halign(g_strip.area, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(g_strip.area, FALSE);
    gtk_widget_set_has_tooltip(g_strip.area, TRUE);
    g_signal_connect(g_strip.area, "query-tooltip", G_CALLBACK(on_strip_tooltip), NULL);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_strip_begin), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_strip_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_strip_cancel), NULL);
    gtk_widget_add_controller(g_strip.area, GTK_EVENT_CONTROLLER(drag));
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_strip_motion), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_strip_leave), NULL);
    gtk_widget_add_controller(g_strip.area, motion);
    build_strip();

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_box_append(GTK_BOX(root), g_area);
    gtk_box_append(GTK_BOX(root), g_strip.area);

    /* the pad rows: a fixed-height area, so the window never resizes */
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PAD_GAP);
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);
    g_pad_generation = (unsigned)-1;
    refresh_pads();

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    g_clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_focus_on_click(g_clear_button, FALSE);
    gtk_widget_set_tooltip_text(g_clear_button,
                                "Take the picked control's key and gamepad button away");
    g_signal_connect(g_clear_button, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_show_bindings = gtk_check_button_new_with_label("Show bindings");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_bindings), show_bindings_saved());
    gtk_widget_set_focus_on_click(g_show_bindings, FALSE);
    g_signal_connect(g_show_bindings, "toggled", G_CALLBACK(on_show_bindings), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_show_bindings);
    gtk_box_append(GTK_BOX(root), maprow);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    /* a long hint ("Press a key or a gamepad button for Player 1: Right")
     * must not widen the window */
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    gtk_box_append(GTK_BOX(root), g_map_hint);

    header = adw_header_bar_new();
    g_title = ADW_WINDOW_TITLE(adw_window_title_new("Controllers",
                                                    sms_console_name(g_console)));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(g_title));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), sms_help_button("controllers"));
    sms_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    set_map_state(-2);
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
}

static void ensure_window(GtkWindow *parent, smssession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);
}

void sms_controllers_window_show(GtkWindow *parent, smssession *session)
{
    ensure_window(parent, session);
    gtk_window_present(GTK_WINDOW(g_window));
}

void sms_controllers_window_toggle(GtkWindow *parent, smssession *session)
{
    ensure_window(parent, session);
    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        gtk_window_present(GTK_WINDOW(g_window));
}

gboolean sms_controllers_window_is_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
