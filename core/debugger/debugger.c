/*
 * debugger.c -- the smsdebug engine (core/include/smsdebug.h).
 *
 * Built on the core's two hooks (core/sms/host.h): the instruction hook,
 * fired at every instruction boundary (z80_opdone) with the address bus on
 * the next opcode, and the bus hook, fired on data reads, writes and I/O
 * only while a breakpoint of that kind exists. Attached, the instruction
 * hook checks the stop request, the breakpoints and the step mode; stopping
 * PARKS the emulation thread right there (a condition variable) until a
 * window resumes it -- the CPU's own progress is what stops, there is no
 * "paused" flag a run loop polls. Detached, neither hook is installed, so a
 * session that never opens the debugger pays nothing. The design is the
 * astrocade and ColecoVision siblings'; the feature set (prompt, conditions,
 * read/write/IN/OUT breakpoints, scanline and frame steps, banked symbols,
 * save) is the NES sibling's.
 *
 * Threading. Everything public is called from UI threads; the hooks run on
 * the emulation thread. Every public call that reads or edits the machine
 * holds the host's run lock (sms_host_lock), which the emulation thread
 * holds while it runs a frame and lets go of between frames and while it is
 * parked here -- so a window refreshing live values while the machine runs
 * waits a moment instead of tearing, and a stopped machine is simply the
 * UI's. Lock order: the run lock, then d->lock (the hooks run with the run
 * lock held and take d->lock). A PC change runs the new opcode fetch on the
 * caller's thread, under the run lock, as the parked thread would have.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debugger_internal.h"
#include "expr.h"
#include "vdpview.h"
#include "z80dasm.h"

#include "host.h"
#include "fuji_mailbox.h"
#include "smsmap.h"

#define MAX_BP     128
#define TRACE_N    4096

enum { MODE_RUN = 0, MODE_STEP, MODE_OVER, MODE_OUT, MODE_RUNTO, MODE_SCANLINE, MODE_FRAME };

struct smsdebug {
    pthread_mutex_t lock;
    pthread_cond_t cond;

    atomic_int attached;
    atomic_int stopped;          /* the emulation thread is parked */
    atomic_int stop_req;
    atomic_int mode;
    long count;
    uint16_t target, sp_guard;
    int last_vpos;
    uint64_t target_frame;
    int was_attached;            /* across a power cycle */

    char reason[128];
    int reason_addr;
    atomic_uint generation;

    smsdebug_breakpoint bp[MAX_BP];
    int nbp, next_id;
    uint8_t map_exec[65536];     /* nonzero: an enabled exec bp covers it */
    uint8_t map_mem[65536];      /* SMSDEBUG_BP_READ / _WRITE bits */
    uint8_t map_io[256];         /* SMSDEBUG_BP_IN / _OUT bits */
    atomic_int break_pending;
    char break_reason[128];
    int break_addr;

    /* the access a condition sees as `value` / `addr` */
    long acc_value, acc_addr;
    int in_access;

    atomic_int trace_on;
    smsdebug_trace ring[TRACE_N];
    int ring_head, ring_count;

    symtab syms;
    char cart_path[1024];
};

static sms_machine_t *machine(void)
{
    return sms_host_is_running() ? sms_host_machine() : NULL;
}

static void bump(smsdebug *d)
{
    atomic_fetch_add(&d->generation, 1);
}

/* the address of the instruction about to run */
static uint16_t cur_pc(const sms_machine_t *m)
{
    return Z80_GET_ADDR(m->cpu.pins);
}

static int beam_hcount(const sms_machine_t *m)
{
    int hclock = sms_vdp_beam_hpos(&m->vdp, m->cycles) - 1;
    int dlt;
    if (hclock < 0)
        hclock += SMS_VDP_WIDTH;
    dlt = hclock - 46;
    return (dlt >= 0 ? dlt / 2 : -((-dlt + 1) / 2)) & 0xff;
}

/* ---- expressions ---------------------------------------------------------- */

const char *const smsdebug_reg_names[] = {
    "a", "f", "b", "c", "d", "e", "h", "l", "af", "bc", "de", "hl", "ix", "iy",
    "sp", "pc", "i", "r", "im", "iff1", "iff2", "af'", "bc'", "de'", "hl'",
    "cf", "nf", "pf", "hf", "zf", "sf", "vpos", "hpos", "vcount", "hcount",
    "frame", "cycles", "value", "addr", NULL,
};

static uint8_t expr_read(void *user, uint16_t addr)
{
    sms_machine_t *m = machine();
    (void)user;
    return m ? sms_machine_peek(m, addr) : 0xff;
}

static int expr_name(void *user, const char *name, long *out)
{
    smsdebug *d = user;
    sms_machine_t *m = machine();
    const z80_t *c;

    if (!strcmp(name, "value") && d->in_access) { *out = d->acc_value; return 1; }
    if (!strcmp(name, "addr") && d->in_access) { *out = d->acc_addr; return 1; }
    if (m)
    {
        c = &m->cpu;
        if (!strcmp(name, "a"))  { *out = c->a; return 1; }
        if (!strcmp(name, "f"))  { *out = c->f; return 1; }
        if (!strcmp(name, "b"))  { *out = c->b; return 1; }
        if (!strcmp(name, "c"))  { *out = c->c; return 1; }
        if (!strcmp(name, "d"))  { *out = c->d; return 1; }
        if (!strcmp(name, "e"))  { *out = c->e; return 1; }
        if (!strcmp(name, "h"))  { *out = c->h; return 1; }
        if (!strcmp(name, "l"))  { *out = c->l; return 1; }
        if (!strcmp(name, "af")) { *out = c->af; return 1; }
        if (!strcmp(name, "bc")) { *out = c->bc; return 1; }
        if (!strcmp(name, "de")) { *out = c->de; return 1; }
        if (!strcmp(name, "hl")) { *out = c->hl; return 1; }
        if (!strcmp(name, "ix")) { *out = c->ix; return 1; }
        if (!strcmp(name, "iy")) { *out = c->iy; return 1; }
        if (!strcmp(name, "sp")) { *out = c->sp; return 1; }
        if (!strcmp(name, "pc")) { *out = cur_pc(m); return 1; }
        if (!strcmp(name, "i"))  { *out = c->i; return 1; }
        if (!strcmp(name, "r"))  { *out = c->r; return 1; }
        if (!strcmp(name, "im")) { *out = c->im; return 1; }
        if (!strcmp(name, "iff1")) { *out = c->iff1; return 1; }
        if (!strcmp(name, "iff2")) { *out = c->iff2; return 1; }
        if (!strcmp(name, "af'")) { *out = c->af2; return 1; }
        if (!strcmp(name, "bc'")) { *out = c->bc2; return 1; }
        if (!strcmp(name, "de'")) { *out = c->de2; return 1; }
        if (!strcmp(name, "hl'")) { *out = c->hl2; return 1; }
        if (!strcmp(name, "cf")) { *out = (c->f >> 0) & 1; return 1; }
        if (!strcmp(name, "nf")) { *out = (c->f >> 1) & 1; return 1; }
        if (!strcmp(name, "pf")) { *out = (c->f >> 2) & 1; return 1; }
        if (!strcmp(name, "hf")) { *out = (c->f >> 4) & 1; return 1; }
        if (!strcmp(name, "zf")) { *out = (c->f >> 6) & 1; return 1; }
        if (!strcmp(name, "sf")) { *out = (c->f >> 7) & 1; return 1; }
        if (!strcmp(name, "vpos")) { *out = sms_vdp_beam_vpos(&m->vdp, m->cycles); return 1; }
        if (!strcmp(name, "hpos")) { *out = sms_vdp_beam_hpos(&m->vdp, m->cycles); return 1; }
        if (!strcmp(name, "vcount")) { *out = sms_vdp_vcount_read(&m->vdp, m->cycles); return 1; }
        if (!strcmp(name, "hcount")) { *out = beam_hcount(m); return 1; }
        if (!strcmp(name, "frame")) { *out = (long)m->vdp.frame_count; return 1; }
        if (!strcmp(name, "cycles")) { *out = (long)m->cycles; return 1; }
    }
    {
        int a = symtab_find(&d->syms, name);
        if (a >= 0)
        {
            *out = a;
            return 1;
        }
    }
    return 0;
}

