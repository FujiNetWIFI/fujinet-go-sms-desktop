/*
 * prompt.c -- the debugger's command line (smsdebug_command) and its Tab
 * completion. Every command is a thin layer over the public API, so a
 * window's buttons and the prompt can never disagree.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "debugger_internal.h"

typedef struct {
    char *dst;
    int cap, len;
} outbuf;

static void out(outbuf *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (o->len >= o->cap - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->dst + o->len, (size_t)(o->cap - o->len), fmt, ap);
    va_end(ap);
    if (n > 0)
        o->len += n < o->cap - o->len ? n : o->cap - o->len - 1;
}

static const char *const commands[] = {
    "help", "step", "over", "out", "run", "stop", "runto", "scanline", "frame",
    "break", "bpr", "bpw", "bpin", "bpout", "delete", "enable", "disable", "clear",
    "breaks", "print", "regs", "set", "mem", "poke", "vram", "vpoke", "cram", "vdp",
    "sprites", "psg", "fm", "io", "cart", "mailbox", "label", "labels", "disasm",
    "trace", "save", NULL,
};

static const char *help_text =
    "step [n]               run n instructions (F7)\n"
    "over                   step over a CALL/RST/DJNZ/block op (F8)\n"
    "out                    run to the return (Shift+F8)\n"
    "run | stop             go / break\n"
    "runto <addr>           run until PC = addr\n"
    "scanline [n] | frame [n]\n"
    "break <addr>[-<end>] [cond]     execute breakpoint\n"
    "bpr | bpw <addr>[-<end>] [cond] read / write breakpoint\n"
    "bpin | bpout <port>[-<end>] [cond]  I/O breakpoint\n"
    "delete <id> | enable <id> | disable <id> | clear | breaks\n"
    "print <expr>           e.g. print hl, print [$c000], print {sp}\n"
    "regs                   CPU registers\n"
    "set <reg> <expr>       e.g. set pc $0038, set zf 1\n"
    "mem <addr> [len]       dump memory (side-effect free)\n"
    "poke <addr> <v...>     write memory\n"
    "vram <addr> [len] | vpoke <addr> <v...> | cram\n"
    "vdp | sprites | psg | fm | io | cart | mailbox\n"
    "label <addr> <name> | labels [file]\n"
    "disasm [addr] [n]\n"
    "trace [on|off|n]       record / show the last n instructions\n"
    "save <dis|ram|vram|cram|sram|arena|regs> <file>\n"
    "Expressions: $C000 0xC000 C000h 49152 %1010; registers a..l af..iy sp pc\n"
    "i r af' ...; flags cf zf sf hf pf nf; [addr] {addr}; labels; vpos hpos\n"
    "vcount hcount frame cycles; in a condition, value and addr; C operators.\n";

static char *next_tok(char **p)
{
    char *s = *p, *t;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
    {
        *p = s;
        return NULL;
    }
    t = s;
    while (*s && !isspace((unsigned char)*s))
        s++;
    if (*s)
        *s++ = '\0';
    *p = s;
    return t;
}

static char *rest_of(char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static int eval(smsdebug *d, outbuf *o, const char *text, long *v)
{
    char err[96];
    if (smsdebug_eval(d, text, v, err, sizeof err) != 0)
    {
        out(o, "error: %s\n", err);
        return -1;
    }
    return 0;
}

/* "<addr>" or "<addr>-<end>" */
static int range(smsdebug *d, outbuf *o, char *tok, long *a, long *b)
{
    char *dash = strchr(tok + 1, '-');
    if (dash)
    {
        *dash = '\0';
        if (eval(d, o, tok, a) || eval(d, o, dash + 1, b))
            return -1;
        return 0;
    }
    if (eval(d, o, tok, a))
        return -1;
    *b = *a;
    return 0;
}

