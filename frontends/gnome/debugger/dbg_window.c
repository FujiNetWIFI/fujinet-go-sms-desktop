/*
 * Debugger window (GTK4/libadwaita) over the session's Z80 / 315-5124
 * debugger engine, via core/include/smsdebug.h.
 *
 * Tabs: a Prompt (the contract's command set, with tab completion and a
 * command history), the Z80 + console RAM, the disassembly with a
 * breakpoint gutter, the VDP (registers, the name table / tile / sprite /
 * palette views, the sprite table and the CRAM), the sound chips and I/O
 * ports, breakpoints, and the FujiNet cartridge. A toolbar carries the
 * stepping controls, symbol loading and file saves; the family's keys apply
 * (F5 run/stop, F7 step, F8 step over, Shift+F8 step out, F12 close).
 *
 * The engine exists only while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling), hiding it detaches and
 * lets the machine run on.
 *
 * The window polls the engine's generation counter on a short timer and
 * refreshes only when something changed, so a stopped machine costs
 * nothing and a running one shows live values twice a second. Every
 * smsdebug call waits for the frame in progress (the host's run lock), so
 * looking at a running machine is safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "smsdebug.h"
#include "../window.h"

#define DISASM_WINDOW 48
#define NREGS 17
#define RAM_BASE 0xC000
#define RAM_SIZE 8192

typedef struct {
    GtkWindow *win;
    smssession *session;
    smsdebug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    int running_ticks;
    guint timer;

    AdwWindowTitle *title;    /* "Debugger", with the stop reason under it */
    GtkButton *run_btn;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;

    /* prompt */
    GtkTextView *prompt_out;
    GtkEntry *prompt_in;
    GtkScrolledWindow *prompt_scroll;
    GPtrArray *history;
    guint history_pos;

    /* cpu + ram */
    GtkEntry *reg[NREGS];     /* PC SP AF BC DE HL IX IY AF' BC' DE' HL' I R IM IFF1 IFF2 */
    GtkCheckButton *flag[6];  /* S Z H P/V N C */
    GtkLabel *beam;
    GtkTextView *ram_view;
    GtkEntry *ram_goto, *ram_addr, *ram_val;
    int ram_mark_row;         /* the row Go to found, highlighted; -1 none */
    gboolean updating_flags;

    /* disassembly */
    GtkTextView *disasm;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_top;           /* address of the first line */
    uint16_t line_addr[DISASM_WINDOW];
    int line_count;

    /* vdp */
    GtkTextView *vdp_text;
    GtkTextView *sprite_view;
    GtkPicture *vdp_pic;
    GtkDropDown *vdp_view;
    GtkDropDown *vdp_palette;
    GtkWidget *cram_swatch[32];
    GtkEntry *cram_entry[32];
    guint32 cram_rgb[32];
    guint32 *view_px, *view_px2;

    /* sound & i/o */
    GtkTextView *io_text;

    /* breakpoints */
    GtkListBox *bp_list;
    GtkDropDown *bp_type;
    GtkEntry *bp_start, *bp_end, *bp_cond;
    GtkLabel *bp_msg;

    /* cart */
    GtkTextView *cart_text;
} DbgWin;

static DbgWin *g_win;

/* The register entries: the Z80's set, the shadows, then the control
 * registers, each with how many hex digits it shows. */
static const struct { const char *name; int reg; int digits; } k_regs[NREGS] = {
    { "PC", SMS_REG_PC, 4 }, { "SP", SMS_REG_SP, 4 }, { "AF", SMS_REG_AF, 4 },
    { "BC", SMS_REG_BC, 4 }, { "DE", SMS_REG_DE, 4 }, { "HL", SMS_REG_HL, 4 },
    { "IX", SMS_REG_IX, 4 }, { "IY", SMS_REG_IY, 4 },
    { "AF'", SMS_REG_AF2, 4 }, { "BC'", SMS_REG_BC2, 4 }, { "DE'", SMS_REG_DE2, 4 },
    { "HL'", SMS_REG_HL2, 4 }, { "I", SMS_REG_I, 2 }, { "R", SMS_REG_R, 2 },
    { "IM", SMS_REG_IM, 1 }, { "IFF1", SMS_REG_IFF1, 1 }, { "IFF2", SMS_REG_IFF2, 1 },
};

/* The tabs, in order, by the names SMS_DEBUGGER_TAB takes. */
static const char *const k_tab_names[] = {
    "prompt", "cpu", "disassembly", "vdp", "sound", "breakpoints", "cart",
};

/* ---- helpers -------------------------------------------------------------- */

/* Replaces a view's text where the reader left it: a refresh (a step, or
 * the live values twice a second) must not throw a scrolled RAM dump or
 * sprite table back to its top. The top line is noted and scrolled back to
 * once the new text is laid out (scroll_to_mark waits for that). */
static void set_text(GtkTextView *view, const char *text)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    GtkAdjustment *adj = GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
    GtkTextIter it;
    int top = 0;

    if (adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &it, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&it);
    }
    gtk_text_buffer_set_text(b, text, -1);
    if (top > 0) {
        GtkTextMark *mark;
        gtk_text_buffer_get_iter_at_line(b, &it, top);
        mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
        gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(b, mark);
    }
}

static void append_text(GtkTextView *view, GtkScrolledWindow *scroll, const char *text)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    GtkAdjustment *adj;
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, text, -1);
    adj = gtk_scrolled_window_get_vadjustment(scroll);
    if (adj) gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

static int parse_num(const char *text, long *out)
{
    char *end;
    long v;
    while (*text == ' ') text++;
    if (*text == '$') v = strtol(text + 1, &end, 16);
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) v = strtol(text + 2, &end, 16);
    else if (*text == '#') v = strtol(text + 1, &end, 10);
    else v = strtol(text, &end, 16);
    if (end == text || *end) return 0;
    *out = v;
    return 1;
}

/* A label or a number, as typed into an address box. */
static int parse_addr(DbgWin *w, const char *text, long *out)
{
    int a = smsdebug_label_address(w->dbg, text);
    if (a >= 0) { *out = a; return 1; }
    return parse_num(text, out);
}

/* The live refresh does not overwrite a value being typed: while the
 * machine runs, a focused entry keeps what is in it. */
static void set_entry(DbgWin *w, GtkEntry *e, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(e));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    if (focus && !smsdebug_is_stopped(w->dbg) &&
        (focus == GTK_WIDGET(e) || gtk_widget_is_ancestor(focus, GTK_WIDGET(e))))
        return;
    gtk_editable_set_text(GTK_EDITABLE(e), text);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean editable)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *padded(GtkWidget *w)
{
    gtk_widget_set_margin_top(w, 8);
    gtk_widget_set_margin_bottom(w, 8);
    gtk_widget_set_margin_start(w, 8);
    gtk_widget_set_margin_end(w, 8);
    return w;
}