int smsdebug_eval(smsdebug *d, const char *text, long *out, char *err, int errsz)
{
    expr_ctx ctx = { expr_name, expr_read, d };
    int rc;
    sms_host_lock();
    rc = expr_eval(text, &ctx, out, err, errsz);
    sms_host_unlock();
    return rc;
}

/* ---- the hooks (emulation thread) ----------------------------------------- */

static void trace_record(smsdebug *d, sms_machine_t *m, uint16_t pc)
{
    smsdebug_trace *t = &d->ring[d->ring_head];
    uint8_t code[4];
    z80d_insn ins;

    for (int i = 0; i < 4; i++)
        code[i] = sms_machine_peek(m, (uint16_t)(pc + i));
    z80_disassemble(&ins, pc, code);
    t->pc = pc;
    t->sp = m->cpu.sp;
    t->af = m->cpu.af;
    t->bc = m->cpu.bc;
    t->de = m->cpu.de;
    t->hl = m->cpu.hl;
    t->len = ins.len;
    memcpy(t->bytes, ins.bytes, 4);
    t->cycles = m->cycles;
    t->vpos = (int16_t)sms_vdp_beam_vpos(&m->vdp, m->cycles);
    t->hpos = (int16_t)sms_vdp_beam_hpos(&m->vdp, m->cycles);
    d->ring_head = (d->ring_head + 1) % TRACE_N;
    if (d->ring_count < TRACE_N)
        d->ring_count++;
}

static int cond_true(smsdebug *d, const smsdebug_breakpoint *b)
{
    long v = 0;
    char err[64];
    if (!b->condition[0])
        return 1;
    if (smsdebug_eval(d, b->condition, &v, err, sizeof err) != 0)
        return 1;   /* a condition that no longer evaluates stops, rather than hides */
    return v != 0;
}

/* Park until a window resumes. Called on the emulation thread, with the run
 * lock held and d->lock not; lets go of the run lock while parked. */
static void park(smsdebug *d, sms_machine_t *m, uint16_t pc, const char *reason)
{
    /* the picture as far as the beam has drawn it, and the cart's latest */
    sms_host_publish_frame();
    sms_cart_frame(m->cart);

    pthread_mutex_lock(&d->lock);
    snprintf(d->reason, sizeof d->reason, "%s", reason);
    d->reason_addr = pc;
    atomic_store(&d->mode, MODE_RUN);
    atomic_store(&d->stop_req, 0);
    atomic_store(&d->stopped, 1);
    bump(d);
    /* parked, the machine is the windows' */
    sms_host_park_release();
    while (atomic_load(&d->stopped) && atomic_load(&d->attached))
        pthread_cond_wait(&d->cond, &d->lock);
    atomic_store(&d->stopped, 0);
    pthread_mutex_unlock(&d->lock);
    sms_host_park_reacquire();

    /* A poke while stopped may have changed the opcode the CPU already
     * fetched: give it the byte now in memory. */
    pc = cur_pc(m);
    {
        const uint8_t now = sms_machine_peek(m, pc);
        if (Z80_GET_DATA(m->pins) != now)
            Z80_SET_DATA(m->pins, now);
    }
}

static void instr_hook(sms_machine_t *m, void *user)
{
    smsdebug *d = user;
    const uint16_t pc = cur_pc(m);
    char why[128];

    if (atomic_load_explicit(&d->trace_on, memory_order_relaxed))
        trace_record(d, m, pc);

    if (atomic_load(&d->break_pending))
    {
        atomic_store(&d->break_pending, 0);
        pthread_mutex_lock(&d->lock);
        snprintf(why, sizeof why, "%s", d->break_reason);
        pthread_mutex_unlock(&d->lock);
        park(d, m, pc, why);
        return;
    }
    if (atomic_load(&d->stop_req))
    {
        park(d, m, pc, "stopped");
        return;
    }
    if (d->map_exec[pc])
    {
        int hit = 0;
        pthread_mutex_lock(&d->lock);
        for (int i = 0; i < d->nbp; i++)
        {
            smsdebug_breakpoint *b = &d->bp[i];
            if (b->enabled && (b->type & SMSDEBUG_BP_EXEC) && pc >= b->start && pc <= b->end &&
                cond_true(d, b))
            {
                b->hits++;
                hit = 1;
                break;
            }
        }
        pthread_mutex_unlock(&d->lock);
        if (hit)
        {
            snprintf(why, sizeof why, "breakpoint: exec $%04X", pc);
            park(d, m, pc, why);
            return;
        }
    }

    switch (atomic_load(&d->mode))
    {
    case MODE_STEP:
        if (--d->count <= 0)
            park(d, m, pc, "step");
        break;
    case MODE_OVER:
        if (pc == d->target && m->cpu.sp >= d->sp_guard)
            park(d, m, pc, "step over");
        break;
    case MODE_OUT:
        if (pc == d->target && m->cpu.sp > d->sp_guard)
            park(d, m, pc, "step out");
        break;
    case MODE_RUNTO:
        if (pc == d->target)
            park(d, m, pc, "run to");
        break;
    case MODE_SCANLINE:
    {
        const int vpos = sms_vdp_beam_vpos(&m->vdp, m->cycles);
        if (vpos != d->last_vpos)
        {
            d->last_vpos = vpos;
            if (--d->count <= 0)
                park(d, m, pc, "scanline");
        }
        break;
    }
    case MODE_FRAME:
        if (m->vdp.frame_count >= d->target_frame)
            park(d, m, pc, "frame");
        break;
    default:
        break;
    }
}

static void bus_hook(sms_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data)
{
    smsdebug *d = user;
    int type, bits;
    const char *verb;

    (void)m;
    switch (kind)
    {
    case SMS_BUS_MEMR: type = SMSDEBUG_BP_READ;  bits = d->map_mem[addr]; verb = "read"; break;
    case SMS_BUS_MEMW: type = SMSDEBUG_BP_WRITE; bits = d->map_mem[addr]; verb = "write"; break;
    case SMS_BUS_IOR:  type = SMSDEBUG_BP_IN;    bits = d->map_io[addr & 0xff]; verb = "in"; break;
    case SMS_BUS_IOW:  type = SMSDEBUG_BP_OUT;   bits = d->map_io[addr & 0xff]; verb = "out"; break;
    default: return;
    }
    if (!(bits & type) || atomic_load(&d->break_pending))
        return;

    pthread_mutex_lock(&d->lock);
    d->acc_value = data;
    d->acc_addr = addr;
    d->in_access = 1;
    for (int i = 0; i < d->nbp; i++)
    {
        smsdebug_breakpoint *b = &d->bp[i];
        if (b->enabled && (b->type & type) && addr >= b->start && addr <= b->end && cond_true(d, b))
        {
            b->hits++;
            if (kind == SMS_BUS_IOR || kind == SMS_BUS_IOW)
                snprintf(d->break_reason, sizeof d->break_reason, "%s $%02X %s $%02X", verb,
                         addr & 0xff, kind == SMS_BUS_IOR ? "->" : "<-", data);
            else
                snprintf(d->break_reason, sizeof d->break_reason, "%s $%04X = $%02X", verb, addr, data);
            atomic_store(&d->break_pending, 1);
            break;
        }
    }
    d->in_access = 0;
    pthread_mutex_unlock(&d->lock);
}