static void show_regs(smsdebug *d, outbuf *o)
{
    smsdebug_cpu c;
    smsdebug_cpu_get(d, &c);
    out(o, "PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X\n",
        c.pc, c.sp, c.af, c.bc, c.de, c.hl, c.ix, c.iy);
    out(o, "AF'=%04X BC'=%04X DE'=%04X HL'=%04X I=%02X R=%02X IM%d IFF1=%d IFF2=%d%s\n",
        c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im, c.iff1, c.iff2, c.halted ? " HALT" : "");
    out(o, "flags %c%c%c%c%c%c%c%c  /INT %s /NMI %s  line %d dot %d  V=%02X H=%02X  frame %u\n",
        c.sf ? 'S' : '-', c.zf ? 'Z' : '-', c.yf ? 'Y' : '-', c.hf ? 'H' : '-',
        c.xf ? 'X' : '-', c.pf ? 'P' : '-', c.nf ? 'N' : '-', c.cf ? 'C' : '-',
        c.int_line ? "low" : "high", c.nmi_line ? "low" : "high", c.vpos, c.hpos,
        c.vcount, c.hcount, c.frame);
}

static void dump(outbuf *o, uint16_t addr, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i += 16)
    {
        out(o, "%04X ", (uint16_t)(addr + i));
        for (int j = 0; j < 16; j++)
            out(o, j < n - i ? " %02X" : "   ", b[i + j]);
        out(o, "  ");
        for (int j = 0; j < 16 && i + j < n; j++)
            out(o, "%c", (b[i + j] >= 0x20 && b[i + j] < 0x7f) ? b[i + j] : '.');
        out(o, "\n");
    }
}

static int reg_by_name(const char *n)
{
    static const struct { const char *name; int reg; } map[] = {
        {"pc", SMS_REG_PC}, {"sp", SMS_REG_SP}, {"af", SMS_REG_AF}, {"bc", SMS_REG_BC},
        {"de", SMS_REG_DE}, {"hl", SMS_REG_HL}, {"ix", SMS_REG_IX}, {"iy", SMS_REG_IY},
        {"af'", SMS_REG_AF2}, {"bc'", SMS_REG_BC2}, {"de'", SMS_REG_DE2}, {"hl'", SMS_REG_HL2},
        {"i", SMS_REG_I}, {"r", SMS_REG_R}, {"im", SMS_REG_IM}, {"iff1", SMS_REG_IFF1},
        {"iff2", SMS_REG_IFF2}, {"a", SMS_REG_A}, {"f", SMS_REG_F}, {"b", SMS_REG_B},
        {"c", SMS_REG_C}, {"d", SMS_REG_D}, {"e", SMS_REG_E}, {"h", SMS_REG_H}, {"l", SMS_REG_L},
        {"sf", SMS_FLAG_S}, {"zf", SMS_FLAG_Z}, {"hf", SMS_FLAG_H}, {"pf", SMS_FLAG_PV},
        {"nf", SMS_FLAG_N}, {"cf", SMS_FLAG_C},
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(map[i].name, n))
            return map[i].reg;
    return -1;
}

static void list_breaks(smsdebug *d, outbuf *o)
{
    smsdebug_breakpoint bp[128];
    int n = smsdebug_breakpoint_list(d, bp, 128);
    if (!n)
    {
        out(o, "no breakpoints\n");
        return;
    }
    for (int i = 0; i < n; i++)
    {
        char kinds[16] = "";
        if (bp[i].type & SMSDEBUG_BP_EXEC)  strcat(kinds, "x");
        if (bp[i].type & SMSDEBUG_BP_READ)  strcat(kinds, "r");
        if (bp[i].type & SMSDEBUG_BP_WRITE) strcat(kinds, "w");
        if (bp[i].type & SMSDEBUG_BP_IN)    strcat(kinds, "i");
        if (bp[i].type & SMSDEBUG_BP_OUT)   strcat(kinds, "o");
        if (bp[i].start == bp[i].end)
            out(o, "#%d %-3s $%04X", bp[i].id, kinds, bp[i].start);
        else
            out(o, "#%d %-3s $%04X-$%04X", bp[i].id, kinds, bp[i].start, bp[i].end);
        out(o, "%s%s%s  hits %u\n", bp[i].enabled ? "" : " (disabled)",
            bp[i].condition[0] ? " if " : "", bp[i].condition, bp[i].hits);
    }
}