static void accent_tag(GtkTextView *view, const char *name)
{
    char rgb[16];
    g_snprintf(rgb, sizeof rgb, "#%06x", SMSSESSION_ACCENT_RGB);
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(view), name,
                               "paragraph-background", rgb, "foreground", "#ffffff", NULL);
}

/* Messages from Load Symbols and Save land in the prompt's transcript and,
 * since the prompt may not be the tab showing, in a toast. */
static void prompt_say(DbgWin *w, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(w->prompt_out, w->prompt_scroll, line);
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_cpu(DbgWin *w)
{
    smsdebug_cpu c;
    char buf[200];
    int i;
    smsdebug_cpu_get(w->dbg, &c);
    {
        const int vals[NREGS] = { c.pc, c.sp, c.af, c.bc, c.de, c.hl, c.ix, c.iy,
                                  c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im,
                                  c.iff1, c.iff2 };
        for (i = 0; i < NREGS; i++) {
            g_snprintf(buf, sizeof buf, "%0*X", k_regs[i].digits, (unsigned)vals[i]);
            set_entry(w, w->reg[i], buf);
        }
    }
    {
        const int flags[6] = { c.sf, c.zf, c.hf, c.pf, c.nf, c.cf };
        w->updating_flags = TRUE;
        for (i = 0; i < 6; i++)
            gtk_check_button_set_active(w->flag[i], flags[i] != 0);
        w->updating_flags = FALSE;
    }
    g_snprintf(buf, sizeof buf,
               "line %d   dot %d   V counter $%02X   H counter $%02X   frame %u   "
               "cycle %llu%s%s%s",
               c.vpos, c.hpos, c.vcount & 0xFF, c.hcount & 0xFF, c.frame,
               (unsigned long long)c.cycles,
               c.int_line ? "   INT" : "", c.nmi_line ? "   NMI" : "",
               c.halted ? "   HALT" : "");
    gtk_label_set_text(w->beam, buf);
}

static void refresh_ram(DbgWin *w)
{
    static uint8_t ram[RAM_SIZE];
    static char text[RAM_SIZE * 4 + 1024];
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->ram_view);
    int len = 0, row, col;
    smsdebug_ram_get(w->dbg, ram);
    len += g_snprintf(text + len, sizeof text - len,
                      "       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (row = 0; row < RAM_SIZE / 16; row++) {
        len += g_snprintf(text + len, sizeof text - len, "$%04X: ", RAM_BASE + row * 16);
        for (col = 0; col < 16; col++)
            len += g_snprintf(text + len, sizeof text - len, "%02X ", ram[row * 16 + col]);
        len += g_snprintf(text + len, sizeof text - len, "\n");
    }
    set_text(w->ram_view, text);

    /* the row Go to found stays marked across refreshes */
    if (w->ram_mark_row >= 0) {
        GtkTextIter s, e;
        gtk_text_buffer_get_iter_at_line(b, &s, w->ram_mark_row + 1);
        gtk_text_buffer_get_iter_at_line(b, &e, w->ram_mark_row + 2);
        gtk_text_buffer_apply_tag_by_name(b, "mark", &s, &e);
    }
}