/* What the bus hook must watch, from the enabled breakpoints. */
static uint8_t watch_bits(smsdebug *d)
{
    uint8_t w = 0;
    for (int i = 0; i < d->nbp; i++)
    {
        if (!d->bp[i].enabled)
            continue;
        if (d->bp[i].type & SMSDEBUG_BP_READ)  w |= SMS_WATCH_MEMR;
        if (d->bp[i].type & SMSDEBUG_BP_WRITE) w |= SMS_WATCH_MEMW;
        if (d->bp[i].type & SMSDEBUG_BP_IN)    w |= SMS_WATCH_IOR;
        if (d->bp[i].type & SMSDEBUG_BP_OUT)   w |= SMS_WATCH_IOW;
    }
    return w;
}

/* Rebuild the lookup maps and the bus watch. Called with the run lock and
 * d->lock held (the hooks read the maps under the run lock). */
static void rebuild_locked(smsdebug *d)
{
    memset(d->map_exec, 0, sizeof d->map_exec);
    memset(d->map_mem, 0, sizeof d->map_mem);
    memset(d->map_io, 0, sizeof d->map_io);
    for (int i = 0; i < d->nbp; i++)
    {
        const smsdebug_breakpoint *b = &d->bp[i];
        if (!b->enabled)
            continue;
        for (unsigned a = b->start; a <= b->end && a <= 0xffff; a++)
        {
            if (b->type & SMSDEBUG_BP_EXEC)
                d->map_exec[a] = 1;
            if (b->type & (SMSDEBUG_BP_READ | SMSDEBUG_BP_WRITE))
                d->map_mem[a] |= (uint8_t)(b->type & (SMSDEBUG_BP_READ | SMSDEBUG_BP_WRITE));
            if ((b->type & (SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT)) && a < 256)
                d->map_io[a] |= (uint8_t)(b->type & (SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT));
        }
    }
    if (atomic_load(&d->attached) && sms_host_is_running())
        sms_host_set_bus_hook(bus_hook, d, watch_bits(d));
}

/* ---- lifecycle -------------------------------------------------------------- */

smsdebug *smsdebug_create(void)
{
    smsdebug *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->cond, NULL);
    d->next_id = 1;
    d->reason_addr = -1;
    symtab_init(&d->syms);
    return d;
}

void smsdebug_free(smsdebug *d)
{
    if (!d)
        return;
    smsdebug_detach(d);
    symtab_free(&d->syms);
    pthread_mutex_destroy(&d->lock);
    pthread_cond_destroy(&d->cond);
    free(d);
}

static void hook(smsdebug *d)
{
    if (!sms_host_is_running())
        return;
    sms_host_set_bus_hook(bus_hook, d, watch_bits(d));
    sms_host_set_instr_hook(instr_hook, d);
}

static void unhook(void)
{
    if (!sms_host_is_running())
        return;
    sms_host_set_instr_hook(NULL, NULL);
    sms_host_set_bus_hook(NULL, NULL, 0);
}

void smsdebug_attach(smsdebug *d)
{
    if (!d)
        return;
    sms_host_lock();
    pthread_mutex_lock(&d->lock);
    if (!atomic_load(&d->attached))
    {
        atomic_store(&d->attached, 1);
        atomic_store(&d->stop_req, 1);
        atomic_store(&d->mode, MODE_RUN);
        hook(d);
    }
    bump(d);
    pthread_mutex_unlock(&d->lock);
    sms_host_unlock();
}

void smsdebug_detach(smsdebug *d)
{
    if (!d)
        return;
    sms_host_lock();
    pthread_mutex_lock(&d->lock);
    atomic_store(&d->attached, 0);
    atomic_store(&d->stop_req, 0);
    atomic_store(&d->mode, MODE_RUN);
    unhook();
    pthread_cond_broadcast(&d->cond);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    sms_host_unlock();
    /* wait for the parked thread to leave the hook */
    for (int i = 0; i < 2000 && atomic_load(&d->stopped); i++)
    {
        struct timespec ts = { 0, 1000000L };
        nanosleep(&ts, NULL);
    }
}

int smsdebug_is_attached(smsdebug *d)
{
    return d && atomic_load(&d->attached);
}

void smsdebug_power_down(smsdebug *d)
{
    if (!d)
        return;
    d->was_attached = atomic_load(&d->attached);
    smsdebug_detach(d);
}

void smsdebug_power_up(smsdebug *d)
{
    if (!d)
        return;
    if (d->was_attached)
        smsdebug_attach(d);
    d->was_attached = 0;
}

void smsdebug_set_cart_path(smsdebug *d, const char *path)
{
    if (d)
        snprintf(d->cart_path, sizeof d->cart_path, "%s", path ? path : "");
}

symtab *smsdebug_symbols(smsdebug *d)
{
    return &d->syms;
}

int smsdebug_is_stopped(smsdebug *d)
{
    return d && atomic_load(&d->stopped);
}

void smsdebug_stop(smsdebug *d)
{
    if (!d)
        return;
    if (!atomic_load(&d->attached))
        smsdebug_attach(d);
    atomic_store(&d->stop_req, 1);
}

/* Leave the parked state with a mode. */
static void go(smsdebug *d, int mode)
{
    pthread_mutex_lock(&d->lock);
    atomic_store(&d->mode, mode);
    atomic_store(&d->stop_req, 0);
    atomic_store(&d->stopped, 0);
    pthread_cond_broadcast(&d->cond);
    bump(d);
    pthread_mutex_unlock(&d->lock);
}

void smsdebug_resume(smsdebug *d)
{
    if (d && atomic_load(&d->stopped))
        go(d, MODE_RUN);
}

int smsdebug_stop_reason(smsdebug *d, char *dst, int dstsz, int *address)
{
    int n;
    if (!d || !dst || dstsz <= 0)
        return 0;
    pthread_mutex_lock(&d->lock);
    n = snprintf(dst, (size_t)dstsz, "%s", atomic_load(&d->stopped) ? d->reason : "running");
    if (address)
        *address = atomic_load(&d->stopped) ? d->reason_addr : -1;
    pthread_mutex_unlock(&d->lock);
    return n;
}

unsigned smsdebug_generation(smsdebug *d)
{
    return d ? atomic_load(&d->generation) : 0;
}

/* ---- stepping ---------------------------------------------------------------- */

static int decode_at(sms_machine_t *m, uint16_t pc, z80d_insn *ins)
{
    uint8_t code[4];
    for (int i = 0; i < 4; i++)
        code[i] = sms_machine_peek(m, (uint16_t)(pc + i));
    return z80_disassemble(ins, pc, code);
}

