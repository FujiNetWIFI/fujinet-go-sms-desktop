/*
 * Debugger window (Win32) over the Z80 / VDP debugger engine, via
 * core/include/smsdebug.h. Mirrors the GTK and Qt debuggers tab for tab --
 * Prompt, CPU & RAM, Disassembly, VDP, Sound & I/O, Breakpoints, Cart --
 * built from plain common controls, with the disassembly, the VDP picture
 * and the CRAM swatches drawn by hand.
 *
 * The engine only exists while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling) and hiding it detaches,
 * which lets the machine run on at full speed. The window refreshes on a
 * timer keyed to the engine's generation counter rather than on every tick.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smsdebug.h"

#define DISASM_MAX 96        /* rows fetched at most; the view shows what fits */
#define MAX_BPS 128
#define HISTORY_MAX 64
#define GUTTER_W 18          /* the disassembly's breakpoint column */
#define COMMENT_COL 62       /* where a disassembly line's comment starts, in characters */

#define WM_DBG_ACCEPT  (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB     (WM_APP + 3)   /* Tab in the prompt: complete */
#define WM_DBG_HISTORY (WM_APP + 4)   /* wp: 0 Up (older), 1 Down (newer) in the prompt */

#define TIMER_REFRESH 1

enum {
    PAGE_PROMPT = 0, PAGE_CPU, PAGE_DISASM, PAGE_VDP, PAGE_SOUND, PAGE_BREAKS, PAGE_CART,
    PAGE_COUNT
};

#define NREGS  17
#define NFLAGS 6
#define NSAVE  7

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_SCAN, IDC_FRAME,
    IDC_STATUS, IDC_TABS,
    IDC_PROMPT_OUT, IDC_PROMPT_IN, IDC_LOAD_SYMBOLS, IDC_SAVE,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + NREGS - 1,
    IDC_FLAG0, IDC_FLAG_LAST = IDC_FLAG0 + NFLAGS - 1,
    IDC_BEAM, IDC_RAM_VIEW, IDC_RAM_GOTO, IDC_RAM_ADDR, IDC_RAM_VAL,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM,
    IDC_VDP_TEXT, IDC_VDP_VIEW, IDC_VDP_PALETTE, IDC_VDP_PIC, IDC_SPRITES,
    IDC_CRAM, IDC_CRAM_INDEX, IDC_CRAM_VAL,
    IDC_SOUND,
    IDC_BP_LIST, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END, IDC_BP_COND, IDC_BP_ADD,
    IDC_BP_REMOVE, IDC_BP_ENABLE, IDC_BP_CLEAR,
    IDC_CART,
    IDC_SAVE_KIND0, IDC_SAVE_KIND_LAST = IDC_SAVE_KIND0 + NSAVE - 1,
    IDC_SYMS_CART, IDC_SYMS_FILE,
    IDC_LABEL_FIRST
};

typedef struct {
    HWND hwnd;
    HWND tabs;
    HWND run_btn, step_btn, over_btn, out_btn, scan_btn, frame_btn, status;

    HWND prompt_out, prompt_in, load_symbols, save;

    HWND reg_label[NREGS], reg_edit[NREGS], flag[NFLAGS], beam, ram_label, ram_view;
    HWND ram_goto_label, ram_goto, ram_addr_label, ram_addr, ram_val_label, ram_val;

    HWND follow_pc, jump_label, jump, disasm_hint, disasm;

    HWND vdp_text, vdp_view_label, vdp_view, vdp_pal_label, vdp_palette, vdp_pic, sprites;
    HWND cram_label, cram, cram_index_label, cram_index, cram_val_label, cram_val;

    HWND sound;

    HWND bp_list, bp_type, bp_start_label, bp_start, bp_end_label, bp_end, bp_cond_label, bp_cond;
    HWND bp_add, bp_remove, bp_enable, bp_clear;

    HWND cart;

    HFONT mono, ui;
    int mono_w, mono_h;        /* the mono font's cell, for the hand-drawn views */
    HACCEL accel;
    HBRUSH accent;

    smssession *session;
    smsdebug *dbg;

    int page;
    unsigned seen_gen;
    int was_stopped;
    int running_ticks;

    int disasm_top;            /* address of the first disassembly line */
    smsdebug_line lines[DISASM_MAX];
    int line_count;
    int pc_line;

    smsdebug_breakpoint bps[MAX_BPS];
    int nbps;

    smsdebug_vdp vdp;          /* the last VDP snapshot, for the CRAM swatches */
    int cram_sel;

    char history[HISTORY_MAX][256];
    int nhistory, history_pos;

    uint32_t *pic_px;
    int pic_w, pic_h;
} debugger;

static debugger *g_dbg;

static const char *const kSaveKinds[NSAVE] = { "dis", "ram", "vram", "cram", "sram", "arena", "regs" };
static const char *const kSaveTitles[NSAVE] = { "Disassembly ($0000-$BFFF)...", "Console RAM...", "VRAM...",
                                                "CRAM...", "Cartridge SRAM...", "Mailbox arena...",
                                                "Registers (text)..." };

/* The editable registers, in the order the CPU page lays them out. */
static const char *const kRegNames[NREGS] = { "PC", "SP", "AF", "BC", "DE", "HL", "IX", "IY",
                                              "AF'", "BC'", "DE'", "HL'", "I", "R", "IM", "IFF1", "IFF2" };
static const int kRegIds[NREGS] = { SMS_REG_PC, SMS_REG_SP, SMS_REG_AF, SMS_REG_BC, SMS_REG_DE, SMS_REG_HL,
                                    SMS_REG_IX, SMS_REG_IY, SMS_REG_AF2, SMS_REG_BC2, SMS_REG_DE2, SMS_REG_HL2,
                                    SMS_REG_I, SMS_REG_R, SMS_REG_IM, SMS_REG_IFF1, SMS_REG_IFF2 };

/* The page names SMS_DEBUGGER_TAB accepts, by page. */
static const char *const kPageNames[PAGE_COUNT] = { "prompt", "cpu", "disassembly", "vdp", "sound",
                                                    "breakpoints", "cart" };

/* ---- helpers ---------------------------------------------------------------- */

static void set_text(HWND h, const char *text) { SetWindowTextA(h, text); }

/* EDIT controls want CRLF and render control characters as boxes. */
static char *to_crlf(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    return buf;
}

static void set_text_lf(HWND h, const char *text)
{
    char *buf = to_crlf(text);
    set_text(h, buf ? buf : text);
    free(buf);
}