static void refresh_disasm(DbgWin *w)
{
    static smsdebug_line lines[DISASM_WINDOW];
    static char text[DISASM_WINDOW * 220];
    int n, pc_line, i, len = 0;
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkTextIter s, e;

    /* Following, the PC sits a third of the way down the window. */
    if (gtk_check_button_get_active(w->follow_pc)) {
        smsdebug_cpu c;
        smsdebug_cpu_get(w->dbg, &c);
        w->disasm_top = smsdebug_row_address(w->dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }

    n = smsdebug_disassemble(w->dbg, (uint16_t)w->disasm_top, lines, DISASM_WINDOW, &pc_line);
    w->line_count = n;
    for (i = 0; i < n; i++) {
        w->line_addr[i] = lines[i].address;
        len += g_snprintf(text + len, sizeof text - len, "%c%c %04X  %-11s %-14s %s%s%s\n",
                          lines[i].has_breakpoint ? '*' : ' ',
                          lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label,
                          lines[i].disasm, lines[i].comment[0] ? "  ; " : "",
                          lines[i].comment);
    }
    if (n == 0)
        len += g_snprintf(text + len, sizeof text - len, "(no disassembly)\n");
    gtk_text_buffer_set_text(b, text, -1);

    /* the PC line in the accent colour */
    if (pc_line >= 0 && pc_line < n) {
        gtk_text_buffer_get_iter_at_line(b, &s, pc_line);
        gtk_text_buffer_get_iter_at_line(b, &e, pc_line + 1);
        gtk_text_buffer_apply_tag_by_name(b, "pc", &s, &e);
    }
}

static void show_view(DbgWin *w)
{
    int view = (int)gtk_drop_down_get_selected(w->vdp_view);
    int pal = (int)gtk_drop_down_get_selected(w->vdp_palette);
    int vw = 0, vh = 0, x, y, ow, oh;
    GBytes *bytes;
    GdkTexture *tex;

    if (!smsdebug_vdp_view(w->dbg, view, pal, w->view_px, &vw, &vh) ||
        vw <= 0 || vh <= 0 || vw * vh > SMSDEBUG_VIEW_MAX_PIXELS)
        return;
    /* Scaled here, nearest-neighbour, twice over, so the tiles stay
     * crisp. */
    ow = vw * 2;
    oh = vh * 2;
    for (y = 0; y < oh; y++)
        for (x = 0; x < ow; x++)
            w->view_px2[y * ow + x] = w->view_px[(y / 2) * vw + x / 2] | 0xFF000000u;
    bytes = g_bytes_new(w->view_px2, (gsize)ow * oh * 4);
    tex = gdk_memory_texture_new(ow, oh, GDK_MEMORY_B8G8R8A8, bytes, (gsize)ow * 4);
    gtk_picture_set_paintable(w->vdp_pic, GDK_PAINTABLE(tex));
    gtk_widget_set_size_request(GTK_WIDGET(w->vdp_pic), ow, oh);
    g_object_unref(tex);
    g_bytes_unref(bytes);
}

static void refresh_vdp(DbgWin *w)
{
    smsdebug_vdp v;
    smsdebug_sprite sp[64];
    static char text[4096], stext[64 * 64 + 128];
    char desc[160];
    int len = 0, r, n, i;

    smsdebug_vdp_get(w->dbg, &v);
    len += g_snprintf(text + len, sizeof text - len,
        "VDP %s   %s   %d lines   %s\n\n",
        v.kind_5246 ? "315-5246" : "315-5124", v.mode_name ? v.mode_name : "?",
        v.y_pixels, v.is_pal ? "PAL" : "NTSC");
    for (r = 0; r <= 10; r++) {
        smsdebug_vdp_describe_register(w->dbg, r, desc, sizeof desc);
        len += g_snprintf(text + len, sizeof text - len, "%s\n", desc);
    }
    len += g_snprintf(text + len, sizeof text - len,
        "\nStatus $%02X   address $%04X   code %d\n"
        "Read buffer $%02X%s\n"
        "Line counter %d   H counter $%02X\n",
        v.status, v.addr, v.code, v.buffer,
        v.second_byte ? "   (control word half written)" : "",
        v.line_counter, v.hcounter);
    if (v.mode == 4)
        len += g_snprintf(text + len, sizeof text - len,
            "Name table $%04X\n"
            "Sprite attributes $%04X   patterns $%04X\n",
            v.name_base, v.sat_base, v.sprite_pattern_base);
    else
        len += g_snprintf(text + len, sizeof text - len,
            "Name table $%04X   colour table $%04X\n"
            "Pattern table $%04X\n"
            "Sprite attributes $%04X   patterns $%04X\n",
            v.name_base, v.color_base, v.pattern_base, v.sat_base,
            v.sprite_pattern_base);
    len += g_snprintf(text + len, sizeof text - len,
        "Scroll X %d   Y %d   backdrop %d\n"
        "Display %s   sprites %s%s\n"
        "VINT %s%s   HINT %s%s\n"
        "Left column %s   sprite shift %s\n"
        "Top rows %s   right columns %s\n"
        "/INT %s   /NMI %s   Pause %s\n"
        "Beam line %d   dot %d   V counter $%02X\n"
        "Frame %u\n",
        v.scroll_x, v.scroll_y, v.backdrop,
        v.display_on ? "on" : "off", v.sprites_16 ? "8x16" : "8x8",
        v.sprites_zoom ? " zoomed" : "",
        v.vint_on ? "on" : "off", v.vint_pending ? " (pending)" : "",
        v.hint_on ? "on" : "off", v.hint_pending ? " (pending)" : "",
        v.left_column_blank ? "blanked" : "shown", v.sprite_shift ? "on" : "off",
        v.hscroll_lock_top ? "fixed" : "scroll",
        v.vscroll_lock_right ? "fixed" : "scroll",
        v.int_line ? "asserted" : "-", v.nmi_line ? "asserted" : "-",
        v.pause_held ? "held" : "-",
        v.vpos, v.hpos, v.vcount & 0xFF, v.frame);
    set_text(w->vdp_text, text);

    /* The sprite table: 64 entries in mode 4; 32 in the TMS modes, which
     * have a colour and the early clock per sprite. */
    n = smsdebug_sprites_get(w->dbg, sp);
    len = 0;
    if (n == 32)
        len += g_snprintf(stext + len, sizeof stext - len, " #    Y    X  tile  colour  EC\n");
    else
        len += g_snprintf(stext + len, sizeof stext - len, " #    Y    X  tile\n");
    for (i = 0; i < n; i++) {
        if (n == 32)
            len += g_snprintf(stext + len, sizeof stext - len, "%2d  %3d  %3d  $%03X  %6d  %s%s\n",
                              i, sp[i].y, sp[i].x, sp[i].tile, sp[i].color,
                              sp[i].early_clock ? "EC" : "  ",
                              sp[i].visible ? "" : "  (hidden)");
        else
            len += g_snprintf(stext + len, sizeof stext - len, "%2d  %3d  %3d  $%03X%s\n",
                              i, sp[i].y, sp[i].x, sp[i].tile,
                              sp[i].visible ? "" : "  (hidden)");
    }
    set_text(w->sprite_view, stext);

    /* the CRAM: 16 background colours, then 16 sprite colours */
    for (i = 0; i < 32; i++) {
        char hex[8];
        g_snprintf(hex, sizeof hex, "%02X", v.cram[i]);
        set_entry(w, w->cram_entry[i], hex);
        if (w->cram_rgb[i] != v.cram_rgb[i]) {
            w->cram_rgb[i] = v.cram_rgb[i];
            gtk_widget_queue_draw(w->cram_swatch[i]);
        }
    }
    show_view(w);
}

static void refresh_io(DbgWin *w)
{
    smsdebug_io io;
    static char text[4096];
    char pad[2][64];
    static const char *const names[6] = { "Up", "Down", "Left", "Right", "1", "2" };
    int len = 0, port, b, i;

    smsdebug_io_get(w->dbg, &io);
    for (port = 0; port < 2; port++) {
        int plen = 0;
        pad[port][0] = '\0';
        for (b = 0; b < 6; b++)
            if (io.pad[port] & (1u << b))
                plen += g_snprintf(pad[port] + plen, sizeof pad[port] - plen, "%s ", names[b]);
        if (!plen) g_snprintf(pad[port], sizeof pad[port], "(nothing held)");
    }
    len += g_snprintf(text + len, sizeof text - len,
        "PSG (SN76489)%s\n"
        "Tone 0     period %4d   volume %2d\n"
        "Tone 1     period %4d   volume %2d\n"
        "Tone 2     period %4d   volume %2d\n"
        "Noise      %s   rate %d   volume %2d   LFSR $%04X\n"
        "           (volume 0 is loudest, 15 is off)\n\n",
        io.psg_audible ? "" : "   (muted)",
        io.tone_period[0], io.tone_volume[0], io.tone_period[1], io.tone_volume[1],
        io.tone_period[2], io.tone_volume[2],
        io.noise_mode ? "white   " : "periodic", io.noise_rate, io.noise_volume, io.lfsr);
    if (io.fm_present) {
        /* the YM2413's 64 registers as a grid, 16 to a row */
        len += g_snprintf(text + len, sizeof text - len, "YM2413 (FM)%s\n",
                          io.fm_audible ? "" : "   (muted)");
        len += g_snprintf(text + len, sizeof text - len,
                          "        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
        for (i = 0; i < 0x40; i++) {
            if ((i & 15) == 0)
                len += g_snprintf(text + len, sizeof text - len, "  $%02X: ", i);
            len += g_snprintf(text + len, sizeof text - len, "%02X ", io.fm_regs[i]);
            if ((i & 15) == 15)
                len += g_snprintf(text + len, sizeof text - len, "\n");
        }
        len += g_snprintf(text + len, sizeof text - len, "\n");
    } else {
        len += g_snprintf(text + len, sizeof text - len,
                          "YM2413 (FM)   not fitted on this console\n\n");
    }
    len += g_snprintf(text + len, sizeof text - len,
        "Memory control $3E = $%02X   cartridge %s   BIOS %s   work RAM %s   I/O chip %s\n"
        "I/O control    $3F = $%02X\n"
        "BIOS           %s",
        io.mem_ctrl, io.cart_enabled ? "on" : "off", io.bios_enabled ? "on" : "off",
        io.ram_enabled ? "on" : "off", io.io_enabled ? "on" : "off",
        io.io_ctrl, io.bios_present ? "present" : "none (the cartridge boots directly)");
    if (io.bios_present)
        len += g_snprintf(text + len, sizeof text - len, "   pages $%02X $%02X $%02X",
                          io.bios_page[0], io.bios_page[1], io.bios_page[2]);
    len += g_snprintf(text + len, sizeof text - len,
        "\nMapper         $FFFC=$%02X  $FFFD=$%02X  $FFFE=$%02X  $FFFF=$%02X   (the RAM copy)\n"
        "Ports          $DC=$%02X  $DD=$%02X\n"
        "Joypad 1       $%02X  %s\n"
        "Joypad 2       $%02X  %s\n"
        "Pause %s   Reset %s\n"
        "Nationality    %s\n"
        "Console        %s\n",
        io.mapper[0], io.mapper[1], io.mapper[2], io.mapper[3],
        io.port_dc, io.port_dd,
        io.pad[0], pad[0], io.pad[1], pad[1],
        io.pause_held ? "held" : "-", io.reset_held ? "held" : "-",
        io.japanese ? "Japanese" : "export",
        io.console_name ? io.console_name : "?");
    set_text(w->io_text, text);
}

static void refresh_cart(DbgWin *w)
{
    smsdebug_cart c;
    char info[256];
    static char text[4096];
    int len = 0, i;
    smsdebug_cart_get(w->dbg, &c);
    smsdebug_cart_info(w->dbg, info, sizeof info);
    if (!c.present) {
        set_text(w->cart_text, info);
        return;
    }
    len += g_snprintf(text + len, sizeof text - len,
        "%s\n\n"
        "Mode         %s (%d)%s%s\n"
        "Link         %s%s\n"
        "Mapper       %s (%d)   banks %02X %02X %02X %02X %02X %02X\n"
        "Cart RAM     %s%s   %u bytes\n"
        "Image        %u bytes   CRC-32 %08X   claim %d\n"
        "Mailbox      ACKSEQ $%02X   STATUS $%02X   last error %u   reply cmd $%02X   RXLEN %u\n"
        "Boot         state %d   %d%%   error %d   %u / %u bytes\n"
        "Load         state %d   window %d / %d   %d%%\n"
        "BIOS snoop   phase %d   $C000=$%02X   $3E=$%02X   $3F=$%02X\n"
        "             VDP R0-R10",
        info,
        c.mode_name ? c.mode_name : "?", c.mode,
        c.booted_game ? "   game booted" : "", c.direct ? "   (opened file)" : "",
        c.link_up ? "up" : "down", c.busy ? "   (transaction in flight)" : "",
        c.mapper < 0 ? "-" : c.mapper_name, c.mapper,
        c.bank[0], c.bank[1], c.bank[2], c.bank[3], c.bank[4], c.bank[5],
        c.ram_enabled ? "enabled" : "off", c.ram_writable ? " (writable)" : "",
        c.ram_size,
        c.image_size, c.image_crc, c.claim,
        c.ackseq, c.status, c.last_error, c.reply_cmd, c.rxlen,
        c.boot_state, c.boot_pct, c.boot_err, c.boot_got, c.boot_total,
        c.load_state, c.load_win, c.load_nwin, c.load_pct,
        c.bios_phase, c.snoop_c000, c.snoop_3e, c.snoop_3f);
    for (i = 0; i < 11; i++)
        len += g_snprintf(text + len, sizeof text - len, " %02X", c.snoop_vdp[i]);
    len += g_snprintf(text + len, sizeof text - len,
        "\nQueue        %u\n\n"
        "1K page -> 8K SRAM bank   (-- = the cartridge's own memory)\n",
        c.queue_depth);
    for (i = 0; i < 48; i++) {
        if ((i & 15) == 0)
            len += g_snprintf(text + len, sizeof text - len, "$%04X:", i * 0x400);
        if (c.page_bank[i] < 0)
            len += g_snprintf(text + len, sizeof text - len, " --");
        else
            len += g_snprintf(text + len, sizeof text - len, " %02X", c.page_bank[i] & 0xFF);
        if ((i & 15) == 15)
            len += g_snprintf(text + len, sizeof text - len, "\n");
    }
    set_text(w->cart_text, text);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud);
static void bp_remove_clicked(GtkButton *b, gpointer ud);

static const char *bp_type_name(int type)
{
    if (type & SMSDEBUG_BP_EXEC) return "Execute";
    if ((type & (SMSDEBUG_BP_READ | SMSDEBUG_BP_WRITE)) == (SMSDEBUG_BP_READ | SMSDEBUG_BP_WRITE))
        return "Read/Write";
    if (type & SMSDEBUG_BP_READ) return "Read";
    if (type & SMSDEBUG_BP_WRITE) return "Write";
    if ((type & (SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT)) == (SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT))
        return "In/Out";
    if (type & SMSDEBUG_BP_IN) return "Port In";
    return "Port Out";
}

static void refresh_bps(DbgWin *w)
{
    smsdebug_breakpoint bps[128];
    GtkWidget *child;
    int n = smsdebug_breakpoint_list(w->dbg, bps, 128), i;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->bp_list))) != NULL)
        gtk_list_box_remove(w->bp_list, child);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No breakpoints. Click a disassembly line, or add one below.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_list_box_append(w->bp_list, l);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *en = gtk_check_button_new();
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *l;
        char text[320], range[32];
        const gboolean port = (bps[i].type & (SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT)) != 0;
        if (bps[i].end != bps[i].start)
            g_snprintf(range, sizeof range, port ? "$%02X-$%02X" : "$%04X-$%04X",
                       bps[i].start, bps[i].end);
        else
            g_snprintf(range, sizeof range, port ? "$%02X" : "$%04X", bps[i].start);
        g_snprintf(text, sizeof text, "#%-3d %-10s %-12s hits %-6u %s%s", bps[i].id,
                   bp_type_name(bps[i].type), range, bps[i].hits,
                   bps[i].condition[0] ? "if " : "", bps[i].condition);
        l = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_add_css_class(l, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(en), bps[i].enabled != 0);
        gtk_widget_set_tooltip_text(en, "Enabled");
        g_object_set_data(G_OBJECT(en), "bp", GINT_TO_POINTER(bps[i].id));
        g_object_set_data(G_OBJECT(rm), "bp", GINT_TO_POINTER(bps[i].id));
        g_signal_connect(en, "toggled", G_CALLBACK(bp_enable_toggled), w);
        g_signal_connect(rm, "clicked", G_CALLBACK(bp_remove_clicked), w);
        gtk_widget_add_css_class(rm, "flat");
        gtk_widget_set_tooltip_text(rm, "Remove");
        gtk_box_append(GTK_BOX(row), en);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), rm);
        gtk_list_box_append(w->bp_list, row);
    }
}