void smsdebug_step(smsdebug *d)
{
    if (!d || !atomic_load(&d->stopped))
        return;
    d->count = 1;
    go(d, MODE_STEP);
}

void smsdebug_step_over(smsdebug *d)
{
    sms_machine_t *m;
    z80d_insn ins;
    int over = 0;

    if (!d)
        return;
    sms_host_lock();
    m = machine();
    if (m && atomic_load(&d->stopped))
    {
        decode_at(m, cur_pc(m), &ins);
        if ((ins.flags & (Z80D_CALL | Z80D_BLOCK | Z80D_HALT)) ||
            ((ins.flags & Z80D_RELATIVE) && (ins.flags & Z80D_COND) && ins.bytes[0] == 0x10))
        {
            /* CALL/RST, the block repeats, HALT and DJNZ: to the next one */
            d->target = (uint16_t)(cur_pc(m) + ins.len);
            d->sp_guard = m->cpu.sp;
            over = 1;
        }
    }
    sms_host_unlock();
    if (over)
        go(d, MODE_OVER);
    else
        smsdebug_step(d);
}

void smsdebug_step_out(smsdebug *d)
{
    sms_machine_t *m;
    int ok = 0;

    if (!d)
        return;
    sms_host_lock();
    m = machine();
    if (m && atomic_load(&d->stopped))
    {
        /* to the return address the CALL pushed, with an SP-depth guard;
         * documented-approximate: a routine that juggles its own return
         * address defeats it, and then the machine runs on rather than
         * stopping wrong */
        d->target = (uint16_t)(sms_machine_peek(m, m->cpu.sp) |
                               (sms_machine_peek(m, (uint16_t)(m->cpu.sp + 1)) << 8));
        d->sp_guard = m->cpu.sp;
        ok = 1;
    }
    sms_host_unlock();
    if (ok)
        go(d, MODE_OUT);
}

void smsdebug_scanline(smsdebug *d, int n)
{
    sms_machine_t *m;
    int ok = 0;

    if (!d)
        return;
    sms_host_lock();
    m = machine();
    if (m && atomic_load(&d->stopped))
    {
        d->last_vpos = sms_vdp_beam_vpos(&m->vdp, m->cycles);
        d->count = n > 0 ? n : 1;
        ok = 1;
    }
    sms_host_unlock();
    if (ok)
        go(d, MODE_SCANLINE);
}

void smsdebug_frame(smsdebug *d, int n)
{
    sms_machine_t *m;
    int ok = 0;

    if (!d)
        return;
    sms_host_lock();
    m = machine();
    if (m && atomic_load(&d->stopped))
    {
        d->target_frame = m->vdp.frame_count + (uint64_t)(n > 0 ? n : 1);
        ok = 1;
    }
    sms_host_unlock();
    if (ok)
        go(d, MODE_FRAME);
}

void smsdebug_run_to(smsdebug *d, uint16_t addr)
{
    if (!d || !atomic_load(&d->stopped))
        return;
    d->target = addr;
    go(d, MODE_RUNTO);
}

/* ---- CPU ---------------------------------------------------------------------- */

static void cpu_get_locked(sms_machine_t *m, smsdebug_cpu *o)
{
    const z80_t *c;

    memset(o, 0, sizeof *o);
    if (!m)
        return;
    c = &m->cpu;
    o->pc = cur_pc(m);
    o->sp = c->sp;
    o->af = c->af; o->bc = c->bc; o->de = c->de; o->hl = c->hl;
    o->ix = c->ix; o->iy = c->iy; o->wz = c->wz;
    o->af2 = c->af2; o->bc2 = c->bc2; o->de2 = c->de2; o->hl2 = c->hl2;
    o->i = c->i; o->r = c->r; o->im = c->im;
    o->iff1 = c->iff1; o->iff2 = c->iff2;
    o->halted = (c->pins & Z80_HALT) != 0;
    o->sf = (c->f >> 7) & 1; o->zf = (c->f >> 6) & 1; o->yf = (c->f >> 5) & 1;
    o->hf = (c->f >> 4) & 1; o->xf = (c->f >> 3) & 1; o->pf = (c->f >> 2) & 1;
    o->nf = (c->f >> 1) & 1; o->cf = c->f & 1;
    o->int_line = m->vdp.n_int_state == 0;
    o->nmi_line = m->vdp.n_nmi_state == 0;
    o->cycles = m->cycles;
    o->vpos = sms_vdp_beam_vpos(&m->vdp, m->cycles);
    o->hpos = sms_vdp_beam_hpos(&m->vdp, m->cycles);
    o->vcount = sms_vdp_vcount_read(&m->vdp, m->cycles);
    o->hcount = beam_hcount(m);
    o->frame = (uint32_t)m->vdp.frame_count;
}

void smsdebug_cpu_get(smsdebug *d, smsdebug_cpu *o)
{
    (void)d;
    sms_host_lock();
    cpu_get_locked(machine(), o);
    sms_host_unlock();
}

static void set_flag(z80_t *c, int bit, int on)
{
    if (on)
        c->f = (uint8_t)(c->f | (1u << bit));
    else
        c->f = (uint8_t)(c->f & ~(1u << bit));
}

void smsdebug_cpu_set(smsdebug *d, int reg, int v)
{
    sms_machine_t *m;
    z80_t *c;

    if (!d)
        return;
    sms_host_lock();
    m = machine();
    if (!m || !atomic_load(&d->stopped))
    {
        sms_host_unlock();
        return;
    }
    c = &m->cpu;
    switch (reg)
    {
    case SMS_REG_PC:
        /* The opcode at the old PC is already fetched: redirect the CPU and
         * run the new fetch, so it stands on the new PC as it would have. */
        m->pins = z80_prefetch(&m->cpu, (uint16_t)(v & 0xffff));
        do
            sms_machine_tick_quiet(m);
        while (!z80_opdone(&m->cpu));
        pthread_mutex_lock(&d->lock);
        d->reason_addr = v & 0xffff;
        pthread_mutex_unlock(&d->lock);
        break;
    case SMS_REG_SP:  c->sp = (uint16_t)v; break;
    case SMS_REG_AF:  c->af = (uint16_t)v; break;
    case SMS_REG_BC:  c->bc = (uint16_t)v; break;
    case SMS_REG_DE:  c->de = (uint16_t)v; break;
    case SMS_REG_HL:  c->hl = (uint16_t)v; break;
    case SMS_REG_IX:  c->ix = (uint16_t)v; break;
    case SMS_REG_IY:  c->iy = (uint16_t)v; break;
    case SMS_REG_AF2: c->af2 = (uint16_t)v; break;
    case SMS_REG_BC2: c->bc2 = (uint16_t)v; break;
    case SMS_REG_DE2: c->de2 = (uint16_t)v; break;
    case SMS_REG_HL2: c->hl2 = (uint16_t)v; break;
    case SMS_REG_I:   c->i = (uint8_t)v; break;
    case SMS_REG_R:   c->r = (uint8_t)v; break;
    case SMS_REG_IM:  c->im = (uint8_t)(v & 3) == 3 ? 2 : (uint8_t)(v & 3); break;
    case SMS_REG_IFF1: c->iff1 = v != 0; break;
    case SMS_REG_IFF2: c->iff2 = v != 0; break;
    case SMS_REG_A:   c->a = (uint8_t)v; break;
    case SMS_REG_F:   c->f = (uint8_t)v; break;
    case SMS_REG_B:   c->b = (uint8_t)v; break;
    case SMS_REG_C:   c->c = (uint8_t)v; break;
    case SMS_REG_D:   c->d = (uint8_t)v; break;
    case SMS_REG_E:   c->e = (uint8_t)v; break;
    case SMS_REG_H:   c->h = (uint8_t)v; break;
    case SMS_REG_L:   c->l = (uint8_t)v; break;
    case SMS_FLAG_S:  set_flag(c, 7, v); break;
    case SMS_FLAG_Z:  set_flag(c, 6, v); break;
    case SMS_FLAG_H:  set_flag(c, 4, v); break;
    case SMS_FLAG_PV: set_flag(c, 2, v); break;
    case SMS_FLAG_N:  set_flag(c, 1, v); break;
    case SMS_FLAG_C:  set_flag(c, 0, v); break;
    default: break;
    }
    sms_host_unlock();
    bump(d);
}