/* For the live views: replace the text but keep the scroll position. */
static void set_text_lf_keep(HWND h, const char *text)
{
    const int first = (int)SendMessageA(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    set_text_lf(h, text);
    SendMessageA(h, EM_LINESCROLL, 0, first);
}

static void append_text_lf(HWND h, const char *text)
{
    char *buf = to_crlf(text);
    int len;
    if (!buf) return;
    len = GetWindowTextLengthA(h);
    SendMessageA(h, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(h, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageA(h, EM_SCROLLCARET, 0, 0);
    free(buf);
}

static void edit_text(HWND h, char *out, int outsz) { GetWindowTextA(h, out, outsz); }

/* $hex, 0xhex, #dec, or bare hex, as the prompt reads numbers. */
static int parse_num(const char *text, long *out)
{
    char buf[64];
    const char *p;
    char *end;
    long v;
    int base = 16;

    snprintf(buf, sizeof buf, "%s", text);
    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if (*p == '#') { p++; base = 10; }
    if (!*p) return 0;
    v = strtol(p, &end, base);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end) return 0;
    *out = v;
    return 1;
}

static int resolve_addr(debugger *d, const char *text)
{
    long v;
    int addr = -1;
    const char *p = text;
    while (*p == ' ') p++;
    if (!*p) return -1;
    if (*p != '$' && *p != '#' && !(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')))
        addr = smsdebug_label_address(d->dbg, p);
    if (addr < 0 && parse_num(p, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

/* The disassembly rows the view has room for. */
static int disasm_rows(debugger *d)
{
    RECT c;
    int rows;
    if (!d->disasm || d->mono_h <= 0) return 1;
    GetClientRect(d->disasm, &c);
    rows = (c.bottom - c.top) / d->mono_h;
    if (rows < 1) rows = 1;
    if (rows > DISASM_MAX) rows = DISASM_MAX;
    return rows;
}

/* ---- refreshers -------------------------------------------------------------- */

static void refresh_status(debugger *d)
{
    char reason[160], buf[220];
    int addr;
    const int stopped = smsdebug_is_stopped(d->dbg);
    smsdebug_stop_reason(d->dbg, reason, sizeof reason, &addr);
    if (stopped) snprintf(buf, sizeof buf, "Stopped%s%s", reason[0] ? ": " : "", reason);
    else snprintf(buf, sizeof buf, "Running");
    set_text(d->status, buf);
    set_text(d->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(d->run_btn, NULL, FALSE);
}

static void refresh_cpu(debugger *d)
{
    smsdebug_cpu c;
    char buf[200];
    int i;
    smsdebug_cpu_get(d->dbg, &c);
    {
        const int v[NREGS] = { c.pc, c.sp, c.af, c.bc, c.de, c.hl, c.ix, c.iy,
                               c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im, c.iff1, c.iff2 };
        for (i = 0; i < NREGS; i++) {
            if (GetFocus() == d->reg_edit[i]) continue;   /* do not fight the user's typing */
            /* the register pairs as four digits, I and R as two, IM and the
             * interrupt flip-flops as they are */
            snprintf(buf, sizeof buf, i < 12 ? "%04X" : i < 14 ? "%02X" : "%d", v[i]);
            set_text(d->reg_edit[i], buf);
        }
    }
    {
        const int flags[NFLAGS] = { c.sf, c.zf, c.hf, c.pf, c.nf, c.cf };
        for (i = 0; i < NFLAGS; i++)
            SendMessageA(d->flag[i], BM_SETCHECK, flags[i] ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    snprintf(buf, sizeof buf, "line %d  dot %d   V counter $%02X  H counter $%02X   frame %u   cycle %llu%s%s%s",
             c.vpos, c.hpos, c.vcount & 0xff, c.hcount & 0xff, c.frame, (unsigned long long)c.cycles,
             c.int_line ? "   INT" : "", c.nmi_line ? "   NMI" : "", c.halted ? "   HALT" : "");
    set_text(d->beam, buf);
}

static void refresh_ram(debugger *d)
{
    static uint8_t ram[8192];
    static char text[8192 / 16 * 64 + 128];
    size_t len = 0;
    int row, col;
    smsdebug_ram_get(d->dbg, ram);
    len += (size_t)snprintf(text + len, sizeof text - len,
                            "        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\r\n");
    for (row = 0; row < 8192 / 16; row++) {
        len += (size_t)snprintf(text + len, sizeof text - len, "$%04X: ", 0xC000 + row * 16);
        for (col = 0; col < 16; col++)
            len += (size_t)snprintf(text + len, sizeof text - len, "%02X ", ram[row * 16 + col]);
        len += (size_t)snprintf(text + len, sizeof text - len, "\r\n");
    }
    {
        /* keep the scroll position across a refresh */
        int first = (int)SendMessageA(d->ram_view, EM_GETFIRSTVISIBLELINE, 0, 0);
        set_text(d->ram_view, text);
        SendMessageA(d->ram_view, EM_LINESCROLL, 0, first);
    }
}

static void refresh_disasm(debugger *d)
{
    smsdebug_cpu c;
    int pc_line = -1;
    const int rows = disasm_rows(d);
    const int follow = SendMessageA(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (follow) {
        /* the PC a third of the way down, so what led to it shows too */
        smsdebug_cpu_get(d->dbg, &c);
        d->disasm_top = smsdebug_row_address(d->dbg, (uint16_t)c.pc, -(rows / 3));
    }
    d->line_count = smsdebug_disassemble(d->dbg, (uint16_t)d->disasm_top, d->lines, rows, &pc_line);
    d->pc_line = pc_line;
    InvalidateRect(d->disasm, NULL, FALSE);
}

static void refresh_vdp(debugger *d)
{
    smsdebug_vdp *v = &d->vdp;
    smsdebug_sprite sp[64];
    static char s[4096];
    char desc[160];
    size_t len = 0;
    int i, n, view, pal;

    smsdebug_vdp_get(d->dbg, v);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "%s VDP, %s, %d lines, %s\n"
        "Frame %u   line %d   dot %d   V counter $%02X\n\n",
        v->kind_5246 ? "315-5246" : "315-5124", v->mode_name ? v->mode_name : "?", v->y_pixels,
        v->is_pal ? "PAL" : "NTSC", v->frame, v->vpos, v->hpos, v->vcount);
    for (i = 0; i < 11; i++) {
        smsdebug_vdp_describe_register(d->dbg, i, desc, sizeof desc);
        len += (size_t)snprintf(s + len, sizeof s - len, "%s\n", desc);
    }
    len += (size_t)snprintf(s + len, sizeof s - len,
        "\nStatus $%02X   address $%04X   code %d   read buffer $%02X%s\n"
        "Line counter $%02X   H counter $%02X\n",
        v->status, v->addr, v->code, v->buffer, v->second_byte ? "   (control write half done)" : "",
        v->line_counter, v->hcounter);
    if (v->mode == 4)
        len += (size_t)snprintf(s + len, sizeof s - len,
            "Name table $%04X   sprite table $%04X   sprite patterns $%04X\n",
            v->name_base, v->sat_base, v->sprite_pattern_base);
    else
        len += (size_t)snprintf(s + len, sizeof s - len,
            "Name table $%04X   colour table $%04X   patterns $%04X\n"
            "Sprite table $%04X   sprite patterns $%04X\n",
            v->name_base, v->color_base, v->pattern_base, v->sat_base, v->sprite_pattern_base);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Scroll X %d   Y %d   backdrop %d\n"
        "Display %s   VINT %s   HINT %s   sprites %s%s\n"
        "Left column %s   top rows %s   right columns %s   sprite shift %s\n"
        "Pending VINT %d   HINT %d   /INT %s   /NMI %s   Pause %s\n",
        v->scroll_x, v->scroll_y, v->backdrop,
        v->display_on ? "on" : "off", v->vint_on ? "on" : "off", v->hint_on ? "on" : "off",
        v->sprites_16 ? "8x16" : "8x8", v->sprites_zoom ? " zoomed" : "",
        v->left_column_blank ? "blanked" : "shown", v->hscroll_lock_top ? "fixed" : "scroll",
        v->vscroll_lock_right ? "fixed" : "scroll", v->sprite_shift ? "on" : "off",
        v->vint_pending, v->hint_pending, v->int_line ? "low" : "high", v->nmi_line ? "low" : "high",
        v->pause_held ? "held" : "up");
    set_text_lf_keep(d->vdp_text, s);

    /* The sprite table: colour and the early clock only mean something in
     * the TMS9918 modes. */
    n = smsdebug_sprites_get(d->dbg, sp);
    len = 0;
    if (v->mode == 4) {
        len += (size_t)snprintf(s + len, sizeof s - len, " #    Y    X  tile\n");
        for (i = 0; i < n; i++)
            len += (size_t)snprintf(s + len, sizeof s - len, "%2d  %3d  %3d  $%03X%s\n", i, sp[i].y, sp[i].x,
                                    sp[i].tile, sp[i].visible ? "" : "  (hidden)");
    } else {
        len += (size_t)snprintf(s + len, sizeof s - len, " #    Y    X  tile  colour\n");
        for (i = 0; i < n; i++)
            len += (size_t)snprintf(s + len, sizeof s - len, "%2d  %3d  %3d  $%02X   %2d%s%s\n", i, sp[i].y, sp[i].x,
                                    sp[i].tile, sp[i].color, sp[i].early_clock ? "  EC" : "",
                                    sp[i].visible ? "" : "  (hidden)");
    }
    if (n == 0) len += (size_t)snprintf(s + len, sizeof s - len, "(no sprites)\n");
    set_text_lf_keep(d->sprites, s);

    view = (int)SendMessageA(d->vdp_view, CB_GETCURSEL, 0, 0);
    pal = (int)SendMessageA(d->vdp_palette, CB_GETCURSEL, 0, 0);
    if (view < 0) view = 0;
    if (pal < 0) pal = 0;
    EnableWindow(d->vdp_palette, view == SMSDEBUG_VIEW_TILES);
    if (!smsdebug_vdp_view(d->dbg, view, pal, d->pic_px, &d->pic_w, &d->pic_h))
        d->pic_w = d->pic_h = 0;
    InvalidateRect(d->vdp_pic, NULL, FALSE);

    /* CRAM: the swatches, and the selected entry's value unless it is
     * being typed. */
    InvalidateRect(d->cram, NULL, FALSE);
    if (GetFocus() != d->cram_index && GetFocus() != d->cram_val) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", d->cram_sel);
        set_text(d->cram_index, buf);
        snprintf(buf, sizeof buf, "%02X", v->cram[d->cram_sel & 31]);
        set_text(d->cram_val, buf);
    }
}

static void refresh_sound(debugger *d)
{
    static const char *const pad_names[6] = { "Up", "Down", "Left", "Right", "1", "2" };
    smsdebug_io io;
    static char s[4096];
    size_t len = 0;
    int i, port, b;

    smsdebug_io_get(d->dbg, &io);
    len += (size_t)snprintf(s + len, sizeof s - len, "PSG%s\n", io.psg_audible ? "" : "   (muted)");
    for (i = 0; i < 3; i++)
        len += (size_t)snprintf(s + len, sizeof s - len, "  Tone %d   period %4d ($%03X)   volume %2d%s\n", i,
                                io.tone_period[i], io.tone_period[i] & 0x3ff, io.tone_volume[i],
                                io.tone_volume[i] >= 15 ? "  (off)" : "");
    len += (size_t)snprintf(s + len, sizeof s - len,
        "  Noise    %s, rate %d%s   volume %2d%s\n"
        "  LFSR     $%04X\n\n",
        io.noise_mode ? "white" : "periodic", io.noise_rate, io.noise_rate == 3 ? " (tone 2's)" : "",
        io.noise_volume, io.noise_volume >= 15 ? "  (off)" : "", io.lfsr);
    if (!io.fm_present) {
        len += (size_t)snprintf(s + len, sizeof s - len, "YM2413   none on this console\n");
    } else {
        int row, col;
        len += (size_t)snprintf(s + len, sizeof s - len, "YM2413%s\n"
                                "         0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n",
                                io.fm_audible ? "" : "   (muted)");
        for (row = 0; row < 4; row++) {
            len += (size_t)snprintf(s + len, sizeof s - len, "  $%02X:  ", row * 16);
            for (col = 0; col < 16; col++)
                len += (size_t)snprintf(s + len, sizeof s - len, "%02X ", io.fm_regs[row * 16 + col]);
            len += (size_t)snprintf(s + len, sizeof s - len, "\n");
        }
    }
    len += (size_t)snprintf(s + len, sizeof s - len,
        "\nMemory control  $3E = $%02X   cartridge %s, BIOS %s, work RAM %s, I/O chip %s\n"
        "I/O control     $3F = $%02X\n"
        "BIOS            %s   pages $%02X $%02X $%02X\n"
        "Mapper          $FFFC=$%02X  $FFFD=$%02X  $FFFE=$%02X  $FFFF=$%02X\n"
        "Ports           $DC=$%02X  $DD=$%02X\n",
        io.mem_ctrl, io.cart_enabled ? "on" : "off", io.bios_enabled ? "on" : "off",
        io.ram_enabled ? "on" : "off", io.io_enabled ? "on" : "off", io.io_ctrl,
        io.bios_present ? "present" : "none", io.bios_page[0], io.bios_page[1], io.bios_page[2],
        io.mapper[0], io.mapper[1], io.mapper[2], io.mapper[3], io.port_dc, io.port_dd);
    for (port = 0; port < 2; port++) {
        len += (size_t)snprintf(s + len, sizeof s - len, "Joypad %d        $%02X ", port + 1, io.pad[port]);
        for (b = 0; b < 6; b++)
            if (io.pad[port] & (1 << b))
                len += (size_t)snprintf(s + len, sizeof s - len, " %s", pad_names[b]);
        len += (size_t)snprintf(s + len, sizeof s - len, "\n");
    }
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Pause           %s   Reset %s\n"
        "Nationality     %s\n"
        "Console         %s\n",
        io.pause_held ? "held" : "up", io.reset_held ? "held" : "up",
        io.japanese ? "Japanese" : "export", io.console_name ? io.console_name : "?");
    set_text_lf_keep(d->sound, s);
}

static void refresh_bps(debugger *d)
{
    int i, sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
    d->nbps = smsdebug_breakpoint_list(d->dbg, d->bps, MAX_BPS);
    SendMessageA(d->bp_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < d->nbps; i++) {
        const smsdebug_breakpoint *b = &d->bps[i];
        const int port = (b->type & (SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT)) != 0;
        char line[256], kind[8] = "", range[24];
        if (b->type & SMSDEBUG_BP_EXEC) strcat(kind, "x");
        if (b->type & SMSDEBUG_BP_READ) strcat(kind, "r");
        if (b->type & SMSDEBUG_BP_WRITE) strcat(kind, "w");
        if (b->type & SMSDEBUG_BP_IN) strcat(kind, "i");
        if (b->type & SMSDEBUG_BP_OUT) strcat(kind, "o");
        /* ports are a byte, addresses a word */
        if (b->start == b->end) snprintf(range, sizeof range, port ? "$%02X" : "$%04X", b->start);
        else snprintf(range, sizeof range, port ? "$%02X-$%02X" : "$%04X-$%04X", b->start, b->end);
        snprintf(line, sizeof line, "%c %3d  %-3s  %-12s  hits %-6u %s%s", b->enabled ? '+' : '-',
                 b->id, kind, range, b->hits, b->condition[0] ? "if " : "", b->condition);
        SendMessageA(d->bp_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (d->nbps == 0) SendMessageA(d->bp_list, LB_ADDSTRING, 0, (LPARAM)"(no breakpoints -- click a disassembly line, or add one below)");
    if (sel >= 0 && sel < d->nbps) SendMessageA(d->bp_list, LB_SETCURSEL, (WPARAM)sel, 0);
}

static void refresh_cart(debugger *d)
{
    smsdebug_cart c;
    char info[256];
    static char s[4096];
    size_t len = 0;
    int i;
    smsdebug_cart_get(d->dbg, &c);
    smsdebug_cart_info(d->dbg, info, sizeof info);
    if (!c.present) { set_text_lf(d->cart, "No FujiNet cartridge is running."); return; }
    len += (size_t)snprintf(s + len, sizeof s - len,
        "%s\n\n"
        "Mode            %s%s\n"
        "Link            %s%s\n",
        info, c.mode_name ? c.mode_name : "?", c.booted_game ? " (a game has been booted)" : "",
        c.link_up ? "up" : "down", c.busy ? " (transaction in flight)" : "");
    if (c.mapper < 0)
        len += (size_t)snprintf(s + len, sizeof s - len, "Mapper          none (CONFIG)\n");
    else
        len += (size_t)snprintf(s + len, sizeof s - len,
            "Mapper          %s   banks $%02X $%02X $%02X $%02X $%02X $%02X\n",
            c.mapper_name, c.bank[0], c.bank[1], c.bank[2], c.bank[3], c.bank[4], c.bank[5]);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Cartridge RAM   %s%s   %u bytes\n"
        "Image           %u bytes   CRC-32 %08X%s\n"
        "Claim           %d\n\n"
        "Mailbox         ACKSEQ $%02X   STATUS $%02X   last error %u   reply cmd $%02X   RXLEN %u\n"
        "Queue           %u deep\n"
        "Boot            state %d   %d%%   error %d   %u of %u bytes\n"
        "Load            state %d   window %d of %d   %d%%\n\n"
        "BIOS snoop      phase %d   $C000=$%02X   $3E=$%02X   $3F=$%02X\n"
        "  VDP R0-R10   ",
        c.ram_enabled ? "enabled" : "off", c.ram_writable ? ", writable" : "", c.ram_size,
        c.image_size, c.image_crc, c.direct ? " (opened file)" : "",
        c.claim,
        c.ackseq, c.status, c.last_error, c.reply_cmd, c.rxlen,
        c.queue_depth,
        c.boot_state, c.boot_pct, c.boot_err, c.boot_got, c.boot_total,
        c.load_state, c.load_win, c.load_nwin, c.load_pct,
        c.bios_phase, c.snoop_c000, c.snoop_3e, c.snoop_3f);
    for (i = 0; i < 11; i++)
        len += (size_t)snprintf(s + len, sizeof s - len, " %02X", c.snoop_vdp[i]);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "\n\nSRAM bank behind each 1K page (-- = the cartridge's own memory)\n");
    for (i = 0; i < 48; i++) {
        if (i % 16 == 0) len += (size_t)snprintf(s + len, sizeof s - len, "  $%04X ", i * 0x400);
        if (c.page_bank[i] < 0) len += (size_t)snprintf(s + len, sizeof s - len, " --");
        else len += (size_t)snprintf(s + len, sizeof s - len, " %02X", c.page_bank[i] & 0xff);
        if (i % 16 == 15) len += (size_t)snprintf(s + len, sizeof s - len, "\n");
    }
    set_text_lf_keep(d->cart, s);
}

static void refresh_all(debugger *d)
{
    refresh_status(d);
    refresh_cpu(d);
    refresh_ram(d);
    refresh_disasm(d);
    refresh_vdp(d);
    refresh_sound(d);
    refresh_bps(d);
    refresh_cart(d);
}

/* ---- actions ------------------------------------------------------------------ */

static void toggle_run(debugger *d)
{
    if (smsdebug_is_stopped(d->dbg)) smsdebug_resume(d->dbg);
    else smsdebug_stop(d->dbg);
    refresh_all(d);
}

static void run_prompt(debugger *d)
{
    static char out[65536];
    char cmd[256], echo[300];
    edit_text(d->prompt_in, cmd, sizeof cmd);
    if (!cmd[0]) return;
    /* history: the newest last, no repeats of the one before */
    if (d->nhistory == 0 || strcmp(d->history[d->nhistory - 1], cmd) != 0) {
        if (d->nhistory == HISTORY_MAX) {
            memmove(d->history[0], d->history[1], sizeof d->history[0] * (HISTORY_MAX - 1));
            d->nhistory--;
        }
        snprintf(d->history[d->nhistory++], sizeof d->history[0], "%s", cmd);
    }
    d->history_pos = d->nhistory;
    snprintf(echo, sizeof echo, "> %s\n", cmd);
    append_text_lf(d->prompt_out, echo);
    smsdebug_command(d->dbg, cmd, out, sizeof out);
    append_text_lf(d->prompt_out, out);
    append_text_lf(d->prompt_out, "\n");
    set_text(d->prompt_in, "");
    refresh_all(d);
}

/* Up / Down in the prompt walk the commands already run; past the newest is
 * an empty line again. */
static void history_step(debugger *d, int newer)
{
    int len;
    if (d->nhistory == 0) return;
    d->history_pos += newer ? 1 : -1;
    if (d->history_pos < 0) d->history_pos = 0;
    if (d->history_pos >= d->nhistory) {
        d->history_pos = d->nhistory;
        set_text(d->prompt_in, "");
        return;
    }
    set_text(d->prompt_in, d->history[d->history_pos]);
    len = GetWindowTextLengthA(d->prompt_in);
    SendMessageA(d->prompt_in, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

static void complete_prompt(debugger *d)
{
    char text[256], comps[4096];
    const char *word;
    char *sp;
    int n;
    edit_text(d->prompt_in, text, sizeof text);
    sp = strrchr(text, ' ');
    word = sp ? sp + 1 : text;
    n = smsdebug_completions(d->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char line[256], joined[520];
        char *nl;
        snprintf(line, sizeof line, "%.255s", comps);
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (sp) sp[1] = '\0'; else text[0] = '\0';
        snprintf(joined, sizeof joined, "%s%s ", text, line);
        set_text(d->prompt_in, joined);
        SendMessageA(d->prompt_in, EM_SETSEL, (WPARAM)strlen(joined), (LPARAM)strlen(joined));
    } else if (n > 1) {
        append_text_lf(d->prompt_out, comps);
    }
}

static void jump_to(debugger *d, const char *text)
{
    const int addr = resolve_addr(d, text);
    if (addr < 0) {
        append_text_lf(d->prompt_out, "No such label or address.\n");
        return;
    }
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->disasm_top = addr;
    refresh_disasm(d);
}

/* Scroll the RAM dump to an address (or label) in console RAM or its
 * mirror at $E000. */
static void ram_goto(debugger *d, const char *text)
{
    const int addr = resolve_addr(d, text);
    int line, first;
    if (addr < 0xC000) {
        append_text_lf(d->prompt_out, addr < 0 ? "No such label or address.\n"
                                               : "Console RAM is $C000-$DFFF (mirrored at $E000-$FFFF).\n");
        return;
    }
    line = ((addr - 0xC000) & 0x1FFF) / 16 + 1;   /* + the column header */
    first = (int)SendMessageA(d->ram_view, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageA(d->ram_view, EM_LINESCROLL, 0, line - first);
}

static int pick_file(debugger *d, int save, const char *title, const char *filter, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void save_file(debugger *d, int which)
{
    char path[MAX_PATH], msg[512], title[64];
    snprintf(title, sizeof title, "Save %s", kSaveTitles[which]);
    if (!pick_file(d, 1, title, "All files\0*.*\0\0", path, sizeof path)) return;
    smsdebug_save(d->dbg, kSaveKinds[which], path, msg, sizeof msg);
    append_text_lf(d->prompt_out, msg);
    append_text_lf(d->prompt_out, "\n");
}

static void popup_below(debugger *d, HWND button, HMENU m)
{
    RECT r;
    GetWindowRect(button, &r);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, d->hwnd, NULL);
    DestroyMenu(m);
}

static void save_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    int i;
    for (i = 0; i < NSAVE; i++) AppendMenuA(m, MF_STRING, (UINT_PTR)(IDC_SAVE_KIND0 + i), kSaveTitles[i]);
    popup_below(d, d->save, m);
}

/* Symbols come from a file the user picks, or from the one beside the
 * opened cartridge (game.sym / .map / .noi next to game.sms). */
static void symbols_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, IDC_SYMS_FILE, "From a file...");
    AppendMenuA(m, MF_STRING, IDC_SYMS_CART, "Beside the cartridge");
    popup_below(d, d->load_symbols, m);
}

static void load_symbols(debugger *d, int beside_cart)
{
    char path[MAX_PATH], msg[512];
    if (!beside_cart &&
        !pick_file(d, 0, "Load symbols",
                   "Symbol files (*.sym;*.map;*.noi;*.txt)\0*.sym;*.map;*.noi;*.txt\0All files\0*.*\0\0",
                   path, sizeof path))
        return;
    smsdebug_load_symbols(d->dbg, beside_cart ? NULL : path, msg, sizeof msg);
    append_text_lf(d->prompt_out, msg);
    append_text_lf(d->prompt_out, "\n");
    refresh_all(d);
}

static void add_breakpoint(debugger *d)
{
    char a[64], b[64], cond[256];
    static const int types[6] = { SMSDEBUG_BP_EXEC, SMSDEBUG_BP_READ, SMSDEBUG_BP_WRITE,
                                  SMSDEBUG_BP_READ | SMSDEBUG_BP_WRITE, SMSDEBUG_BP_IN, SMSDEBUG_BP_OUT };
    int start, end, sel, id, port;
    edit_text(d->bp_start, a, sizeof a);
    edit_text(d->bp_end, b, sizeof b);
    edit_text(d->bp_cond, cond, sizeof cond);
    sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 5) sel = 0;
    port = (types[sel] & (SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT)) != 0;
    start = resolve_addr(d, a);
    if (start < 0) { MessageBoxA(d->hwnd, "The start is not an address or label.", "Breakpoint", MB_ICONWARNING); return; }
    end = b[0] ? resolve_addr(d, b) : start;
    if (end < start) end = start;
    if (port && end > 0xFF) { MessageBoxA(d->hwnd, "An I/O port is $00-$FF.", "Breakpoint", MB_ICONWARNING); return; }
    id = smsdebug_breakpoint_add(d->dbg, types[sel], (uint16_t)start, (uint16_t)end, cond);
    if (id < 0) { MessageBoxA(d->hwnd, "The condition does not parse.", "Breakpoint", MB_ICONWARNING); return; }
    set_text(d->bp_start, "");
    set_text(d->bp_end, "");
    set_text(d->bp_cond, "");
    refresh_bps(d);
    refresh_disasm(d);
}

static void on_accept(debugger *d, int id)
{
    char buf[512];
    long v;

    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        edit_text(d->reg_edit[id - IDC_REG0], buf, sizeof buf);
        if (parse_num(buf, &v)) smsdebug_cpu_set(d->dbg, kRegIds[id - IDC_REG0], (int)v);
        SetFocus(d->hwnd);
        refresh_all(d);
        return;
    }
    switch (id) {
    case IDC_PROMPT_IN: run_prompt(d); break;
    case IDC_RAM_GOTO:
        edit_text(d->ram_goto, buf, sizeof buf);
        ram_goto(d, buf);
        break;
    case IDC_RAM_ADDR:
    case IDC_RAM_VAL: {
        char abuf[64];
        int a;
        edit_text(d->ram_addr, abuf, sizeof abuf);
        edit_text(d->ram_val, buf, sizeof buf);
        a = resolve_addr(d, abuf);
        if (a >= 0 && parse_num(buf, &v)) smsdebug_write(d->dbg, (uint16_t)a, (uint8_t)v);
        refresh_all(d);
        break;
    }
    case IDC_CRAM_INDEX:
    case IDC_CRAM_VAL: {
        /* the entry is a plain decimal 0-31, the value hex as anywhere else */
        char ibuf[16], *end;
        long idx;
        edit_text(d->cram_index, ibuf, sizeof ibuf);
        edit_text(d->cram_val, buf, sizeof buf);
        idx = strtol(ibuf, &end, 10);
        if (end != ibuf && idx >= 0 && idx < 32 && parse_num(buf, &v)) {
            d->cram_sel = (int)idx;
            smsdebug_cram_write(d->dbg, (int)idx, (uint8_t)v);
        }
        SetFocus(d->hwnd);
        refresh_vdp(d);
        break;
    }
    case IDC_JUMP:
        edit_text(d->jump, buf, sizeof buf);
        jump_to(d, buf);
        break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND:
        add_breakpoint(d);
        break;
    default: break;
    }
}

/* ---- the VDP picture --------------------------------------------------------------- */

static LRESULT CALLBACK pic_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        GetClientRect(hwnd, &c);
        FillRect(dc, &c, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
        if (d && d->pic_px && d->pic_w > 0 && d->pic_h > 0) {
            BITMAPINFO bmi;
            int scale = 2, w, h;
            /* 2x nearest when it fits, else 1x: a tile's pixels stay whole */
            if (d->pic_w * 2 > c.right || d->pic_h * 2 > c.bottom) scale = 1;
            w = d->pic_w * scale;
            h = d->pic_h * scale;
            memset(&bmi, 0, sizeof bmi);
            bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
            bmi.bmiHeader.biWidth = d->pic_w;
            bmi.bmiHeader.biHeight = -d->pic_h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, (c.right - w) / 2 > 0 ? (c.right - w) / 2 : 0, 0, w, h,
                          0, 0, d->pic_w, d->pic_h, d->pic_px, &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- the CRAM swatches -------------------------------------------------------------- */

/* 32 entries in two rows of 16 -- the background palette over the sprite
 * palette -- each a swatch of its colour with its value under it. A click
 * picks the entry for the value field below. */
static void cram_cell(HWND hwnd, int i, RECT *rc)
{
    RECT c;
    int cw, ch;
    GetClientRect(hwnd, &c);
    cw = c.right / 16;
    ch = c.bottom / 2;
    SetRect(rc, (i % 16) * cw, (i / 16) * ch, (i % 16) * cw + cw, (i / 16) * ch + ch);
}

static LRESULT CALLBACK cram_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_PAINT && d) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        int i;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, d->mono);
        FillRect(mem, &c, GetSysColorBrush(COLOR_BTNFACE));
        SetBkMode(mem, TRANSPARENT);
        for (i = 0; i < 32; i++) {
            RECT cell, sw, text;
            const uint32_t rgb = d->vdp.cram_rgb[i];
            HBRUSH b = CreateSolidBrush(RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff));
            char val[8];
            cram_cell(hwnd, i, &cell);
            SetRect(&sw, cell.left + 2, cell.top + 2, cell.right - 2, cell.bottom - d->mono_h - 2);
            FillRect(mem, &sw, b);
            FrameRect(mem, &sw, (HBRUSH)GetStockObject(GRAY_BRUSH));
            DeleteObject(b);
            if (i == d->cram_sel) {
                RECT sel = cell;
                FrameRect(mem, &sel, d->accent);
                InflateRect(&sel, -1, -1);
                FrameRect(mem, &sel, d->accent);
            }
            SetRect(&text, cell.left, cell.bottom - d->mono_h - 1, cell.right, cell.bottom);
            snprintf(val, sizeof val, "%02X", d->vdp.cram[i]);
            SetTextColor(mem, GetSysColor(COLOR_BTNTEXT));
            DrawTextA(mem, val, -1, &text, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        }
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_LBUTTONDOWN && d) {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int i;
        for (i = 0; i < 32; i++) {
            RECT cell;
            cram_cell(hwnd, i, &cell);
            if (PtInRect(&cell, p)) {
                char buf[16];
                d->cram_sel = i;
                snprintf(buf, sizeof buf, "%d", i);
                set_text(d->cram_index, buf);
                snprintf(buf, sizeof buf, "%02X", d->vdp.cram[i]);
                set_text(d->cram_val, buf);
                /* ready to type the new value and press Enter */
                SetFocus(d->cram_val);
                SendMessageA(d->cram_val, EM_SETSEL, 0, -1);
                InvalidateRect(hwnd, NULL, FALSE);
                break;
            }
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- the disassembly ---------------------------------------------------------------- */

/* Drawn by hand, one row per line: the breakpoint gutter, address, bytes,
 * label, instruction, and the comment (a jump target's label, a port's
 * name) in grey. The PC's line is filled with the accent. */
static void paint_disasm(debugger *d, HWND hwnd, HDC dc)
{
    RECT c;
    int i;
    GetClientRect(hwnd, &c);
    FillRect(dc, &c, GetSysColorBrush(COLOR_WINDOW));
    SelectObject(dc, d->mono);
    SetBkMode(dc, TRANSPARENT);
    if (d->line_count == 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        TextOutA(dc, GUTTER_W, 0, "(no disassembly)", 16);
        return;
    }
    for (i = 0; i < d->line_count; i++) {
        const smsdebug_line *l = &d->lines[i];
        const int y = i * d->mono_h;
        RECT row;
        char text[200];
        int len;
        if (y >= c.bottom) break;
        SetRect(&row, 0, y, c.right, y + d->mono_h);
        if (l->is_pc) {
            FillRect(dc, &row, d->accent);
            SetTextColor(dc, RGB(255, 255, 255));
        } else {
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        }
        if (l->has_breakpoint) {
            const int size = d->mono_h - 6 > 4 ? d->mono_h - 6 : 4;
            HGDIOBJ oldb = SelectObject(dc, l->is_pc ? GetStockObject(WHITE_BRUSH) : (HGDIOBJ)d->accent);
            HGDIOBJ oldp = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, 4, y + 3, 4 + size + 1, y + 3 + size + 1);
            SelectObject(dc, oldp);
            SelectObject(dc, oldb);
        }
        len = snprintf(text, sizeof text, "%04X  %-12.12s %-16.16s %s", l->address, l->bytes, l->label, l->disasm);
        if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
        TextOutA(dc, GUTTER_W, y, text, len);
        if (l->comment[0]) {
            len = snprintf(text, sizeof text, "; %s", l->comment);
            if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
            if (!l->is_pc) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            TextOutA(dc, GUTTER_W + d->mono_w * COMMENT_COL, y, text, len);
        }
    }
}

/* A click toggles the breakpoint on that line; the wheel and the keys
 * browse (and turn Follow PC off, as in the other frontends). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, d->mono);
        paint_disasm(d, hwnd, mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        /* more or fewer rows fit now */
        if (d->disasm == hwnd) refresh_disasm(d);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN: {
        const int line = d->mono_h > 0 ? GET_Y_LPARAM(lp) / d->mono_h : -1;
        SetFocus(hwnd);
        if (line >= 0 && line < d->line_count) {
            smsdebug_breakpoint_toggle(d->dbg, d->lines[line].address);
            refresh_disasm(d);
            refresh_bps(d);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int rows = -GET_WHEEL_DELTA_WPARAM(wp) / 40;
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        if (rows) d->disasm_top = smsdebug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
        refresh_disasm(d);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
            const int page = disasm_rows(d) / 2 > 0 ? disasm_rows(d) / 2 : 1;
            int rows = wp == VK_PRIOR ? -page : wp == VK_NEXT ? page : wp == VK_UP ? -1 : 1;
            SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
            d->disasm_top = smsdebug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
            refresh_disasm(d);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;

/* Enter in a single-line field means "apply this value"; Tab in the prompt
 * completes and Up/Down recall. None may beep its way through the default
 * handler. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrA(hwnd, GWLP_ID);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_KEYDOWN && wp == VK_TAB) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_TAB, 0, 0);
        return 0;
    }
    if (id == IDC_PROMPT_IN && msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_HISTORY, wp == VK_DOWN, 0);
        return 0;
    }
    if (id == IDC_PROMPT_IN && msg == WM_CHAR && wp == VK_TAB) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

/* ---- construction ------------------------------------------------------------------ */

static HWND child(debugger *d, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExA(0, cls, text, WS_CHILD | style, 0, 0, 10, 10, d->hwnd,
                             (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND mono_view(debugger *d, int id, int wrap)
{
    return child(d, "EDIT", "",
                 WS_BORDER | WS_VSCROLL | (wrap ? 0 : WS_HSCROLL) | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                 id, d->mono);
}

static HWND field(debugger *d, int id, HFONT font)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id, font);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    return h;
}

static HWND label(debugger *d, const char *text)
{
    static int next_id;
    return child(d, "STATIC", text, SS_LEFT, IDC_LABEL_FIRST + next_id++, d->ui);
}

static HWND button(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, d->ui);
}

static HWND checkbox(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, d->ui);
}

static HWND combo(debugger *d, int id, const char *const *items, int n)
{
    HWND c = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, d->ui);
    int i;
    for (i = 0; i < n; i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageA(c, CB_SETCURSEL, 0, 0);
    return c;
}

static void build_controls(debugger *d)
{
    TCITEMA item;
    static const char *const tabs[PAGE_COUNT] = { "Prompt", "CPU && RAM", "Disassembly", "VDP",
                                                  "Sound && I/O", "Breakpoints", "Cart" };
    static const char *const flag_names[NFLAGS] = { "S", "Z", "H", "P/V", "N", "C" };
    static const char *const views[4] = { "Name table", "Tiles", "Sprites", "Palette" };
    static const char *const pals[2] = { "Background", "Sprites" };
    static const char *const bp_types[6] = { "Execute", "Read", "Write", "Read/Write", "Port in", "Port out" };
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    int i;

    d->tabs = child(d, WC_TABCONTROLA, "", WS_VISIBLE | WS_CLIPSIBLINGS, IDC_TABS, d->ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < PAGE_COUNT; i++) {
        item.pszText = (char *)tabs[i];
        SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
    }

    /* Toolbar: the stepping buttons, the symbol and save menus (they
     * matter on every page), then the stop reason. Run/Stop is owner-drawn
     * so it can wear the accent colour while the machine is stopped. */
    d->run_btn = child(d, "BUTTON", "Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, d->ui);
    d->step_btn = button(d, "Step (F7)", IDC_STEP);
    d->over_btn = button(d, "Over (F8)", IDC_OVER);
    d->out_btn = button(d, "Out (Shift+F8)", IDC_OUT);
    d->scan_btn = button(d, "Scanline+1", IDC_SCAN);
    d->frame_btn = button(d, "Frame+1", IDC_FRAME);
    d->load_symbols = button(d, "Load symbols...", IDC_LOAD_SYMBOLS);
    d->save = button(d, "Save...", IDC_SAVE);
    d->status = child(d, "STATIC", "Running", SS_RIGHT | SS_ENDELLIPSIS, IDC_STATUS, d->ui);
    {
        HWND bar[9] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->scan_btn, d->frame_btn,
                        d->load_symbols, d->save, d->status };
        for (i = 0; i < 9; i++) ShowWindow(bar[i], SW_SHOW);
    }

    /* Prompt */
    d->prompt_out = mono_view(d, IDC_PROMPT_OUT, 1);
    set_text(d->prompt_out, "Master System debugger prompt. Type 'help' for every command.\r\n");
    d->prompt_in = field(d, IDC_PROMPT_IN, d->mono);
    SendMessageA(d->prompt_in, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"command (help, step, break, bpw, print, mem, runto, ...) - Tab completes, Up/Down recall");

    /* CPU & RAM */
    for (i = 0; i < NREGS; i++) {
        d->reg_label[i] = label(d, kRegNames[i]);
        d->reg_edit[i] = field(d, IDC_REG0 + i, d->mono);
        SendMessageA(d->reg_edit[i], EM_SETLIMITTEXT, 6, 0);
    }
    for (i = 0; i < NFLAGS; i++) d->flag[i] = checkbox(d, flag_names[i], IDC_FLAG0 + i);
    d->beam = label(d, "");
    d->ram_label = label(d, "Console RAM ($C000-$DFFF)");
    d->ram_view = mono_view(d, IDC_RAM_VIEW, 0);
    d->ram_goto_label = label(d, "Go to");
    d->ram_goto = field(d, IDC_RAM_GOTO, d->mono);
    d->ram_addr_label = label(d, "Write address");
    d->ram_addr = field(d, IDC_RAM_ADDR, d->mono);
    d->ram_val_label = label(d, "value");
    d->ram_val = field(d, IDC_RAM_VAL, d->mono);

    /* Disassembly */
    d->follow_pc = checkbox(d, "Follow PC", IDC_FOLLOW_PC);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = label(d, "Jump to");
    d->jump = field(d, IDC_JUMP, d->mono);
    d->disasm_hint = label(d, "Click a line to toggle its breakpoint; scroll to browse");
    d->disasm = CreateWindowExA(WS_EX_CLIENTEDGE, "SMSDbgDisasm", "", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10,
                                d->hwnd, (HMENU)(INT_PTR)IDC_DISASM, inst, NULL);

    /* VDP */
    d->vdp_text = mono_view(d, IDC_VDP_TEXT, 0);
    d->sprites = mono_view(d, IDC_SPRITES, 0);
    d->vdp_view_label = label(d, "View");
    d->vdp_view = combo(d, IDC_VDP_VIEW, views, 4);
    d->vdp_pal_label = label(d, "Palette");
    d->vdp_palette = combo(d, IDC_VDP_PALETTE, pals, 2);
    d->vdp_pic = child(d, "SMSDbgPixels", "", 0, IDC_VDP_PIC, d->ui);
    d->cram_label = label(d, "CRAM (click an entry, type its new value, Enter)");
    d->cram = child(d, "SMSDbgCram", "", 0, IDC_CRAM, d->ui);
    d->cram_index_label = label(d, "Entry");
    d->cram_index = field(d, IDC_CRAM_INDEX, d->mono);
    d->cram_val_label = label(d, "value");
    d->cram_val = field(d, IDC_CRAM_VAL, d->mono);

    /* Sound & I/O */
    d->sound = mono_view(d, IDC_SOUND, 0);

    /* Breakpoints */
    d->bp_list = child(d, "LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, IDC_BP_LIST, d->mono);
    d->bp_type = combo(d, IDC_BP_TYPE, bp_types, 6);
    d->bp_start_label = label(d, "Address");
    d->bp_start = field(d, IDC_BP_START, d->mono);
    d->bp_end_label = label(d, "to");
    d->bp_end = field(d, IDC_BP_END, d->mono);
    d->bp_cond_label = label(d, "if");
    d->bp_cond = field(d, IDC_BP_COND, d->mono);
    SendMessageA(d->bp_cond, EM_SETCUEBANNER, TRUE, (LPARAM)L"optional condition, e.g. a == $FF");
    d->bp_add = button(d, "Add", IDC_BP_ADD);
    d->bp_remove = button(d, "Remove", IDC_BP_REMOVE);
    d->bp_enable = button(d, "Enable/Disable", IDC_BP_ENABLE);
    d->bp_clear = button(d, "Clear all", IDC_BP_CLEAR);

    /* Cart */
    d->cart = mono_view(d, IDC_CART, 0);
}

static void show_page(debugger *d, int page)
{
    HWND prompt[] = { d->prompt_out, d->prompt_in };
    HWND cpu[] = { d->beam, d->ram_label, d->ram_view, d->ram_goto_label, d->ram_goto,
                   d->ram_addr_label, d->ram_addr, d->ram_val_label, d->ram_val };
    HWND dis[] = { d->follow_pc, d->jump_label, d->jump, d->disasm_hint, d->disasm };
    HWND vdp[] = { d->vdp_text, d->sprites, d->vdp_view_label, d->vdp_view, d->vdp_pal_label, d->vdp_palette,
                   d->vdp_pic, d->cram_label, d->cram, d->cram_index_label, d->cram_index,
                   d->cram_val_label, d->cram_val };
    HWND brk[] = { d->bp_list, d->bp_type, d->bp_start_label, d->bp_start, d->bp_end_label, d->bp_end,
                   d->bp_cond_label, d->bp_cond, d->bp_add, d->bp_remove, d->bp_enable, d->bp_clear };
    size_t i;
    d->page = page;
#define SHOW(arr, p) for (i = 0; i < sizeof(arr) / sizeof((arr)[0]); i++) ShowWindow((arr)[i], page == (p) ? SW_SHOW : SW_HIDE)
    SHOW(prompt, PAGE_PROMPT);
    SHOW(cpu, PAGE_CPU);
    SHOW(d->reg_label, PAGE_CPU);
    SHOW(d->reg_edit, PAGE_CPU);
    SHOW(d->flag, PAGE_CPU);
    SHOW(dis, PAGE_DISASM);
    SHOW(vdp, PAGE_VDP);
    ShowWindow(d->sound, page == PAGE_SOUND ? SW_SHOW : SW_HIDE);
    SHOW(brk, PAGE_BREAKS);
    ShowWindow(d->cart, page == PAGE_CART ? SW_SHOW : SW_HIDE);
#undef SHOW
}

static void layout(debugger *d)
{
    RECT client, page;
    int y = 8, bx = 8, px, py, pw, ph, i;

    GetClientRect(d->hwnd, &client);

    MoveWindow(d->run_btn, bx, y, 96, 26, TRUE);     bx += 102;
    MoveWindow(d->step_btn, bx, y, 90, 26, TRUE);    bx += 96;
    MoveWindow(d->over_btn, bx, y, 90, 26, TRUE);    bx += 96;
    MoveWindow(d->out_btn, bx, y, 110, 26, TRUE);    bx += 116;
    MoveWindow(d->scan_btn, bx, y, 90, 26, TRUE);    bx += 96;
    MoveWindow(d->frame_btn, bx, y, 80, 26, TRUE);   bx += 92;
    MoveWindow(d->load_symbols, bx, y, 110, 26, TRUE); bx += 116;
    MoveWindow(d->save, bx, y, 80, 26, TRUE);        bx += 86;
    MoveWindow(d->status, bx, y + 5, client.right - bx - 8 > 40 ? client.right - bx - 8 : 40, 20, TRUE);

    MoveWindow(d->tabs, 8, 40, client.right - 16, client.bottom - 48, TRUE);
    page = (RECT){ 8, 40, client.right - 8, client.bottom - 8 };
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    px = page.left + 4; py = page.top + 4;
    pw = page.right - page.left - 8; ph = page.bottom - page.top - 8;
    if (pw < 200) pw = 200;
    if (ph < 200) ph = 200;

    /* Prompt */
    MoveWindow(d->prompt_out, px, py, pw, ph - 32, TRUE);
    MoveWindow(d->prompt_in, px, py + ph - 26, pw, 24, TRUE);

    /* CPU & RAM: the main set on one row, the shadow set and the interrupt
     * state on the next, then the flags and the beam. */
    {
        int x = px, ry = py;
        for (i = 0; i < NREGS; i++) {
            if (i == 8) { x = px; ry += 30; }
            MoveWindow(d->reg_label[i], x, ry + 4, 32, 18, TRUE); x += 34;
            MoveWindow(d->reg_edit[i], x, ry, 58, 24, TRUE); x += 68;
        }
        ry += 32;
        x = px;
        for (i = 0; i < NFLAGS; i++) { MoveWindow(d->flag[i], x, ry, i == 3 ? 52 : 40, 22, TRUE); x += i == 3 ? 56 : 44; }
        MoveWindow(d->beam, x + 8, ry + 3, pw - (x - px) - 8, 18, TRUE);
        ry += 30;
        MoveWindow(d->ram_label, px, ry, pw, 18, TRUE); ry += 20;
        MoveWindow(d->ram_view, px, ry, pw, ph - (ry - py) - 34, TRUE);
        ry = py + ph - 26;
        MoveWindow(d->ram_goto_label, px, ry + 4, 36, 18, TRUE);
        MoveWindow(d->ram_goto, px + 40, ry, 100, 24, TRUE);
        MoveWindow(d->ram_addr_label, px + 160, ry + 4, 80, 18, TRUE);
        MoveWindow(d->ram_addr, px + 244, ry, 80, 24, TRUE);
        MoveWindow(d->ram_val_label, px + 334, ry + 4, 40, 18, TRUE);
        MoveWindow(d->ram_val, px + 376, ry, 60, 24, TRUE);
    }

    /* Disassembly */
    MoveWindow(d->follow_pc, px, py + 2, 90, 22, TRUE);
    MoveWindow(d->jump_label, px + 96, py + 4, 50, 18, TRUE);
    MoveWindow(d->jump, px + 148, py, 150, 24, TRUE);
    MoveWindow(d->disasm_hint, px + 308, py + 4, pw - 308 > 40 ? pw - 308 : 40, 18, TRUE);
    MoveWindow(d->disasm, px, py + 30, pw, ph - 30, TRUE);

    /* VDP: registers and sprites left; the picture and CRAM right */
    {
        const int lw = pw * 42 / 100, rx = px + lw + 10, rw = pw - lw - 10;
        const int th = ph * 58 / 100;
        const int cram_h = 2 * (d->mono_h + 26);
        const int cy = py + ph - 26 - cram_h - 22;
        MoveWindow(d->vdp_text, px, py, lw, th, TRUE);
        MoveWindow(d->sprites, px, py + th + 6, lw, ph - th - 6, TRUE);
        MoveWindow(d->vdp_view_label, rx, py + 4, 40, 18, TRUE);
        MoveWindow(d->vdp_view, rx + 42, py, 120, 200, TRUE);
        MoveWindow(d->vdp_pal_label, rx + 172, py + 4, 46, 18, TRUE);
        MoveWindow(d->vdp_palette, rx + 220, py, 110, 200, TRUE);
        MoveWindow(d->vdp_pic, rx, py + 30, rw, cy - (py + 30) - 6, TRUE);
        MoveWindow(d->cram_label, rx, cy, rw, 18, TRUE);
        MoveWindow(d->cram, rx, cy + 20, rw < 640 ? rw : 640, cram_h, TRUE);
        MoveWindow(d->cram_index_label, rx, py + ph - 22, 36, 18, TRUE);
        MoveWindow(d->cram_index, rx + 40, py + ph - 26, 50, 24, TRUE);
        MoveWindow(d->cram_val_label, rx + 100, py + ph - 22, 40, 18, TRUE);
        MoveWindow(d->cram_val, rx + 142, py + ph - 26, 60, 24, TRUE);
    }

    /* Sound & I/O */
    MoveWindow(d->sound, px, py, pw, ph, TRUE);

    /* Breakpoints */
    {
        int ry = py + ph - 62;
        MoveWindow(d->bp_list, px, py, pw, ry - py - 6, TRUE);
        MoveWindow(d->bp_type, px, ry, 100, 200, TRUE);
        MoveWindow(d->bp_start_label, px + 108, ry + 4, 50, 18, TRUE);
        MoveWindow(d->bp_start, px + 160, ry, 90, 24, TRUE);
        MoveWindow(d->bp_end_label, px + 256, ry + 4, 18, 18, TRUE);
        MoveWindow(d->bp_end, px + 276, ry, 90, 24, TRUE);
        MoveWindow(d->bp_cond_label, px + 374, ry + 4, 14, 18, TRUE);
        MoveWindow(d->bp_cond, px + 390, ry, pw - 390 - 70 > 80 ? pw - 390 - 70 : 80, 24, TRUE);
        MoveWindow(d->bp_add, px + pw - 64, ry - 1, 64, 26, TRUE);
        ry += 32;
        MoveWindow(d->bp_remove, px, ry, 90, 26, TRUE);
        MoveWindow(d->bp_enable, px + 96, ry, 120, 26, TRUE);
        MoveWindow(d->bp_clear, px + 222, ry, 90, 26, TRUE);
    }

    /* Cart */
    MoveWindow(d->cart, px, py, pw, ph, TRUE);
}

/* ---- attach / detach -------------------------------------------------------------- */

static void attach(debugger *d)
{
    smsdebug_attach(d->dbg);
    d->seen_gen = smsdebug_generation(d->dbg);
    d->was_stopped = 1;
    refresh_all(d);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
}

static void detach_and_hide(debugger *d)
{
    KillTimer(d->hwnd, TIMER_REFRESH);
    smsdebug_detach(d->dbg);
    ShowWindow(d->hwnd, SW_HIDE);
}

/* ---- window proc -------------------------------------------------------------------- */

static void draw_run_button(debugger *d, const DRAWITEMSTRUCT *di)
{
    char text[32];
    const int stopped = smsdebug_is_stopped(d->dbg);
    RECT r = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &r, d->accent);
        FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, RGB(255, 255, 255));
    } else {
        DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    GetWindowTextA(di->hwndItem, text, sizeof text);
    DrawTextA(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(di->hDC, &r);
    }
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d || d->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REFRESH && IsWindowVisible(hwnd)) {
            const unsigned gen = smsdebug_generation(d->dbg);
            const int stopped = smsdebug_is_stopped(d->dbg);
            if (gen != d->seen_gen || stopped != d->was_stopped) {
                d->seen_gen = gen;
                d->was_stopped = stopped;
                refresh_all(d);
            } else if (!stopped && ++d->running_ticks >= 5) {
                /* running: the live pages twice a second (every smsdebug
                 * call takes the host's run lock, so this is safe) */
                d->running_ticks = 0;
                refresh_status(d);
                if (d->page == PAGE_CPU) { refresh_cpu(d); refresh_ram(d); }
                else if (d->page == PAGE_DISASM) refresh_disasm(d);
                else if (d->page == PAGE_VDP) refresh_vdp(d);
                else if (d->page == PAGE_SOUND) refresh_sound(d);
                else if (d->page == PAGE_BREAKS) refresh_bps(d);
                else if (d->page == PAGE_CART) refresh_cart(d);
            }
        }
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_TAB:
        complete_prompt(d);
        return 0;
    case WM_DBG_HISTORY:
        history_step(d, (int)wp);
        return 0;
    case WM_NOTIFY:
        if (((LPNMHDR)lp)->code == (UINT)TCN_SELCHANGE) {
            show_page(d, (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0));
            layout(d);
            return 0;
        }
        break;
    case WM_DRAWITEM:
        if (wp == IDC_RUN) { draw_run_button(d, (const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == d->status || (HWND)lp == d->beam || (HWND)lp == d->disasm_hint) {
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= IDC_FLAG0 && id <= IDC_FLAG_LAST && HIWORD(wp) == BN_CLICKED) {
            static const int flag_ids[NFLAGS] = { SMS_FLAG_S, SMS_FLAG_Z, SMS_FLAG_H, SMS_FLAG_PV,
                                                  SMS_FLAG_N, SMS_FLAG_C };
            if (smsdebug_is_stopped(d->dbg))
                smsdebug_cpu_set(d->dbg, flag_ids[id - IDC_FLAG0],
                                 SendMessageA(d->flag[id - IDC_FLAG0], BM_GETCHECK, 0, 0) == BST_CHECKED);
            refresh_cpu(d);
            return 0;
        }
        if (id >= IDC_SAVE_KIND0 && id <= IDC_SAVE_KIND_LAST) { save_file(d, id - IDC_SAVE_KIND0); return 0; }
        switch (id) {
        case IDC_RUN: toggle_run(d); return 0;
        case IDC_STEP: smsdebug_step(d->dbg); refresh_all(d); return 0;
        case IDC_OVER: smsdebug_step_over(d->dbg); refresh_all(d); return 0;
        case IDC_OUT: smsdebug_step_out(d->dbg); refresh_all(d); return 0;
        case IDC_SCAN: smsdebug_scanline(d->dbg, 1); refresh_all(d); return 0;
        case IDC_FRAME: smsdebug_frame(d->dbg, 1); refresh_all(d); return 0;
        case IDC_LOAD_SYMBOLS: symbols_menu(d); return 0;
        case IDC_SYMS_FILE: load_symbols(d, 0); return 0;
        case IDC_SYMS_CART: load_symbols(d, 1); return 0;
        case IDC_SAVE: save_menu(d); return 0;
        case IDC_FOLLOW_PC: refresh_disasm(d); return 0;
        case IDC_VDP_VIEW:
        case IDC_VDP_PALETTE:
            if (HIWORD(wp) == CBN_SELCHANGE) refresh_vdp(d);
            return 0;
        case IDC_BP_ADD: add_breakpoint(d); return 0;
        case IDC_BP_REMOVE:
        case IDC_BP_ENABLE: {
            int sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < d->nbps) {
                if (id == IDC_BP_REMOVE) smsdebug_breakpoint_remove(d->dbg, d->bps[sel].id);
                else smsdebug_breakpoint_enable(d->dbg, d->bps[sel].id, !d->bps[sel].enabled);
                refresh_bps(d);
                refresh_disasm(d);
            }
            return 0;
        }
        case IDC_BP_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) {
                int sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < d->nbps &&
                    !(d->bps[sel].type & (SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT))) {
                    char a[16];
                    snprintf(a, sizeof a, "$%04X", d->bps[sel].start);
                    TabCtrl_SetCurSel(d->tabs, PAGE_DISASM);
                    show_page(d, PAGE_DISASM);
                    layout(d);
                    jump_to(d, a);
                }
            }
            return 0;
        case IDC_BP_CLEAR:
            smsdebug_breakpoint_clear(d->dbg);
            refresh_bps(d);
            refresh_disasm(d);
            return 0;
        case IDCANCEL:
            detach_and_hide(d);
            return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; F12 brings it back, stopped. */
        detach_and_hide(d);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        DeleteObject(d->mono);
        DeleteObject(d->accent);
        DestroyAcceleratorTable(d->accel);
        free(d->pic_px);
        free(d);
        g_dbg = NULL;
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ---------------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const char *name, WNDPROC proc, HBRUSH background)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    RegisterClassA(&wc);
}

/* SMS_DEBUGGER_TAB: a page number, or its name (prompt, cpu, disassembly,
 * vdp, sound, breakpoints, cart). */
static int page_from_env(void)
{
    const char *tab = getenv("SMS_DEBUGGER_TAB");
    int i;
    if (!tab || !*tab) return PAGE_PROMPT;
    if (tab[0] >= '0' && tab[0] <= '9') {
        i = atoi(tab);
        return (i >= 0 && i < PAGE_COUNT) ? i : PAGE_PROMPT;
    }
    for (i = 0; i < PAGE_COUNT; i++)
        if (lstrcmpiA(tab, kPageNames[i]) == 0) return i;
    return PAGE_PROMPT;
}

void sms_debugger_show(HWND parent, smssession *session)
{
    HINSTANCE inst;
    debugger *d;
    ACCEL accels[6];

    if (g_dbg) {
        if (!IsWindowVisible(g_dbg->hwnd)) {
            ShowWindow(g_dbg->hwnd, SW_SHOW);
            attach(g_dbg);
        } else {
            smsdebug_stop(g_dbg->dbg);
            refresh_all(g_dbg);
        }
        SetForegroundWindow(g_dbg->hwnd);
        return;
    }

    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);
    register_class(inst, "SMSDebuggerWindow", dbg_proc, (HBRUSH)(COLOR_BTNFACE + 1));
    register_class(inst, "SMSDbgPixels", pic_proc, NULL);
    register_class(inst, "SMSDbgDisasm", disasm_proc, NULL);
    register_class(inst, "SMSDbgCram", cram_proc, NULL);

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = smssession_debugger(session);
    d->pic_px = calloc(SMSDEBUG_VIEW_MAX_PIXELS, sizeof *d->pic_px);
    g_dbg = d;

    d->hwnd = CreateWindowExA(0, "SMSDebuggerWindow", "Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820, NULL, NULL, inst, NULL);
    if (!d->hwnd) { free(d->pic_px); free(d); g_dbg = NULL; return; }

    d->ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    d->mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    {
        HDC dc = GetDC(d->hwnd);
        TEXTMETRICA tm;
        HGDIOBJ old = SelectObject(dc, d->mono);
        d->mono_w = 7;
        d->mono_h = 15;
        if (GetTextMetricsA(dc, &tm)) {
            d->mono_w = tm.tmAveCharWidth;
            d->mono_h = tm.tmHeight;
        }
        SelectObject(dc, old);
        ReleaseDC(d->hwnd, dc);
    }
    d->accent = CreateSolidBrush(RGB((SMSSESSION_ACCENT_RGB >> 16) & 0xff, (SMSSESSION_ACCENT_RGB >> 8) & 0xff,
                                     SMSSESSION_ACCENT_RGB & 0xff));

    build_controls(d);

    /* F5/F7/F8/Shift+F8/F12 work wherever the focus is inside the window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY;          accels[4].key = VK_F12; accels[4].cmd = IDCANCEL;
    accels[5].fVirt = FVIRTKEY | FSHIFT; accels[5].key = VK_F7;  accels[5].cmd = IDC_FRAME;
    d->accel = CreateAcceleratorTableA(accels, 6);

    d->page = page_from_env();
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);
    layout(d);

    ShowWindow(d->hwnd, SW_SHOW);
    attach(d);
}

int sms_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel) return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd)) return 0;
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) { detach_and_hide(g_dbg); return 1; }
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}

int sms_debugger_visible(void)
{
    return g_dbg && IsWindowVisible(g_dbg->hwnd);
}

void sms_debugger_toggle(HWND parent, smssession *session)
{
    if (sms_debugger_visible()) detach_and_hide(g_dbg);
    else sms_debugger_show(parent, session);
}