static void refresh_status(DbgWin *w)
{
    char reason[160], text[256];
    int addr;
    gboolean stopped = smsdebug_is_stopped(w->dbg) != 0;
    smsdebug_stop_reason(w->dbg, reason, sizeof reason, &addr);
    if (stopped)
        g_snprintf(text, sizeof text, "Stopped%s%s", reason[0] ? ": " : "", reason);
    else
        g_snprintf(text, sizeof text, "Running");
    adw_window_title_set_subtitle(w->title, text);
    gtk_button_set_label(w->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(w->run_btn), "sms-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(w->run_btn), "sms-accent");
}

static void refresh_all(DbgWin *w)
{
    refresh_status(w);
    refresh_cpu(w);
    refresh_ram(w);
    refresh_disasm(w);
    refresh_vdp(w);
    refresh_io(w);
    refresh_bps(w);
    refresh_cart(w);
}

static void attach_and_refresh(DbgWin *w);

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    unsigned gen;
    gboolean stopped;
    if (!gtk_widget_get_visible(GTK_WIDGET(w->win))) return G_SOURCE_CONTINUE;
    /* Opened before the session started (SMS_OPEN_DEBUGGER), or showing
     * across a session restart: the engine comes up with the machine. */
    if (!smsdebug_is_attached(w->dbg) && smssession_is_running(w->session)) {
        attach_and_refresh(w);
        return G_SOURCE_CONTINUE;
    }
    gen = smsdebug_generation(w->dbg);
    stopped = smsdebug_is_stopped(w->dbg) != 0;
    if (gen != w->seen_generation || stopped != w->was_stopped) {
        w->seen_generation = gen;
        w->was_stopped = stopped;
        refresh_all(w);
    } else if (!stopped && ++w->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        w->running_ticks = 0;
        refresh_status(w);
        refresh_cpu(w);
        refresh_vdp(w);
        refresh_io(w);
        refresh_cart(w);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (smsdebug_is_stopped(w->dbg)) smsdebug_resume(w->dbg);
    else smsdebug_stop(w->dbg);
    refresh_all(w);
}

static void on_step(GtkButton *b, gpointer ud) { (void)b; smsdebug_step(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_step_over(GtkButton *b, gpointer ud) { (void)b; smsdebug_step_over(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_step_out(GtkButton *b, gpointer ud) { (void)b; smsdebug_step_out(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_scanline(GtkButton *b, gpointer ud) { (void)b; smsdebug_scanline(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }
static void on_frame(GtkButton *b, gpointer ud) { (void)b; smsdebug_frame(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }

static void on_prompt(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    if (!cmd || !*cmd) return;
    /* the history keeps each command once in a row */
    if (w->history->len == 0 ||
        strcmp(g_ptr_array_index(w->history, w->history->len - 1), cmd) != 0)
        g_ptr_array_add(w->history, g_strdup(cmd));
    w->history_pos = w->history->len;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(w->prompt_out, w->prompt_scroll, line);
    smsdebug_command(w->dbg, cmd, out, sizeof out);
    append_text(w->prompt_out, w->prompt_scroll, out);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(w->prompt_out, w->prompt_scroll, "\n");
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    refresh_all(w);
}

static void history_show(DbgWin *w)
{
    const char *text = w->history_pos < w->history->len
                           ? g_ptr_array_index(w->history, w->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(w->prompt_in), text);
    gtk_editable_set_position(GTK_EDITABLE(w->prompt_in), -1);
}

/* Tab completes the current word against the commands, the registers and
 * the labels; several matches are listed instead. Up and Down walk the
 * command history. */
static gboolean on_prompt_key(GtkEventControllerKey *c, guint keyval, guint code,
                              GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    char comps[4096];
    const char *text;
    const char *word;
    int n;
    (void)c; (void)code; (void)st;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (w->history_pos > 0) {
            w->history_pos--;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (w->history_pos < w->history->len) {
            w->history_pos++;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(w->prompt_in));
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    n = smsdebug_completions(w->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char merged[600];
        char *nl = strchr(comps, '\n');
        if (nl) *nl = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(w->prompt_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(w->prompt_in), -1);
    } else if (n > 1) {
        append_text(w->prompt_out, w->prompt_scroll, comps);
        if (comps[0] && comps[strlen(comps) - 1] != '\n')
            append_text(w->prompt_out, w->prompt_scroll, "\n");
    }
    return TRUE;
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long v;
    if (parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v))
        smsdebug_cpu_set(w->dbg, k_regs[i].reg, (int)v);
    refresh_all(w);
}

static void on_flag_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "flag"));
    static const int flags[6] = { SMS_FLAG_S, SMS_FLAG_Z, SMS_FLAG_H, SMS_FLAG_PV,
                                  SMS_FLAG_N, SMS_FLAG_C };
    if (w->updating_flags || !smsdebug_is_stopped(w->dbg)) return;
    smsdebug_cpu_set(w->dbg, flags[i], gtk_check_button_get_active(b));
}

static void on_ram_write(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a, v;
    (void)entry;
    if (parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->ram_addr)), &a)
        && parse_num(gtk_editable_get_text(GTK_EDITABLE(w->ram_val)), &v))
        smsdebug_write(w->dbg, (uint16_t)a, (uint8_t)v);
    refresh_all(w);
}

/* Go to: the RAM row holding an address (or a label), scrolled to and
 * marked. $E000-$FFFF mirror the 8K. */
static void on_ram_goto(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->ram_view);
    GtkTextMark *mark;
    GtkTextIter it;
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a) ||
        a < RAM_BASE || a > 0xFFFF) {
        w->ram_mark_row = -1;
        refresh_ram(w);
        return;
    }
    w->ram_mark_row = (int)((a - RAM_BASE) & (RAM_SIZE - 1)) / 16;
    refresh_ram(w);
    gtk_text_buffer_get_iter_at_line(b, &it, w->ram_mark_row + 1);
    mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
    gtk_text_view_scroll_to_mark(w->ram_view, mark, 0.0, TRUE, 0.0, 0.3);
    gtk_text_buffer_delete_mark(b, mark);
}