/* ---- VDP ---------------------------------------------------------------------- */

static void snap(const sms_machine_t *m, smsvdp_snap *s)
{
    memcpy(s->vram, m->vdp.vram, sizeof s->vram);
    memcpy(s->cram, m->vdp.CRAM, sizeof s->cram);
    memcpy(s->reg, m->vdp.reg, sizeof s->reg);
    s->mode = m->vdp.vdp_mode;
    s->y_pixels = m->vdp.y_pixels;
    s->kind_5246 = m->vdp.kind == SMS_VDP_5246;
    memcpy(s->pens, m->vdp.pens, sizeof s->pens);
}

static void vdp_get_locked(sms_machine_t *m, smsdebug_vdp *o)
{
    smsvdp_snap *s;
    smsvdp_tables t;
    const sms_vdp_t *v;

    memset(o, 0, sizeof *o);
    if (!m)
        return;
    v = &m->vdp;
    s = malloc(sizeof *s);
    if (!s)
        return;
    snap(m, s);
    smsvdp_get_tables(s, &t);

    memcpy(o->reg, v->reg, sizeof o->reg);
    o->status = v->status;
    o->addr = v->addr & 0x3fff;
    o->code = v->addrmode;
    o->buffer = v->buffer;
    o->second_byte = v->pending_control_write;
    o->line_counter = v->line_counter;
    o->hcounter = v->hcounter;
    o->mode = v->vdp_mode;
    o->mode_name = smsvdp_mode_name(v->vdp_mode, v->y_pixels);
    o->y_pixels = v->y_pixels;
    o->kind_5246 = v->kind == SMS_VDP_5246;
    o->is_pal = v->is_pal;
    o->display_on = (v->reg[1] >> 6) & 1;
    o->vint_on = (v->reg[1] >> 5) & 1;
    o->hint_on = (v->reg[0] >> 4) & 1;
    o->sprites_16 = (v->reg[1] >> 1) & 1;
    o->sprites_zoom = v->reg[1] & 1;
    o->left_column_blank = (v->reg[0] >> 5) & 1;
    o->hscroll_lock_top = (v->reg[0] >> 6) & 1;
    o->vscroll_lock_right = (v->reg[0] >> 7) & 1;
    o->sprite_shift = (v->reg[0] >> 3) & 1;
    o->name_base = t.name;
    o->sat_base = t.sprite_attr;
    o->sprite_pattern_base = t.sprite_pattern;
    o->color_base = t.color;
    o->pattern_base = t.pattern;
    o->scroll_x = v->reg[8];
    o->scroll_y = v->reg[9];
    o->backdrop = v->reg[7] & 15;
    o->vint_pending = ((v->status | v->pending_status) & 0x80) != 0;
    o->hint_pending = v->pending_hint || v->hint_occurred;
    o->int_line = v->n_int_state == 0;
    o->nmi_line = v->n_nmi_state == 0;
    o->pause_held = v->n_nmi_in_state == 0;
    o->vpos = sms_vdp_beam_vpos(v, m->cycles);
    o->hpos = sms_vdp_beam_hpos(v, m->cycles);
    o->vcount = sms_vdp_vcount_read(v, m->cycles);
    o->frame = (uint32_t)v->frame_count;
    memcpy(o->cram, v->CRAM, sizeof o->cram);
    for (int i = 0; i < 32; i++)
        o->cram_rgb[i] = v->pens[v->CRAM[i] & 0x3f];
    free(s);
}

void smsdebug_vdp_get(smsdebug *d, smsdebug_vdp *o)
{
    (void)d;
    sms_host_lock();
    vdp_get_locked(machine(), o);
    sms_host_unlock();
}

/* A copy of the VDP's state under the run lock, for the pure renderers;
 * NULL when not running. The caller frees it. */
static smsvdp_snap *take_snap(void)
{
    smsvdp_snap *s = NULL;
    sms_machine_t *m;

    sms_host_lock();
    m = machine();
    if (m && (s = malloc(sizeof *s)) != NULL)
        snap(m, s);
    sms_host_unlock();
    return s;
}

int smsdebug_vdp_describe_register(smsdebug *d, int reg, char *dst, int dstsz)
{
    smsvdp_snap *s;
    int n;

    (void)d;
    if (!dst || dstsz <= 0 || (s = take_snap()) == NULL)
        return 0;
    n = smsvdp_describe_register(s, reg, dst, dstsz);
    free(s);
    return n;
}

int smsdebug_vdp_view(smsdebug *d, int view, int palette, uint32_t *dst, int *width, int *height)
{
    smsvdp_snap *s;

    (void)d;
    if ((s = take_snap()) == NULL)
        return 0;
    switch (view)
    {
    case SMSDEBUG_VIEW_NAMETABLE: smsvdp_render_nametable(s, dst, width, height, SMSSESSION_ACCENT_RGB); break;
    case SMSDEBUG_VIEW_TILES:     smsvdp_render_tiles(s, palette, dst, width, height); break;
    case SMSDEBUG_VIEW_SPRITES:   smsvdp_render_sprites(s, dst, width, height, SMSSESSION_ACCENT_RGB); break;
    case SMSDEBUG_VIEW_PALETTE:   smsvdp_render_palette(s, dst, width, height); break;
    default: free(s); return 0;
    }
    free(s);
    return 1;
}

int smsdebug_sprites_get(smsdebug *d, smsdebug_sprite out[64])
{
    smsvdp_snap *s;
    smsvdp_sprite tmp[64];
    int n;

    (void)d;
    if ((s = take_snap()) == NULL)
        return 0;
    n = smsvdp_sprites(s, tmp);
    for (int i = 0; i < n; i++)
    {
        out[i].y = tmp[i].y;
        out[i].x = tmp[i].x;
        out[i].tile = tmp[i].tile;
        out[i].color = tmp[i].color;
        out[i].early_clock = tmp[i].early_clock;
        out[i].visible = tmp[i].visible;
    }
    free(s);
    return n;
}

/* ---- sound and I/O -------------------------------------------------------------- */