static void add_bp(smsdebug *d, outbuf *o, int type, char *args, int io)
{
    char *t = next_tok(&args);
    long a, b;
    int id;

    if (!t)
    {
        out(o, "usage: <addr>[-<end>] [condition]\n");
        return;
    }
    if (range(d, o, t, &a, &b))
        return;
    if (io && (a > 255 || b > 255))
    {
        out(o, "error: ports are 0-255\n");
        return;
    }
    id = smsdebug_breakpoint_add(d, type, (uint16_t)a, (uint16_t)b, rest_of(args));
    if (id < 0)
        out(o, "error: the condition does not parse\n");
    else
        out(o, "breakpoint #%d\n", id);
}

int smsdebug_command(smsdebug *d, const char *command, char *dst, int dstsz)
{
    char line[512];
    char *p = line, *cmd;
    outbuf o = { dst, dstsz, 0 };
    long v, w;

    if (!d || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    snprintf(line, sizeof line, "%s", command ? command : "");
    cmd = next_tok(&p);
    if (!cmd)
        return 0;
    for (char *c = cmd; *c; c++)
        *c = (char)tolower((unsigned char)*c);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
        out(&o, "%s", help_text);
    else if (!strcmp(cmd, "step") || !strcmp(cmd, "s"))
    {
        char *t = next_tok(&p);
        v = 1;
        if (t && eval(d, &o, t, &v))
            return o.len;
        if (!smsdebug_is_stopped(d))
            out(&o, "the machine is running (stop first)\n");
        else if (v <= 1)
            smsdebug_step(d);
        else
        {
            /* step n: the hook counts them down */
            smsdebug_step(d);
            for (long i = 1; i < v; i++)
            {
                int tries = 0;
                while (!smsdebug_is_stopped(d) && tries++ < 2000)
                {
                    struct timespec ts = { 0, 100000L };
                    nanosleep(&ts, NULL);
                }
                smsdebug_step(d);
            }
        }
    }
    else if (!strcmp(cmd, "over") || !strcmp(cmd, "o"))
        smsdebug_step_over(d);
    else if (!strcmp(cmd, "out"))
        smsdebug_step_out(d);
    else if (!strcmp(cmd, "run") || !strcmp(cmd, "go") || !strcmp(cmd, "g"))
        smsdebug_resume(d);
    else if (!strcmp(cmd, "stop"))
        smsdebug_stop(d);
    else if (!strcmp(cmd, "runto"))
    {
        char *t = next_tok(&p);
        if (!t || eval(d, &o, t, &v))
            return o.len;
        smsdebug_run_to(d, (uint16_t)v);
    }
    else if (!strcmp(cmd, "scanline") || !strcmp(cmd, "frame"))
    {
        char *t = next_tok(&p);
        v = 1;
        if (t && eval(d, &o, t, &v))
            return o.len;
        if (!strcmp(cmd, "frame"))
            smsdebug_frame(d, (int)v);
        else
            smsdebug_scanline(d, (int)v);
    }
    else if (!strcmp(cmd, "break") || !strcmp(cmd, "bp"))
        add_bp(d, &o, SMSDEBUG_BP_EXEC, p, 0);
    else if (!strcmp(cmd, "bpr"))
        add_bp(d, &o, SMSDEBUG_BP_READ, p, 0);
    else if (!strcmp(cmd, "bpw"))
        add_bp(d, &o, SMSDEBUG_BP_WRITE, p, 0);
    else if (!strcmp(cmd, "bpin"))
        add_bp(d, &o, SMSDEBUG_BP_IN, p, 1);
    else if (!strcmp(cmd, "bpout"))
        add_bp(d, &o, SMSDEBUG_BP_OUT, p, 1);
    else if (!strcmp(cmd, "delete") || !strcmp(cmd, "enable") || !strcmp(cmd, "disable"))
    {
        char *t = next_tok(&p);
        if (!t)
            out(&o, "usage: %s <id>\n", cmd);
        else
        {
            long id = strtol(t[0] == '#' ? t + 1 : t, NULL, 10);
            if (!strcmp(cmd, "delete"))
                smsdebug_breakpoint_remove(d, (int)id);
            else
                smsdebug_breakpoint_enable(d, (int)id, !strcmp(cmd, "enable"));
        }
    }
    else if (!strcmp(cmd, "clear"))
        smsdebug_breakpoint_clear(d);
    else if (!strcmp(cmd, "breaks") || !strcmp(cmd, "bl"))
        list_breaks(d, &o);
    else if (!strcmp(cmd, "print") || !strcmp(cmd, "p"))
    {
        if (eval(d, &o, rest_of(p), &v) == 0)
        {
            char bin[17];
            for (int i = 0; i < 16; i++)
                bin[i] = (char)('0' + ((v >> (15 - i)) & 1));
            bin[16] = '\0';
            out(&o, "$%04lX  %ld  %%%s\n", (unsigned long)v & 0xffff, v, bin);
        }
    }
    else if (!strcmp(cmd, "regs") || !strcmp(cmd, "r"))
        show_regs(d, &o);
    else if (!strcmp(cmd, "set"))
    {
        char *r = next_tok(&p);
        int reg = r ? reg_by_name(r) : -1;
        if (reg < 0)
            out(&o, "usage: set <reg> <expr>\n");
        else if (eval(d, &o, rest_of(p), &v) == 0)
        {
            if (!smsdebug_is_stopped(d))
                out(&o, "the machine is running (stop first)\n");
            else
            {
                smsdebug_cpu_set(d, reg, (int)v);
                show_regs(d, &o);
            }
        }
    }
    else if (!strcmp(cmd, "mem") || !strcmp(cmd, "m") || !strcmp(cmd, "vram"))
    {
        char *t = next_tok(&p), *l = next_tok(&p);
        uint8_t buf[256];
        w = 64;
        if (!t || eval(d, &o, t, &v) || (l && eval(d, &o, l, &w)))
            return o.len;
        if (w > 256)
            w = 256;
        if (!strcmp(cmd, "vram"))
            smsdebug_vram_read(d, (uint16_t)v, buf, (int)w);
        else
            smsdebug_read(d, (uint16_t)v, buf, (int)w);
        dump(&o, (uint16_t)v, buf, (int)w);
    }
    else if (!strcmp(cmd, "poke") || !strcmp(cmd, "vpoke"))
    {
        char *t = next_tok(&p), *b;
        int n = 0;
        if (!t || eval(d, &o, t, &v))
            return o.len;
        while ((b = next_tok(&p)) != NULL)
        {
            if (eval(d, &o, b, &w))
                return o.len;
            if (!strcmp(cmd, "vpoke"))
            {
                uint8_t byte = (uint8_t)w;
                smsdebug_vram_write(d, (uint16_t)(v + n), &byte, 1);
            }
            else
                smsdebug_write(d, (uint16_t)(v + n), (uint8_t)w);
            n++;
        }
        out(&o, "%d byte%s at $%04lX\n", n, n == 1 ? "" : "s", (unsigned long)v & 0xffff);
    }
    else if (!strcmp(cmd, "cram") || !strcmp(cmd, "vdp"))
    {
        smsdebug_vdp vd;
        smsdebug_vdp_get(d, &vd);
        if (!strcmp(cmd, "vdp"))
        {
            char desc[128];
            out(&o, "%s, %s%s  addr $%04X code %d buffer $%02X  status $%02X  line counter %d\n",
                vd.mode_name, vd.kind_5246 ? "315-5246" : "315-5124", vd.is_pal ? " PAL" : "",
                vd.addr, vd.code, vd.buffer, vd.status, vd.line_counter);
            for (int r = 0; r < 11; r++)
            {
                smsdebug_vdp_describe_register(d, r, desc, sizeof desc);
                out(&o, "%s\n", desc);
            }
            out(&o, "beam line %d dot %d  V=$%02X  /INT %s  frame %u\n", vd.vpos, vd.hpos,
                vd.vcount, vd.int_line ? "low" : "high", vd.frame);
        }
        else
        {
            for (int i = 0; i < 32; i++)
                out(&o, "%s%02d:$%02X%s", i % 8 ? " " : "", i, vd.cram[i], i % 8 == 7 ? "\n" : "");
        }
    }
    else if (!strcmp(cmd, "sprites"))
    {
        smsdebug_sprite sp[64];
        int n = smsdebug_sprites_get(d, sp);
        for (int i = 0; i < n; i++)
            if (sp[i].visible)
                out(&o, "%2d: x=%3d y=%3d tile=$%03X%s\n", i, sp[i].x, sp[i].y, sp[i].tile,
                    n == 32 ? (sp[i].early_clock ? " EC" : "") : "");
    }
    else if (!strcmp(cmd, "psg") || !strcmp(cmd, "fm") || !strcmp(cmd, "io"))
    {
        smsdebug_io io;
        smsdebug_io_get(d, &io);
        if (!strcmp(cmd, "psg"))
        {
            for (int i = 0; i < 3; i++)
                out(&o, "tone %d: period %4d volume %2d\n", i, io.tone_period[i], io.tone_volume[i]);
            out(&o, "noise: %s, rate %d, volume %d, LFSR $%04X%s\n", io.noise_mode ? "white" : "periodic",
                io.noise_rate, io.noise_volume, io.lfsr, io.psg_audible ? "" : " (muted)");
        }
        else if (!strcmp(cmd, "fm"))
        {
            if (!io.fm_present)
                out(&o, "no YM2413 on this console\n");
            else
            {
                out(&o, "YM2413%s\n", io.fm_audible ? "" : " (muted)");
                dump(&o, 0, io.fm_regs, 0x40);
            }
        }
        else
        {
            out(&o, "%s: $3E=%02X (cart %s, BIOS %s, RAM %s, I/O %s)  $3F=%02X\n", io.console_name,
                io.mem_ctrl, io.cart_enabled ? "on" : "off", io.bios_enabled ? "on" : "off",
                io.ram_enabled ? "on" : "off", io.io_enabled ? "on" : "off", io.io_ctrl);
            out(&o, "$DC=%02X $DD=%02X  pads %02X %02X  pause %d reset %d  mapper %02X %02X %02X %02X%s\n",
                io.port_dc, io.port_dd, io.pad[0], io.pad[1], io.pause_held, io.reset_held,
                io.mapper[0], io.mapper[1], io.mapper[2], io.mapper[3],
                io.bios_present ? "" : "  (no BIOS)");
        }
    }
    else if (!strcmp(cmd, "cart") || !strcmp(cmd, "mailbox"))
    {
        smsdebug_cart c;
        char info[200];
        smsdebug_cart_get(d, &c);
        smsdebug_cart_info(d, info, sizeof info);
        out(&o, "%s\n", info);
        if (!strcmp(cmd, "cart"))
        {
            if (c.mode)
                out(&o, "banks %02X %02X %02X %02X %02X %02X  RAM %s%s  CRC %08X  claim %d\n",
                    c.bank[0], c.bank[1], c.bank[2], c.bank[3], c.bank[4], c.bank[5],
                    c.ram_enabled ? "on" : "off", c.ram_writable ? " (writable)" : "",
                    c.image_crc, c.claim);
            out(&o, "BIOS snoop%s: $C000=%02X $3E=%02X $3F=%02X\n", c.bios_phase ? " (in progress)" : "",
                c.snoop_c000, c.snoop_3e, c.snoop_3f);
        }
        else
        {
            out(&o, "ACKSEQ %d  status %02X  err %d  reply %02X  rxlen %u  queue %u\n", c.ackseq,
                c.status, c.last_error, c.reply_cmd, c.rxlen, c.queue_depth);
            out(&o, "boot state %d %d%% err %d (%u/%u)  load state %d window %d/%d %d%%\n",
                c.boot_state, c.boot_pct, c.boot_err, c.boot_got, c.boot_total, c.load_state,
                c.load_win, c.load_nwin, c.load_pct);
        }
    }
    else if (!strcmp(cmd, "label"))
    {
        char *t = next_tok(&p), *n = next_tok(&p);
        if (!t || eval(d, &o, t, &v))
            return o.len;
        smsdebug_set_label(d, (uint16_t)v, n);
    }
    else if (!strcmp(cmd, "labels"))
    {
        char msg[256];
        char *f = rest_of(p);
        smsdebug_load_symbols(d, *f ? f : NULL, msg, sizeof msg);
        out(&o, "%s\n", msg);
    }
    else if (!strcmp(cmd, "disasm") || !strcmp(cmd, "d"))
    {
        char *t = next_tok(&p), *n = next_tok(&p);
        smsdebug_line lines[64];
        smsdebug_cpu c;
        int pcl, cnt;
        smsdebug_cpu_get(d, &c);
        v = c.pc;
        w = 16;
        if ((t && eval(d, &o, t, &v)) || (n && eval(d, &o, n, &w)))
            return o.len;
        if (w > 64)
            w = 64;
        cnt = smsdebug_disassemble(d, (uint16_t)v, lines, (int)w, &pcl);
        for (int i = 0; i < cnt; i++)
        {
            if (lines[i].label[0])
                out(&o, "%s:\n", lines[i].label);
            out(&o, "%c%c%04X  %-12s %-22s%s%s\n", lines[i].is_pc ? '>' : ' ',
                lines[i].has_breakpoint ? '*' : ' ', lines[i].address, lines[i].bytes,
                lines[i].disasm, lines[i].comment[0] ? "; " : "", lines[i].comment);
        }
    }
    else if (!strcmp(cmd, "trace"))
    {
        char *t = next_tok(&p);
        if (t && !strcmp(t, "on"))
            smsdebug_trace_enable(d, 1);
        else if (t && !strcmp(t, "off"))
            smsdebug_trace_enable(d, 0);
        else if (t && !strcmp(t, "clear"))
            smsdebug_trace_clear(d);
        else
        {
            smsdebug_trace tr[64];
            int n;
            w = 16;
            if (t && eval(d, &o, t, &w))
                return o.len;
            if (w > 64)
                w = 64;
            n = smsdebug_trace_read(d, tr, (int)w);
            if (!n)
                out(&o, "trace is %s and empty\n", smsdebug_trace_enabled(d) ? "on" : "off");
            for (int i = n - 1; i >= 0; i--)
            {
                smsdebug_line l;
                int pcl;
                smsdebug_disassemble(d, tr[i].pc, &l, 1, &pcl);
                out(&o, "%10llu %3d:%3d  %04X  %-22s AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X\n",
                    (unsigned long long)tr[i].cycles, tr[i].vpos, tr[i].hpos, tr[i].pc,
                    l.disasm, tr[i].af, tr[i].bc, tr[i].de, tr[i].hl, tr[i].sp);
            }
        }
    }
    else if (!strcmp(cmd, "save"))
    {
        char *k = next_tok(&p);
        char *f = rest_of(p);
        char msg[300];
        if (!k || !*f)
            out(&o, "usage: save <dis|ram|vram|cram|sram|arena|regs> <file>\n");
        else
        {
            smsdebug_save(d, k, f, msg, sizeof msg);
            out(&o, "%s\n", msg);
        }
    }
    else
        out(&o, "unknown command '%s' (try help)\n", cmd);

    return o.len;
}

int smsdebug_completions(smsdebug *d, const char *prefix, char *dst, int dstsz)
{
    outbuf o = { dst, dstsz, 0 };
    const char *word;
    size_t n;
    int count = 0;
    const int first_word = !strchr(prefix ? prefix : "", ' ');

    if (!d || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!prefix)
        prefix = "";
    word = strrchr(prefix, ' ');
    word = word ? word + 1 : prefix;
    n = strlen(word);

    if (first_word)
    {
        for (int i = 0; commands[i]; i++)
            if (!strncmp(commands[i], word, n))
            {
                out(&o, "%s\n", commands[i]);
                count++;
            }
        return count;
    }
    if (!strncmp(prefix, "save ", 5) && !strchr(prefix + 5, ' '))
    {
        static const char *const kinds[] = { "dis", "ram", "vram", "cram", "sram", "arena", "regs", NULL };
        for (int i = 0; kinds[i]; i++)
            if (!strncmp(kinds[i], word, n))
            {
                out(&o, "%s\n", kinds[i]);
                count++;
            }
        return count;
    }
    for (int i = 0; smsdebug_reg_names[i]; i++)
        if (n && !strncmp(smsdebug_reg_names[i], word, n))
        {
            out(&o, "%s\n", smsdebug_reg_names[i]);
            count++;
        }
    {
        symtab *t = smsdebug_symbols(d);
        const char *name;
        for (int i = 0; (name = symtab_name_at(t, i)) != NULL && count < 200; i++)
            if (n && !strncasecmp(name, word, n))
            {
                out(&o, "%s\n", name);
                count++;
            }
    }
    return count;
}