static void on_disasm_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextIter it;
    int bx, by, line;
    (void)g; (void)n;
    gtk_text_view_window_to_buffer_coords(w->disasm, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    gtk_text_view_get_iter_at_location(w->disasm, &it, bx, by);
    line = gtk_text_iter_get_line(&it);
    if (line < 0 || line >= w->line_count) return;
    smsdebug_breakpoint_toggle(w->dbg, w->line_addr[line]);
    refresh_disasm(w);
    refresh_bps(w);
}

static void on_follow_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    refresh_disasm(ud);
}

static void on_jump(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a)) return;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = (int)(a & 0xFFFF);
    refresh_disasm(w);
}

static gboolean on_disasm_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    DbgWin *w = ud;
    int rows = (int)(dy * 3);
    (void)c; (void)dx;
    if (!rows) rows = dy > 0 ? 1 : -1;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = smsdebug_row_address(w->dbg, (uint16_t)w->disasm_top, rows);
    refresh_disasm(w);
    return TRUE;
}

static void on_view_changed(GObject *o, GParamSpec *ps, gpointer ud)
{
    DbgWin *w = ud;
    (void)o; (void)ps;
    gtk_widget_set_sensitive(GTK_WIDGET(w->vdp_palette),
        gtk_drop_down_get_selected(w->vdp_view) == SMSDEBUG_VIEW_TILES);
    show_view(w);
}