static void io_get_locked(sms_machine_t *m, smsdebug_io *o)
{
    memset(o, 0, sizeof *o);
    if (!m)
        return;
    for (int i = 0; i < 3; i++)
    {
        o->tone_period[i] = m->psg.reg[i * 2];
        o->tone_volume[i] = m->psg.reg[i * 2 + 1] & 0x0f;
    }
    o->noise_mode = (m->psg.reg[6] >> 2) & 1;
    o->noise_rate = m->psg.reg[6] & 3;
    o->noise_volume = m->psg.reg[7] & 0x0f;
    o->lfsr = (uint16_t)m->psg.RNG;
    o->fm_present = m->fm != NULL;
    if (m->fm)
        sms_fm_regs(m->fm, o->fm_regs);
    o->psg_audible = m->mix.psg_gain > 0.0f;
    o->fm_audible = m->fm && m->mix.fm_gain > 0.0f;
    o->mem_ctrl = m->mem_ctrl_reg;
    o->io_ctrl = m->io_ctrl_reg;
    o->cart_enabled = (m->mem_device_enabled & SMS_ENABLE_CART) != 0;
    o->bios_enabled = (m->mem_device_enabled & SMS_ENABLE_BIOS) != 0;
    o->ram_enabled = !(m->mem_device_enabled & SMS_ENABLE_EXT_RAM);
    o->io_enabled = !(m->mem_ctrl_reg & SMS_IO_CHIP);
    o->bios_present = m->BIOS != NULL;
    memcpy(o->bios_page, m->bios_page, 3);
    for (int i = 0; i < 4; i++)
        o->mapper[i] = m->mainram[0x1ffc + i];
    o->port_dc = sms_machine_io_peek(m, 0xdc);
    o->port_dd = sms_machine_io_peek(m, 0xdd);
    o->pad[0] = m->pad[0];
    o->pad[1] = m->pad[1];
    o->pause_held = m->vdp.n_nmi_in_state == 0;
    o->reset_held = m->reset_btn;
    o->japanese = m->model->ioctrl_region_is_japan;
    o->console_name = m->model->name;
}

void smsdebug_io_get(smsdebug *d, smsdebug_io *o)
{
    (void)d;
    sms_host_lock();
    io_get_locked(machine(), o);
    sms_host_unlock();
}

/* ---- memory ------------------------------------------------------------------- */

int smsdebug_read(smsdebug *d, uint16_t addr, uint8_t *dst, int n)
{
    sms_machine_t *m;
    (void)d;
    sms_host_lock();
    m = machine();
    if (m)
        for (int i = 0; i < n; i++)
            dst[i] = sms_machine_peek(m, (uint16_t)(addr + i));
    sms_host_unlock();
    return m ? n : 0;
}

void smsdebug_write(smsdebug *d, uint16_t addr, uint8_t value)
{
    sms_machine_t *m;
    sms_host_lock();
    m = machine();
    if (m)
        sms_machine_poke(m, addr, value);
    sms_host_unlock();
    if (m)
        bump(d);
}

void smsdebug_ram_get(smsdebug *d, uint8_t out[8192])
{
    sms_machine_t *m;
    (void)d;
    sms_host_lock();
    m = machine();
    if (m)
        memcpy(out, m->mainram, 8192);
    else
        memset(out, 0, 8192);
    sms_host_unlock();
}

int smsdebug_vram_read(smsdebug *d, uint16_t addr, uint8_t *dst, int n)
{
    sms_machine_t *m;
    (void)d;
    sms_host_lock();
    m = machine();
    if (m)
        for (int i = 0; i < n; i++)
            dst[i] = m->vdp.vram[(addr + i) & 0x3fff];
    sms_host_unlock();
    return m ? n : 0;
}

int smsdebug_vram_write(smsdebug *d, uint16_t addr, const uint8_t *src, int n)
{
    sms_machine_t *m;
    sms_host_lock();
    m = machine();
    if (m)
        for (int i = 0; i < n; i++)
            m->vdp.vram[(addr + i) & 0x3fff] = src[i];
    sms_host_unlock();
    if (!m)
        return 0;
    bump(d);
    return n;
}

void smsdebug_cram_write(smsdebug *d, int index, uint8_t value)
{
    sms_machine_t *m;
    if (index < 0 || index > 31)
        return;
    sms_host_lock();
    m = machine();
    if (m)
    {
        m->vdp.CRAM[index] = value;
        m->vdp.cram_dirty = true;
    }
    sms_host_unlock();
    if (m)
        bump(d);
}

/* ---- labels ---------------------------------------------------------------------- */

int smsdebug_bank_at(smsdebug *d, uint16_t addr)
{
    sms_cart_t *c;
    int16_t banks[48];

    (void)d;
    if (addr >= 0xc000)
        return SYM_BANK_ANY;
    sms_host_lock();
    c = sms_host_cart();
    if (c)
        sms_cart_page_banks(c, banks);
    sms_host_unlock();
    if (!c)
        return SYM_BANK_ANY;
    if (banks[addr >> 10] < 0 || banks[addr >> 10] >= SMSMAP_REV_BANK)
        return SYM_BANK_ANY;
    return banks[addr >> 10] >> 1;   /* 8K SRAM bank -> 16K ROM bank */
}

int smsdebug_label_address(smsdebug *d, const char *label)
{
    return d ? symtab_find(&d->syms, label) : -1;
}

int smsdebug_address_label(smsdebug *d, uint16_t addr, char *dst, int dstsz)
{
    const char *l;
    if (!d || !dst || dstsz <= 0)
        return 0;
    l = symtab_lookup(&d->syms, addr, smsdebug_bank_at(d, addr));
    return snprintf(dst, (size_t)dstsz, "%s", l ? l : "");
}

int smsdebug_set_label(smsdebug *d, uint16_t addr, const char *label)
{
    if (!d)
        return -1;
    symtab_remove_at(&d->syms, addr);
    if (label && *label)
        symtab_add(&d->syms, addr, SYM_BANK_ANY, label, 0);
    bump(d);
    return 0;
}

int smsdebug_load_symbols(smsdebug *d, const char *path, char *msg, int msgsz)
{
    int n;
    if (!d)
        return -1;
    if (!path || !*path)
    {
        static const char *const exts[] = { ".sym", ".map", ".noi", NULL };
        char cand[1100];
        const char *dot;
        size_t stem;

        if (!d->cart_path[0])
        {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "No cartridge file is open; choose a symbol file.");
            return -1;
        }
        dot = strrchr(d->cart_path, '.');
        stem = dot ? (size_t)(dot - d->cart_path) : strlen(d->cart_path);
        for (int i = 0; exts[i]; i++)
        {
            FILE *f;
            snprintf(cand, sizeof cand, "%.*s%s", (int)stem, d->cart_path, exts[i]);
            f = fopen(cand, "r");
            if (f)
            {
                fclose(f);
                n = symtab_load(&d->syms, cand, msg, msgsz);
                bump(d);
                return n;
            }
        }
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "No .sym, .map or .noi next to %s", d->cart_path);
        return -1;
    }
    n = symtab_load(&d->syms, path, msg, msgsz);
    bump(d);
    return n;
}

/* ---- disassembly ------------------------------------------------------------------ */