static void on_palette_changed(GObject *o, GParamSpec *ps, gpointer ud)
{
    (void)o; (void)ps;
    show_view(ud);
}

static void draw_swatch(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgWin *w = g_object_get_data(G_OBJECT(area), "dbg");
    int i = GPOINTER_TO_INT(ud);
    guint32 rgb = w ? w->cram_rgb[i] : 0;
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_set_source_rgb(cr, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                         (rgb & 0xFF) / 255.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.8);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

static void on_cram_write(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "cram"));
    long v;
    if (parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v))
        smsdebug_cram_write(w->dbg, i, (uint8_t)v);
    refresh_all(w);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    smsdebug_breakpoint_enable(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")),
                               gtk_check_button_get_active(b));
    refresh_disasm(w);
}

static void bp_remove_clicked(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    smsdebug_breakpoint_remove(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")));
    refresh_all(w);
}

static void on_bp_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    static const int types[6] = { SMSDEBUG_BP_EXEC, SMSDEBUG_BP_READ, SMSDEBUG_BP_WRITE,
                                  SMSDEBUG_BP_READ | SMSDEBUG_BP_WRITE,
                                  SMSDEBUG_BP_IN, SMSDEBUG_BP_OUT };
    guint sel = gtk_drop_down_get_selected(w->bp_type);
    long a, b;
    int id;
    const char *end = gtk_editable_get_text(GTK_EDITABLE(w->bp_end));
    (void)wgt;
    if (sel >= G_N_ELEMENTS(types)) sel = 0;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->bp_start)), &a)) {
        gtk_label_set_text(w->bp_msg, "Not an address or a label");
        return;
    }
    b = a;
    if (end && *end && !parse_addr(w, end, &b)) {
        gtk_label_set_text(w->bp_msg, "The end is not an address or a label");
        return;
    }
    id = smsdebug_breakpoint_add(w->dbg, types[sel], (uint16_t)a, (uint16_t)b,
                                 gtk_editable_get_text(GTK_EDITABLE(w->bp_cond)));
    if (id < 0) {
        gtk_label_set_text(w->bp_msg, "The condition does not parse");
        return;
    }
    gtk_label_set_text(w->bp_msg, "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_cond), "");
    refresh_all(w);
}

static void on_bp_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    smsdebug_breakpoint_clear(w->dbg);
    refresh_all(w);
}

static void load_symbols(DbgWin *w, const char *path)
{
    char msg[512];
    smsdebug_load_symbols(w->dbg, path, msg, sizeof msg);
    prompt_say(w, msg);
    refresh_all(w);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    load_symbols(w, path);
}

static void on_symbols(GtkWidget *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *syms = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    const char *cart = smssession_cart_path(w->session);
    (void)b;

    gtk_file_filter_set_name(syms, "Symbol files (*.sym, *.map, *.noi, *.txt)");
    gtk_file_filter_add_suffix(syms, "sym");
    gtk_file_filter_add_suffix(syms, "map");
    gtk_file_filter_add_suffix(syms, "noi");
    gtk_file_filter_add_suffix(syms, "txt");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, syms);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load Symbols (WLA-DX .sym, z88dk .map, SDCC .noi, AAAA name)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    /* a game's symbols usually sit beside it */
    if (cart && *cart) {
        g_autofree char *dir = g_path_get_dirname(cart);
        g_autoptr(GFile) folder = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(dlg, folder);
    }
    gtk_file_dialog_open(dlg, w->win, NULL, on_symbols_chosen, w);
    g_object_unref(syms);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

/* <cart>.sym/.map/.noi next to the opened cartridge, no picker needed. */
static void on_symbols_beside(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    load_symbols(w, NULL);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    smsdebug_save(w->dbg, kind, path, msg, sizeof msg);
    prompt_say(w, msg);
}

static void on_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *kind = g_object_get_data(G_OBJECT(b), "kind");
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    char title[64];
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    g_snprintf(title, sizeof title, "Save %s", (const char *)g_object_get_data(G_OBJECT(b), "title"));
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_initial_name(dlg, g_object_get_data(G_OBJECT(b), "file"));
    g_object_set_data(G_OBJECT(dlg), "kind", (gpointer)kind);
    gtk_file_dialog_save(dlg, w->win, NULL, on_save_chosen, w);
    g_object_unref(dlg);
}

static void hide_window(DbgWin *w)
{
    smsdebug_detach(w->dbg);
    gtk_widget_set_visible(GTK_WIDGET(w->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    (void)c; (void)code;
    switch (keyval) {
    case GDK_KEY_F5: on_run(NULL, w); return TRUE;
    case GDK_KEY_F7: on_step(NULL, w); return TRUE;
    case GDK_KEY_F8:
        if (st & GDK_SHIFT_MASK) on_step_out(NULL, w); else on_step_over(NULL, w);
        return TRUE;
    case GDK_KEY_F12: hide_window(w); return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer ud)
{
    (void)win;
    hide_window(ud);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *build_toolbar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b, *sym, *sympop, *beside, *save, *pop, *popbox;
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly ($0000-$BFFF)", "disassembly.asm" },
        { "ram", "Console RAM", "ram.bin" },
        { "vram", "VRAM", "vram.bin" },
        { "cram", "CRAM", "cram.bin" },
        { "sram", "Cartridge SRAM", "sram.bin" },
        { "arena", "Mailbox Arena", "arena.bin" },
        { "regs", "Registers (text)", "registers.txt" } };
    unsigned i;

    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, cb) do { b = gtk_button_new_with_label(label); \
    g_signal_connect(b, "clicked", G_CALLBACK(cb), w); gtk_box_append(GTK_BOX(bar), b); } while (0)
    w->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(w->run_btn, "clicked", G_CALLBACK(on_run), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_btn));
    TB("Step (F7)", on_step);
    TB("Step Over (F8)", on_step_over);
    TB("Step Out (\xe2\x87\xa7""F8)", on_step_out);
    TB("Scanline+1", on_scanline);
    TB("Frame+1", on_frame);
#undef TB

    /* Load Symbols: a file, or (the arrow) the files beside the cartridge */
    sym = adw_split_button_new();
    adw_split_button_set_label(ADW_SPLIT_BUTTON(sym), "Load Symbols\xe2\x80\xa6");
    adw_split_button_set_dropdown_tooltip(ADW_SPLIT_BUTTON(sym), "More ways to load symbols");
    g_signal_connect(sym, "clicked", G_CALLBACK(on_symbols), w);
    sympop = gtk_popover_new();
    beside = gtk_button_new_with_label("Beside the Cartridge (.sym, .map, .noi)");
    gtk_widget_add_css_class(beside, "flat");
    g_signal_connect(beside, "clicked", G_CALLBACK(on_symbols_beside), w);
    gtk_popover_set_child(GTK_POPOVER(sympop), beside);
    adw_split_button_set_popover(ADW_SPLIT_BUTTON(sym), GTK_POPOVER(sympop));
    gtk_box_append(GTK_BOX(bar), sym);

    save = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(save), "Save\xe2\x80\xa6");
    pop = gtk_popover_new();
    popbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (i = 0; i < G_N_ELEMENTS(saves); i++) {
        GtkWidget *sb = gtk_button_new_with_label(saves[i].title);
        gtk_widget_add_css_class(sb, "flat");
        g_object_set_data(G_OBJECT(sb), "kind", (gpointer)saves[i].kind);
        g_object_set_data(G_OBJECT(sb), "title", (gpointer)saves[i].title);
        g_object_set_data(G_OBJECT(sb), "file", (gpointer)saves[i].file);
        g_signal_connect(sb, "clicked", G_CALLBACK(on_save), w);
        gtk_box_append(GTK_BOX(popbox), sb);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), popbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(save), pop);
    gtk_box_append(GTK_BOX(bar), save);
    return bar;
}