static void fill_line(smsdebug *d, sms_machine_t *m, uint16_t addr, smsdebug_line *l, uint16_t pc)
{
    z80d_insn ins;
    const char *lab;
    const sms_model_info_t *mi = m->model;

    memset(l, 0, sizeof *l);
    decode_at(m, addr, &ins);
    l->address = addr;
    l->length = ins.len;
    l->is_pc = addr == pc;
    l->has_breakpoint = d->map_exec[addr] != 0;
    for (int i = 0, o = 0; i < ins.len && o < (int)sizeof l->bytes - 3; i++)
        o += snprintf(l->bytes + o, sizeof l->bytes - (size_t)o, "%s%02X", i ? " " : "", ins.bytes[i]);
    lab = symtab_lookup(&d->syms, addr, smsdebug_bank_at(d, addr));
    if (lab)
        snprintf(l->label, sizeof l->label, "%s", lab);
    snprintf(l->disasm, sizeof l->disasm, "%s", ins.text);

    if (ins.flags & (Z80D_JUMP | Z80D_CALL))
    {
        const char *t = symtab_lookup(&d->syms, ins.target, smsdebug_bank_at(d, ins.target));
        if (t)
            snprintf(l->comment, sizeof l->comment, "%s", t);
    }
    else
    {
        /* IN A,(n) / OUT (n),A: the port's name */
        uint8_t op = ins.bytes[0];
        if ((op == 0xdb || op == 0xd3) && ins.len == 2)
        {
            const char *pn = sms_port_name(ins.bytes[1], mi->is_smsj, mi->is_mark_iii);
            if (pn)
                snprintf(l->comment, sizeof l->comment, "%s", pn);
        }
        else
        {
            /* an absolute operand that names a place: LD (nn),A, LD HL,nn ... */
            const char *p = strchr(ins.text, '$');
            if (p && strlen(p) >= 5)
            {
                unsigned v;
                if (sscanf(p + 1, "%4x", &v) == 1)
                {
                    const char *t = symtab_lookup(&d->syms, (uint16_t)v, smsdebug_bank_at(d, (uint16_t)v));
                    if (t)
                        snprintf(l->comment, sizeof l->comment, "%s", t);
                }
            }
        }
    }
}

int smsdebug_disassemble(smsdebug *d, uint16_t addr, smsdebug_line *out, int max, int *pc_line)
{
    sms_machine_t *m;
    uint16_t pc;
    int n = 0;

    if (pc_line)
        *pc_line = -1;
    if (!d)
        return 0;
    sms_host_lock();
    m = machine();
    if (m)
    {
        pc = cur_pc(m);
        while (n < max)
        {
            fill_line(d, m, addr, &out[n], pc);
            if (out[n].is_pc && pc_line)
                *pc_line = n;
            addr = (uint16_t)(addr + out[n].length);
            n++;
        }
    }
    sms_host_unlock();
    return n;
}

static int row_address_locked(smsdebug *d, sms_machine_t *m, uint16_t addr, int rows);

int smsdebug_row_address(smsdebug *d, uint16_t addr, int rows)
{
    int a;
    sms_host_lock();
    a = row_address_locked(d, machine(), addr, rows);
    sms_host_unlock();
    return a;
}

static int row_address_locked(smsdebug *d, sms_machine_t *m, uint16_t addr, int rows)
{
    z80d_insn ins;

    if (!d || !m || rows == 0)
        return addr;
    if (rows > 0)
    {
        for (int i = 0; i < rows; i++)
            addr = (uint16_t)(addr + decode_at(m, addr, &ins));
        return addr;
    }
    /* Backwards: the longest run of instructions that lands exactly on
     * addr, from as far back as the rows could take. */
    rows = -rows;
    for (int back = rows * 4; back >= 1; back--)
    {
        uint16_t starts[256];
        int count = 0;
        uint16_t a = (uint16_t)(addr - back);
        while (count < 256)
        {
            int dist = (uint16_t)(addr - a);
            if (dist == 0 || dist > back)
                break;
            starts[count++] = a;
            a = (uint16_t)(a + decode_at(m, a, &ins));
        }
        if (a == addr && count >= rows)
            return starts[count - rows];
    }
    return (uint16_t)(addr - rows);
}

/* ---- the cartridge ----------------------------------------------------------------- */

static void cart_get_locked(smsdebug_cart *o)
{
    sms_cart_status_t st;
    sms_cart_t *c = sms_host_cart();
    static const char *const modes[] = { "CONFIG", "game", "app (mailbox live)" };

    memset(o, 0, sizeof *o);
    sms_host_cart_status(&st);
    o->present = st.powered;
    if (!st.powered)
        return;
    o->link_up = st.link_up;
    o->busy = st.busy;
    o->direct = st.direct;
    o->booted_game = st.booted_game;
    o->mode = st.mode;
    o->mode_name = (st.mode >= 0 && st.mode <= 2) ? modes[st.mode] : "?";
    o->mapper = st.mapper;
    snprintf(o->mapper_name, sizeof o->mapper_name, "%s", st.mapper_name ? st.mapper_name : "");
    memcpy(o->bank, st.bank, sizeof o->bank);
    o->ram_enabled = st.ram_en;
    o->ram_writable = st.ram_we;
    o->image_size = st.image_size;
    o->image_crc = st.image_crc;
    o->ram_size = st.ram_size;
    o->claim = st.claim;
    if (c)
        sms_cart_page_banks(c, o->page_bank);
    o->load_state = st.load_state;
    o->load_win = st.load_win;
    o->load_nwin = st.load_nwin;
    o->load_pct = st.load_pct;
    o->boot_state = st.boot_state;
    o->boot_pct = st.boot_pct;
    o->boot_err = st.boot_err;
    o->boot_got = st.boot_got;
    o->boot_total = st.boot_total;
    o->ackseq = st.ackseq;
    o->status = st.status;
    o->last_error = st.err;
    o->reply_cmd = st.reply_cmd;
    o->rxlen = st.rxlen;
    o->bios_phase = st.bios_phase;
    o->snoop_c000 = st.snoop_c000;
    o->snoop_3e = st.snoop_3e;
    o->snoop_3f = st.snoop_3f;
    memcpy(o->snoop_vdp, st.snoop_vdp, sizeof o->snoop_vdp);
    o->queue_depth = st.queue;
}

void smsdebug_cart_get(smsdebug *d, smsdebug_cart *o)
{
    (void)d;
    sms_host_lock();
    cart_get_locked(o);
    sms_host_unlock();
}

int smsdebug_cart_info(smsdebug *d, char *dst, int dstsz)
{
    smsdebug_cart c;
    smsdebug_cart_get(d, &c);
    if (!c.present)
        return snprintf(dst, (size_t)dstsz, "FujiNet cartridge: not powered");
    if (c.mode == FN_MODE_RESIDENT)
        return snprintf(dst, (size_t)dstsz, "FujiNet cartridge: CONFIG, link %s",
                        c.link_up ? "up" : "down");
    return snprintf(dst, (size_t)dstsz, "FujiNet cartridge: %s, %s mapper, %u bytes%s, link %s",
                    c.mode_name, c.mapper_name, c.image_size, c.direct ? " (opened file)" : "",
                    c.link_up ? "up" : "down");
}

/* ---- breakpoints ---------------------------------------------------------------- */

int smsdebug_breakpoint_add(smsdebug *d, int type, uint16_t start, uint16_t end, const char *cond)
{
    smsdebug_breakpoint *b;
    char err[96];
    int id;

    if (!d || !type)
        return -1;
    if (cond && *cond && expr_eval(cond, NULL, NULL, err, sizeof err) != 0)
        return -1;
    if (end < start)
    {
        uint16_t t = start;
        start = end;
        end = t;
    }
    sms_host_lock();
    pthread_mutex_lock(&d->lock);
    if (d->nbp >= MAX_BP)
    {
        pthread_mutex_unlock(&d->lock);
        sms_host_unlock();
        return -1;
    }
    b = &d->bp[d->nbp++];
    memset(b, 0, sizeof *b);
    b->id = id = d->next_id++;
    b->type = type;
    b->start = start;
    b->end = end;
    b->enabled = 1;
    snprintf(b->condition, sizeof b->condition, "%s", cond ? cond : "");
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    sms_host_unlock();
    return id;
}

void smsdebug_breakpoint_remove(smsdebug *d, int id)
{
    if (!d)
        return;
    sms_host_lock();
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < d->nbp; i++)
    {
        if (d->bp[i].id == id)
        {
            memmove(&d->bp[i], &d->bp[i + 1], (size_t)(d->nbp - i - 1) * sizeof d->bp[0]);
            d->nbp--;
            break;
        }
    }
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    sms_host_unlock();
}

void smsdebug_breakpoint_enable(smsdebug *d, int id, int enabled)
{
    if (!d)
        return;
    sms_host_lock();
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < d->nbp; i++)
        if (d->bp[i].id == id)
            d->bp[i].enabled = enabled != 0;
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    sms_host_unlock();
}

int smsdebug_breakpoint_list(smsdebug *d, smsdebug_breakpoint *out, int max)
{
    int n;
    if (!d)
        return 0;
    pthread_mutex_lock(&d->lock);
    n = d->nbp < max ? d->nbp : max;
    memcpy(out, d->bp, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&d->lock);
    return n;
}

void smsdebug_breakpoint_clear(smsdebug *d)
{
    if (!d)
        return;
    sms_host_lock();
    pthread_mutex_lock(&d->lock);
    d->nbp = 0;
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    sms_host_unlock();
}

int smsdebug_breakpoint_check(smsdebug *d, uint16_t addr)
{
    return d && d->map_exec[addr] != 0;
}

int smsdebug_breakpoint_toggle(smsdebug *d, uint16_t addr)
{
    int found = -1;
    if (!d)
        return 0;
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < d->nbp; i++)
        if (d->bp[i].type == SMSDEBUG_BP_EXEC && d->bp[i].start == addr && d->bp[i].end == addr)
        {
            found = d->bp[i].id;
            break;
        }
    pthread_mutex_unlock(&d->lock);
    if (found >= 0)
    {
        smsdebug_breakpoint_remove(d, found);
        return 0;
    }
    return smsdebug_breakpoint_add(d, SMSDEBUG_BP_EXEC, addr, addr, NULL) >= 0;
}

/* ---- trace ---------------------------------------------------------------------- */

void smsdebug_trace_enable(smsdebug *d, int on)
{
    if (d)
    {
        atomic_store(&d->trace_on, on != 0);
        bump(d);
    }
}

int smsdebug_trace_enabled(smsdebug *d)
{
    return d && atomic_load(&d->trace_on);
}

/* The ring is written by the instruction hook, under the run lock. */
int smsdebug_trace_read(smsdebug *d, smsdebug_trace *out, int max)
{
    int n;
    if (!d)
        return 0;
    sms_host_lock();
    n = d->ring_count < max ? d->ring_count : max;
    for (int i = 0; i < n; i++)
        out[i] = d->ring[(d->ring_head - 1 - i + TRACE_N) % TRACE_N];
    sms_host_unlock();
    return n;
}

void smsdebug_trace_clear(smsdebug *d)
{
    if (!d)
        return;
    sms_host_lock();
    d->ring_head = d->ring_count = 0;
    sms_host_unlock();
    bump(d);
}

/* ---- files ------------------------------------------------------------------------ */

static int write_bytes(const char *path, const uint8_t *data, size_t n, char *msg, int msgsz)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n)
    {
        if (f)
            fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
        return -1;
    }
    fclose(f);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "Saved %zu bytes to %s", n, path);
    return 0;
}

static int save_locked(smsdebug *d, sms_machine_t *m, const char *kind, const char *path,
                       char *msg, int msgsz);

int smsdebug_save(smsdebug *d, const char *kind, const char *path, char *msg, int msgsz)
{
    int rc;
    sms_host_lock();
    rc = save_locked(d, machine(), kind, path, msg, msgsz);
    sms_host_unlock();
    return rc;
}

static int save_locked(smsdebug *d, sms_machine_t *m, const char *kind, const char *path,
                       char *msg, int msgsz)
{
    if (!d || !m || !kind || !path)
    {
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "The machine is not running.");
        return -1;
    }
    if (!strcmp(kind, "ram"))
        return write_bytes(path, m->mainram, sizeof m->mainram, msg, msgsz);
    if (!strcmp(kind, "vram"))
        return write_bytes(path, m->vdp.vram, sizeof m->vdp.vram, msg, msgsz);
    if (!strcmp(kind, "cram"))
        return write_bytes(path, m->vdp.CRAM, sizeof m->vdp.CRAM, msg, msgsz);
    if (!strcmp(kind, "arena") || !strcmp(kind, "sram"))
    {
        sms_cart_t *c = sms_host_cart();
        sms_cart_status_t st;
        uint8_t *buf;
        uint32_t n;
        int rc;

        if (!c)
            return -1;
        buf = malloc(SMSMAP_SRAM_SIZE);
        if (!buf)
            return -1;
        if (!strcmp(kind, "arena"))
            n = sms_cart_copy_arena(c, buf, FN_ARENA_SIZE);
        else
        {
            sms_cart_status(c, &st);
            n = sms_cart_copy_sram(c, buf, SMSMAP_SRAM_SIZE);
            if (st.mode != FN_MODE_RESIDENT && st.image_size)
            {
                /* as far as the image fills it, padded to 16K as mapped */
                uint32_t used = (st.image_size + 0x3fff) & ~0x3fffu;
                if (used < n)
                    n = used;
            }
        }
        rc = write_bytes(path, buf, n, msg, msgsz);
        free(buf);
        return rc;
    }
    if (!strcmp(kind, "dis") || !strcmp(kind, "regs"))
    {
        FILE *f = fopen(path, "w");
        if (!f)
        {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
            return -1;
        }
        if (!strcmp(kind, "dis"))
        {
            const uint16_t pc = cur_pc(m);
            uint32_t a = 0;
            while (a < 0xc000)
            {
                smsdebug_line l;
                fill_line(d, m, (uint16_t)a, &l, pc);
                if (l.label[0])
                    fprintf(f, "%s:\n", l.label);
                fprintf(f, "  %04X  %-12s %-24s%s%s\n", l.address, l.bytes, l.disasm,
                        l.comment[0] ? "; " : "", l.comment);
                a += (uint32_t)l.length;
            }
        }
        else
        {
            smsdebug_cpu c;
            smsdebug_vdp v;
            cpu_get_locked(m, &c);
            vdp_get_locked(m, &v);
            fprintf(f, "PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X\n",
                    c.pc, c.sp, c.af, c.bc, c.de, c.hl, c.ix, c.iy);
            fprintf(f, "AF'=%04X BC'=%04X DE'=%04X HL'=%04X I=%02X R=%02X IM=%d IFF1=%d IFF2=%d\n",
                    c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im, c.iff1, c.iff2);
            fprintf(f, "cycles=%llu frame=%u vpos=%d hpos=%d\n",
                    (unsigned long long)c.cycles, c.frame, c.vpos, c.hpos);
            for (int r = 0; r < 11; r++)
                fprintf(f, "VDP R%-2d = %02X\n", r, v.reg[r]);
            fprintf(f, "VDP status=%02X addr=%04X code=%d\n", v.status, v.addr, v.code);
        }
        fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "Unknown save kind %s (dis, ram, vram, cram, sram, arena, regs)", kind);
    return -1;
}