static GtkWidget *build_prompt(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&w->prompt_out, FALSE);
    GtkEventController *keys;

    w->prompt_scroll = GTK_SCROLLED_WINDOW(scroll);
    gtk_text_view_set_wrap_mode(w->prompt_out, GTK_WRAP_WORD_CHAR);
    w->prompt_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->prompt_in,
        "Command (help, step, break, bpw, bpout, print, mem, poke, vdp, disasm, cart, ...) "
        "\xe2\x80\x94 Tab completes, Up/Down recall");
    gtk_widget_set_hexpand(GTK_WIDGET(w->prompt_in), TRUE);
    g_signal_connect(w->prompt_in, "activate", G_CALLBACK(on_prompt), w);
    /* ahead of the entry's own text handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_prompt_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->prompt_in), keys);
    w->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->prompt_in));
    set_text(w->prompt_out, "Master System debugger prompt (Z80 and 315-5124). "
                            "Type 'help' for every command.\n");
    return padded(box);
}

static GtkWidget *build_cpu(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *regs1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *regs2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ram = mono_view(&w->ram_view, FALSE);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *hint, *title;
    static const char *const fnames[6] = { "S", "Z", "H", "P/V", "N", "C" };
    int i;
    for (i = 0; i < NREGS; i++) {
        w->reg[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(w->reg[i], 6);
        gtk_editable_set_width_chars(GTK_EDITABLE(w->reg[i]), k_regs[i].digits + 1);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(w->reg[i]), k_regs[i].digits + 1);
        g_object_set_data(G_OBJECT(w->reg[i]), "reg", GINT_TO_POINTER(i));
        g_signal_connect(w->reg[i], "activate", G_CALLBACK(on_reg_activate), w);
        /* the main set on the first row; shadows and control on the second */
        gtk_box_append(GTK_BOX(i < 8 ? regs1 : regs2),
                       labeled(k_regs[i].name, GTK_WIDGET(w->reg[i])));
    }
    for (i = 0; i < 6; i++) {
        w->flag[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(fnames[i]));
        g_object_set_data(G_OBJECT(w->flag[i]), "flag", GINT_TO_POINTER(i));
        g_signal_connect(w->flag[i], "toggled", G_CALLBACK(on_flag_toggled), w);
        gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->flag[i]));
    }
    w->beam = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->beam), "dim-label");
    gtk_label_set_xalign(w->beam, 0);

    w->ram_goto = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->ram_goto), 10);
    gtk_entry_set_placeholder_text(w->ram_goto, "$C100 / label");
    g_signal_connect(w->ram_goto, "activate", G_CALLBACK(on_ram_goto), w);
    w->ram_addr = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->ram_addr), 10);
    gtk_entry_set_placeholder_text(w->ram_addr, "$C000");
    w->ram_val = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->ram_val), 4);
    gtk_entry_set_placeholder_text(w->ram_val, "$00");
    g_signal_connect(w->ram_val, "activate", G_CALLBACK(on_ram_write), w);
    hint = gtk_label_new("(any address or label: RAM takes it as a bus write, "
                         "$FFFC-$FFFF bank; below $C000 it edits the cartridge's memory)");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_box_append(GTK_BOX(edit), labeled("Go to", GTK_WIDGET(w->ram_goto)));
    gtk_box_append(GTK_BOX(edit), labeled("Write address", GTK_WIDGET(w->ram_addr)));
    gtk_box_append(GTK_BOX(edit), labeled("value", GTK_WIDGET(w->ram_val)));
    gtk_box_append(GTK_BOX(edit), hint);

    title = gtk_label_new("Console RAM ($C000-$DFFF, mirrored at $E000-$FFFF)");
    gtk_box_append(GTK_BOX(box), regs1);
    gtk_box_append(GTK_BOX(box), regs2);
    gtk_box_append(GTK_BOX(box), flags);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->beam));
    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), ram);
    gtk_box_append(GTK_BOX(box), edit);
    accent_tag(w->ram_view, "mark");
    w->ram_mark_row = -1;
    return padded(box);
}

static GtkWidget *build_disasm(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&w->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkWidget *hint;

    accent_tag(w->disasm, "pc");

    w->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(w->follow_pc, TRUE);
    g_signal_connect(w->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), w);
    w->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->jump, "$0038 or label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->jump), 14);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    hint = gtk_label_new("Click a line to toggle its breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(w->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), scrollc);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    w->disasm_top = 0x0000;
    return padded(box);
}

static GtkWidget *build_vdp(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *picscroll = gtk_scrolled_window_new();
    GtkWidget *regs = mono_view(&w->vdp_text, FALSE);
    GtkWidget *sprites = mono_view(&w->sprite_view, FALSE);
    GtkWidget *cram = gtk_grid_new();
    static const char *const views[] = { "Name table", "Tiles", "Sprites", "Palette", NULL };
    static const char *const palettes[] = { "Background palette", "Sprite palette", NULL };
    static const char *const cram_rows[4] = { "BG 0-7", "BG 8-15", "Sprite 0-7", "Sprite 8-15" };
    int i;

    gtk_widget_set_size_request(left, 460, -1);
    gtk_widget_set_hexpand(left, FALSE);
    gtk_box_append(GTK_BOX(left), regs);
    gtk_box_append(GTK_BOX(left), gtk_label_new("Sprite table"));
    gtk_box_append(GTK_BOX(left), sprites);

    w->vdp_view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    g_signal_connect(w->vdp_view, "notify::selected", G_CALLBACK(on_view_changed), w);
    w->vdp_palette = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(palettes));
    g_signal_connect(w->vdp_palette, "notify::selected", G_CALLBACK(on_palette_changed), w);
    gtk_widget_set_sensitive(GTK_WIDGET(w->vdp_palette), FALSE);
    gtk_box_append(GTK_BOX(row), labeled("View", GTK_WIDGET(w->vdp_view)));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->vdp_palette));

    w->vdp_pic = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_can_shrink(w->vdp_pic, FALSE);
    gtk_widget_set_halign(GTK_WIDGET(w->vdp_pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(w->vdp_pic), GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(picscroll), GTK_WIDGET(w->vdp_pic));
    gtk_widget_set_vexpand(picscroll, TRUE);
    gtk_widget_set_hexpand(picscroll, TRUE);

    /* The CRAM: each entry's colour through the VDP's palette, and its byte,
     * editable (Enter writes it). */
    gtk_grid_set_row_spacing(GTK_GRID(cram), 4);
    gtk_grid_set_column_spacing(GTK_GRID(cram), 4);
    for (i = 0; i < 4; i++) {
        GtkWidget *l = gtk_label_new(cram_rows[i]);
        gtk_widget_add_css_class(l, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_grid_attach(GTK_GRID(cram), l, 0, i, 1, 1);
    }
    for (i = 0; i < 32; i++) {
        GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        w->cram_swatch[i] = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(w->cram_swatch[i]), 18);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(w->cram_swatch[i]), 18);
        gtk_widget_set_valign(w->cram_swatch[i], GTK_ALIGN_CENTER);
        g_object_set_data(G_OBJECT(w->cram_swatch[i]), "dbg", w);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->cram_swatch[i]), draw_swatch,
                                       GINT_TO_POINTER(i), NULL);
        w->cram_entry[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(w->cram_entry[i], 3);
        gtk_editable_set_width_chars(GTK_EDITABLE(w->cram_entry[i]), 2);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(w->cram_entry[i]), 2);
        g_object_set_data(G_OBJECT(w->cram_entry[i]), "cram", GINT_TO_POINTER(i));
        g_signal_connect(w->cram_entry[i], "activate", G_CALLBACK(on_cram_write), w);
        gtk_box_append(GTK_BOX(cell), w->cram_swatch[i]);
        gtk_box_append(GTK_BOX(cell), GTK_WIDGET(w->cram_entry[i]));
        gtk_grid_attach(GTK_GRID(cram), cell, 1 + (i & 7), i / 8, 1, 1);
    }

    gtk_box_append(GTK_BOX(right), row);
    gtk_box_append(GTK_BOX(right), picscroll);
    gtk_box_append(GTK_BOX(right), gtk_label_new("CRAM"));
    gtk_box_append(GTK_BOX(right), cram);

    gtk_box_append(GTK_BOX(box), left);
    gtk_box_append(GTK_BOX(box), right);
    w->view_px = g_new0(guint32, SMSDEBUG_VIEW_MAX_PIXELS);
    w->view_px2 = g_new0(guint32, SMSDEBUG_VIEW_MAX_PIXELS * 4);
    return padded(box);
}

static GtkWidget *build_io(DbgWin *w)
{
    return padded(mono_view(&w->io_text, FALSE));
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear;
    static const char *const types[] = { "Execute", "Read", "Write", "Read/Write",
                                         "Port In", "Port Out", NULL };

    w->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    w->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    w->bp_start = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_start, "$C000 / port / label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_start), 14);
    w->bp_end = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_end, "(end)");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_end), 8);
    w->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_cond, "condition (a == $FF && [$C010] > 3) \xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_cond), TRUE);
    g_signal_connect(w->bp_start, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_end, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_cond, "activate", G_CALLBACK(on_bp_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), w);
    clear = gtk_button_new_with_label("Clear all");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_bp_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_type));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_start));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_end));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_cond));
    gtk_box_append(GTK_BOX(row), add);
    gtk_box_append(GTK_BOX(row), clear);
    w->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_msg), "sms-accent-text");
    gtk_label_set_xalign(w->bp_msg, 0);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bp_msg));
    return padded(box);
}

static GtkWidget *build_cart(DbgWin *w)
{
    return padded(mono_view(&w->cart_text, FALSE));
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    smsdebug_detach(w->dbg);
    g_free(w->view_px);
    g_free(w->view_px2);
    if (w->history) g_ptr_array_unref(w->history);
    if (g_win == w) g_win = NULL;
    g_free(w);
}

static void attach_and_refresh(DbgWin *w)
{
    smsdebug_attach(w->dbg);     /* stops the machine */
    w->seen_generation = smsdebug_generation(w->dbg);
    w->was_stopped = smsdebug_is_stopped(w->dbg) != 0;
    refresh_all(w);
}

/* SMS_DEBUGGER_TAB: a page number, or a tab's name (or the start of one). */
static int tab_from_env(void)
{
    const char *tab = g_getenv("SMS_DEBUGGER_TAB");
    char *end;
    long n;
    unsigned i;
    if (!tab || !*tab) return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < (long)G_N_ELEMENTS(k_tab_names)) ? (int)n : -1;
    for (i = 0; i < G_N_ELEMENTS(k_tab_names); i++)
        if (g_ascii_strncasecmp(tab, k_tab_names[i], strlen(tab)) == 0)
            return (int)i;
    return -1;
}

void sms_debugger_show(GtkWindow *parent, smssession *session)
{
    DbgWin *w;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay;
    GtkEventController *keys;
    int tab;

    if (g_win) {
        gtk_window_present(g_win->win);
        attach_and_refresh(g_win);
        return;
    }
    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = smssession_debugger(session);
    g_win = w;

    w->win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_default_size(w->win, 1150, 820);
    gtk_window_set_transient_for(w->win, parent);
    gtk_window_set_application(w->win, gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(w->win, TRUE);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    w->notebook = GTK_NOTEBOOK(notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_prompt(w), gtk_label_new("Prompt"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_cpu(w), gtk_label_new("CPU & RAM"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_disasm(w), gtk_label_new("Disassembly"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_vdp(w), gtk_label_new("VDP"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_io(w), gtk_label_new("Sound & I/O"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_breaks(w), gtk_label_new("Breakpoints"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_cart(w), gtk_label_new("Cart"));
    gtk_box_append(GTK_BOX(root), notebook);
    tab = tab_from_env();
    if (tab >= 0) gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), tab);

    overlay = adw_toast_overlay_new();
    w->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(w->toasts, root);

    /* the status line -- why the machine stopped -- is the title's subtitle */
    header = adw_header_bar_new();
    w->title = ADW_WINDOW_TITLE(adw_window_title_new("Debugger", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(w->title));
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), overlay);
    adw_window_set_content(ADW_WINDOW(w->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    attach_and_refresh(w);
    w->timer = g_timeout_add(100, tick_refresh, w);
    gtk_window_present(w->win);
}

void sms_debugger_toggle(GtkWindow *parent, smssession *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        sms_debugger_show(parent, session);
}
