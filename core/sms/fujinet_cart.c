/* fujinet_cart.c -- the FujiNet SMS cartridge device (see fujinet_cart.h).
 *
 * A C transposition of fujinet-firmware's own MAME cart model
 * (pico/sms/emu/fujinet.cpp, BSD-3-Clause, copyright-holders Thomas
 * Cherryhomes), kept structurally line-for-line where C allows so the two
 * stay diffable: MAME's std::vector buffers become fixed allocations,
 * machine().side_effects_disabled() becomes the `commit` parameter, the
 * PC-equals-address /M1 guess becomes the real /M1, and the memory taps
 * become the snoop calls the machine makes on every cycle.
 *
 * Two things are this app's own, both modelled on the cartridge rather than
 * on MAME:
 *
 *   - The mailbox service (fujimail) runs on a worker thread, as it runs on
 *     the RP2354B's core0 while core1 serves the bus. Hotspot writes go
 *     through a ring (core1 -> core0 on the cart); what fujimail publishes
 *     goes into a shadow of the arena the emulation thread drains, under
 *     one lock, so ACKSEQ -- published last -- still arrives last. The load
 *     sequence (fuji_load) stays on the emulation thread: it owns the load
 *     window and the page tables the bus reads. The pattern is the NES
 *     sibling's FujiNetCart.
 *   - FN_R_LINK is published after every service pass, as the firmware's
 *     fujinet.c does (the MAME device leaves it alone).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 * (the transposed device logic: BSD-3-Clause, see COMPLIANCE.md)
 */

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fujinet_cart.h"

/* The cartridge firmware's own sources, staged verbatim by
 * cmake/StageFujiProto.cmake (see that file and COMPLIANCE.md). */
#include "fuji_mailbox.h"
#include "fujimail.h"
#include "fujitcp.h"
#include "fujitcp_host.h"
#include "fujiconfigrom.h"
#include "smsloaderrom.h"
#include "sms_cart.h"
#include "smsmap.h"
#include "fuji_load.h"

#define STORE_RAM_MAX  (256u * 1024)
#define STORE_MAX      0x110000u
#define QUEUE_SIZE     4096
#define SWAP_SYNC_MS   2000

struct sms_cart {
    uint8_t arena[FN_ARENA_SIZE];
    uint8_t window[FN_LOADWIN_SIZE];
    uint8_t *sram;                       /* SMSMAP_SRAM_SIZE */
    uint8_t resident[FN_RESIDENT_MAX];   /* 32K served RESIDENT */
    uint8_t *file;                       /* the opened image as on disk */
    uint32_t file_len;
    uint8_t *store[2];                   /* the cart's RAM and flash tiers */
    uint32_t store_len[2];
    uint8_t *cfg;                        /* the pushed .cfg sibling */
    uint32_t cfg_len;
    int open_tier;
    char cfg_mapper[64];

    const uint8_t *ptab_res[SMS_PAGES];
    const uint8_t *ptab_app[SMS_PAGES];
    const uint8_t *ptab_game[SMS_PAGES];
    sms_bus_t bus;
    smsmap_t res_map, game_map;
    smsmap_t *live;
    smsmap_plan_t direct_plan;
    smsmap_plan_t resident_plan;        /* an opened client serving RESIDENT */
    bool resident_client;
    fuji_load_t loader;
    fuji_load_port_t load_port;
    bool game, mbox, ram_we, load;
    bool direct;
    bool powered;
    bool debug;
    char boip[128];
    bool have_boip;

    /* the mailbox worker (core0) */
    bool sync;
    bool worker_running;
    pthread_t worker;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_cond;
    uint16_t queue[QUEUE_SIZE];
    atomic_uint qhead, qtail;
    atomic_bool stop;
    pthread_mutex_t mail_lock;           /* held while the worker is inside fujimail */
    pthread_mutex_t load_lock;           /* the fuji_load_t, shared with the worker */
    pthread_mutex_t publish_lock;
    uint8_t published[FN_ARENA_SIZE];
    unsigned pub_lo, pub_hi;
    atomic_bool publish_ready;
    atomic_bool busy;
};

/* fujimail's port is C function pointers with no context argument; one
 * slot, one cart, as on the hardware. */
static sms_cart_t *s_cart = NULL;

/* Which side of the cart a callback runs on: the worker publishes through
 * the shadow, the emulation thread straight into the arena. */
static _Thread_local bool t_on_worker = false;

static uint8_t bitswap8(uint8_t v)
{
    v = (uint8_t)((v & 0xF0) >> 4 | (v & 0x0F) << 4);
    v = (uint8_t)((v & 0xCC) >> 2 | (v & 0x33) << 2);
    return (uint8_t)((v & 0xAA) >> 1 | (v & 0x55) << 1);
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/*-------------------------------------------------
    publishing into the arena
-------------------------------------------------*/

static void drain_published(sms_cart_t *c)
{
    pthread_mutex_lock(&c->publish_lock);
    for (unsigned i = c->pub_lo; i < c->pub_hi; i++)
        c->arena[i] = c->published[i];
    c->pub_lo = FN_R_PAINT_END;
    c->pub_hi = 0;
    atomic_store_explicit(&c->publish_ready, false, memory_order_relaxed);
    pthread_mutex_unlock(&c->publish_lock);
}

/* sega8_fujinet_device::poke, on the emulation thread. Into the shadow too:
 * a drain copies the whole pending range, not just the worker's bytes, so a
 * byte written only here would otherwise come back as the shadow's stale
 * value with the worker's next publish. */
static void poke_direct(sms_cart_t *c, unsigned offset, uint8_t value)
{
    if (offset >= FN_R_PAINT_END)
        return;
    if (atomic_load_explicit(&c->publish_ready, memory_order_acquire))
        drain_published(c);
    pthread_mutex_lock(&c->publish_lock);
    c->published[offset] = value;
    c->arena[offset] = value;
    pthread_mutex_unlock(&c->publish_lock);

    if (offset == FN_R_LOAD_STATE && value == FN_LOAD_DONE && c->loader.next != &c->res_map && c->debug)
        fprintf(stderr, "fujinet: loaded kind=%s crc=%08X size=%u ram=%u claim=%d\n",
                smsmap_kind_name(c->loader.plan.kind), c->loader.plan.crc,
                c->loader.plan.size, c->loader.plan.ram_size, c->loader.plan.claim);
}

/* Every byte is offered, not just ACKSEQ, so the busy flag and a mount's
 * progress can be watched while the transaction is out. */
static void poke_worker(sms_cart_t *c, unsigned offset, uint8_t value)
{
    if (offset >= FN_R_PAINT_END)
        return;
    pthread_mutex_lock(&c->publish_lock);
    c->published[offset] = value;
    if (offset < c->pub_lo)
        c->pub_lo = offset;
    if (offset + 1 > c->pub_hi)
        c->pub_hi = offset + 1;
    pthread_mutex_unlock(&c->publish_lock);
    atomic_store_explicit(&c->publish_ready, true, memory_order_release);
}

/*-------------------------------------------------
    C port callbacks
-------------------------------------------------*/

static void c_poke(unsigned offset, uint8_t value)
{
    if (t_on_worker)
        poke_worker(s_cart, offset, value);
    else
        poke_direct(s_cart, offset, value);
}

static void c_set_load(bool on) { s_cart->load = on; }

static bool c_link_up(void) { return fujitcp_active(); }

static void publish_link(sms_cart_t *c)
{
    c_poke(FN_R_LINK, fujitcp_active() ? 1 : 0);
    (void)c;
}

/* FN_R_STATUS_BUSY is declared by the protocol but never raised by the
 * firmware, whose transaction is synchronous with its bus loop. Here it is
 * real: the console keeps running while this blocks. fujimail republishes
 * FN_R_STATUS itself once it has the reply. */
static fb_status_t c_transact(uint8_t device, uint8_t command,
                              const fb_param_t *params, unsigned nparams,
                              const uint8_t *payload, uint16_t payload_len,
                              uint32_t timeout_ms, fb_reply_t *reply)
{
    sms_cart_t *c = s_cart;
    fb_status_t st;

    atomic_store(&c->busy, true);
    c_poke(FN_R_STATUS, FN_R_STATUS_LINK | FN_R_STATUS_BUSY);
    st = fujitcp_transact(device, command, params, nparams, payload, payload_len,
                          timeout_ms, reply);
    if (st == FB_ENOLINK)
        fujitcp_close();   /* the next transaction reconnects */
    atomic_store(&c->busy, false);
    return st;
}

/* The console reaches its first transaction before the link may be up (and
 * FujiNet can be restarted from its web UI under a running console): try
 * to connect for up to `ms`. */
static void c_wait_link(uint32_t ms)
{
    sms_cart_t *c = s_cart;
    const uint64_t deadline = now_ms() + ms;

    while (!fujitcp_active() && !atomic_load(&c->stop))
    {
        if (fujitcp_init(c->have_boip ? c->boip : NULL) == 0)
            break;
        if (now_ms() >= deadline)
            break;
        sleep_ms(250);
    }
    publish_link(c);
}

static uint8_t c_stream_open(int stream, uint32_t size);
static void c_stream_write(int stream, const uint8_t *chunk, unsigned len);
static uint8_t c_stream_close(int stream, uint32_t got, bool aborted);
static void c_arm_swap(void);

static void c_on_txn(const fujimail_txn_t *t)
{
    char txt[40];
    unsigned k, m = 0;

    for (k = 0; k < t->rxlen && m < sizeof txt - 1; k++)
    {
        uint8_t ch = t->rx[k];
        if (ch == 0)
            break;
        txt[m++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    txt[m] = '\0';
    fprintf(stderr,
            "fujinet: dev=%02X cmd=%02X nparam=%u txlen=%u seq=%u"
            " -> err=%d reply=%02X rxlen=%u%s%s%s\n",
            t->device, t->command, t->nparam, t->txlen, t->seq,
            t->status, t->reply_cmd, t->rxlen,
            m ? " \"" : "", txt, m ? "\"" : "");
}

static void c_on_dbc(fujimail_dbc_ev_t ev, int stream, uint32_t expect, unsigned got, bool aborted)
{
    if (ev == FUJIMAIL_DBC_OPEN)
        fprintf(stderr, "fujinet: DBC open stream=%d size=%u\n", stream, expect);
    else
        fprintf(stderr, "fujinet: DBC close stream=%d got=%u%s\n",
                stream, got, aborted ? " ABORTED" : "");
}

static const fujimail_port_t debug_port = {
    c_poke, c_link_up, c_transact, fujitcp_send_bare,
    c_stream_open, c_stream_write, c_stream_close, c_arm_swap,
    c_wait_link, NULL, c_on_txn, c_on_dbc,
};

static const fujimail_port_t quiet_port = {
    c_poke, c_link_up, c_transact, fujitcp_send_bare,
    c_stream_open, c_stream_write, c_stream_close, c_arm_swap,
    c_wait_link, NULL, NULL, NULL,
};

/*-------------------------------------------------
    the mailbox worker (the cart's core0)
-------------------------------------------------*/

static unsigned queue_depth(sms_cart_t *c)
{
    unsigned h = atomic_load_explicit(&c->qhead, memory_order_acquire);
    unsigned t = atomic_load_explicit(&c->qtail, memory_order_acquire);
    return (h + QUEUE_SIZE - t) % QUEUE_SIZE;
}

static void *worker_main(void *arg)
{
    sms_cart_t *c = arg;

    t_on_worker = true;
    for (;;)
    {
        pthread_mutex_lock(&c->queue_lock);
        while (queue_depth(c) == 0 && !atomic_load(&c->stop))
            pthread_cond_wait(&c->queue_cond, &c->queue_lock);
        pthread_mutex_unlock(&c->queue_lock);
        if (atomic_load(&c->stop))
            break;

        /* the lock is taken BEFORE an event leaves the ring, so "ring empty
         * and lock free" (sync_worker) really means idle */
        pthread_mutex_lock(&c->mail_lock);
        while (!atomic_load(&c->stop))
        {
            unsigned t = atomic_load_explicit(&c->qtail, memory_order_relaxed);
            if (t == atomic_load_explicit(&c->qhead, memory_order_acquire))
                break;
            uint16_t offset = c->queue[t];
            atomic_store_explicit(&c->qtail, (t + 1) % QUEUE_SIZE, memory_order_release);
            fujimail_read_hotspot(offset);
        }
        publish_link(c);
        pthread_mutex_unlock(&c->mail_lock);
    }
    return NULL;
}

/* Hand one decoded hotspot write to the mailbox service: the core1 -> core0
 * ring, or inline in sync mode. Nothing is dropped unless the ring is full,
 * as on the hardware. */
static void note(sms_cart_t *c, uint16_t offset)
{
    if (c->sync || !c->worker_running)
    {
        fujimail_read_hotspot(offset);
        return;
    }
    unsigned h = atomic_load_explicit(&c->qhead, memory_order_relaxed);
    unsigned next = (h + 1) % QUEUE_SIZE;
    if (next == atomic_load_explicit(&c->qtail, memory_order_acquire))
        return;
    c->queue[h] = offset;
    atomic_store_explicit(&c->qhead, next, memory_order_release);
    pthread_mutex_lock(&c->queue_lock);
    pthread_cond_signal(&c->queue_cond);
    pthread_mutex_unlock(&c->queue_lock);
}

/* Wait until the worker has nothing queued and is not inside fujimail: the
 * bus layer must see the effect of events the console has already written
 * (BOOTLOCK before HOT_SWAP), which on the cart takes core0 microseconds. */
static bool sync_worker(sms_cart_t *c, unsigned timeout_ms)
{
    if (c->sync || !c->worker_running)
        return true;

    const uint64_t deadline = now_ms() + timeout_ms;
    for (;;)
    {
        if (queue_depth(c) == 0 && pthread_mutex_trylock(&c->mail_lock) == 0)
        {
            bool empty = queue_depth(c) == 0;
            pthread_mutex_unlock(&c->mail_lock);
            if (empty)
                return true;
        }
        if (now_ms() >= deadline)
        {
            fprintf(stderr, "fujinet: mailbox worker did not go idle\n");
            return false;
        }
        sched_yield();
    }
}

static void worker_start(sms_cart_t *c)
{
    atomic_store(&c->stop, false);
    atomic_store(&c->qhead, 0u);
    atomic_store(&c->qtail, 0u);
    c->worker_running = false;
    if (c->sync)
        return;
    if (pthread_create(&c->worker, NULL, worker_main, c) == 0)
        c->worker_running = true;
    else
        fprintf(stderr, "fujinet: no mailbox worker; servicing inline\n");
}

static void worker_stop(sms_cart_t *c)
{
    if (!c->worker_running)
        return;
    atomic_store(&c->stop, true);
    fujitcp_abort();
    pthread_mutex_lock(&c->queue_lock);
    pthread_cond_broadcast(&c->queue_cond);
    pthread_mutex_unlock(&c->queue_lock);
    pthread_join(c->worker, NULL);
    c->worker_running = false;
}

/*-------------------------------------------------
    the modes, as core1 switches them
-------------------------------------------------*/

static sms_glue_t glue(const sms_cart_t *c)
{
    sms_glue_t g;
    g.pwr_ok = true;
    g.game = c->game;
    g.mbox = c->mbox;
    g.ram_we = c->ram_we;
    g.load = c->load;
    return g;
}

static void to_resident(sms_cart_t *c)
{
    c->live = &c->res_map;
    c->bus.ptab = c->ptab_res;
    c->bus.mode = FN_MODE_RESIDENT;
    c->game = c->mbox = c->ram_we = false;
}

static void flip(sms_cart_t *c)
{
    uint8_t mode = c->loader.next_mode;

    c->live = c->loader.next;
    c->bus.mode = mode;
    c->bus.ptab = mode == FN_MODE_RESIDENT ? c->ptab_res
                : mode == FN_MODE_APP ? c->ptab_app : c->ptab_game;
    c->game = mode != FN_MODE_RESIDENT;
    c->mbox = mode == FN_MODE_APP;
    c->ram_we = c->game && c->live->ram_we;
    if (c->debug)
        fprintf(stderr, "fujinet: flip to mode %u (%s)\n", mode,
                mode == FN_MODE_RESIDENT ? "CONFIG" : smsmap_kind_name(c->live->plan.kind));
}

static void mapper_write(sms_cart_t *c, uint16_t a, uint8_t d)
{
    if (smsmap_write(c->live, a, d))
        c->ram_we = c->live->ram_we;
}

static void direct_boot(sms_cart_t *c, const uint8_t *img, const smsmap_plan_t *plan)
{
    memset(c->sram, 0, SMSMAP_SRAM_SIZE);
    memcpy(c->sram, img + plan->offset, plan->size);
    if (plan->kind == SMSMAP_JANGGUN)
        for (uint32_t i = 0; i < plan->padded; i++)
            c->sram[SMSMAP_REV_BANK * 0x2000 + i] = bitswap8(c->sram[i]);
    smsmap_init(&c->game_map, plan);
    c->direct_plan = *plan;
    c->loader.next = &c->game_map;
    c->loader.next_mode = plan->claim ? FN_MODE_APP : FN_MODE_GAME;
    flip(c);
    c->direct = true;
}

/*-------------------------------------------------
    the bus
-------------------------------------------------*/

uint8_t sms_cart_read(sms_cart_t *c, uint16_t a, bool m1, bool commit)
{
    if (commit && a == 0 && m1 && sms_fetch(&c->bus, 0, true))
        flip(c);

    sms_glue_t g = glue(c);
    if (sms_glue_oe(g, a, true, true))
        return c->sram[smsmap_sram_offset(c->live, a)];

    if (sms_in_arena(a) && atomic_load_explicit(&c->publish_ready, memory_order_acquire))
        drain_published(c);

    const uint8_t *p = sms_serve_ptr(&c->bus, a);
    uint8_t d = p ? *p : 0xff;
    if (commit && sms_glue_we(g, a, true, false, true))
        c->sram[smsmap_sram_offset(c->live, a)] = d;
    return d;
}

/* One hotspot write, decoded as fujinet.c does on the cart. */
static void mailbox_event(sms_cart_t *c, uint16_t offset, uint8_t data)
{
    unsigned page = offset & FN_H_PAGE_MASK;
    unsigned low = offset & 0xFF;

    if (page == FN_H_REGSEL || page == FN_H_REGDATA)
    {
        if (low == FN_REG_SLICE_ACK)
        {
            pthread_mutex_lock(&c->load_lock);
            fuji_load_ack(&c->loader);
            pthread_mutex_unlock(&c->load_lock);
            return;
        }
        if (low >= 0x80)
            return;
        note(c, (uint16_t)(FN_H_REGSEL + low));
        note(c, (uint16_t)(FN_H_REGDATA + data));
    }
    else if (page == FN_H_DATA)
    {
        note(c, (uint16_t)(FN_H_DATA + data));
    }
    if (c->sync || !c->worker_running)
        publish_link(c);
}

void sms_cart_write(sms_cart_t *c, uint16_t a, uint8_t d)
{
    if (sms_glue_we(glue(c), a, false, true, true))
        c->sram[smsmap_sram_offset(c->live, a)] = d;

    switch (sms_write_kind(&c->bus, a, true))
    {
    case SMS_W_MAPPER:
        mapper_write(c, a, d);
        break;
    case SMS_W_MAILBOX:
        mailbox_event(c, (uint16_t)(a & (FN_ARENA_SIZE - 1)), d);
        break;
    case SMS_W_SWAP:
        to_resident(c);
        /* BOOTLOCK -> arm_swap must have landed (worker) */
        sync_worker(c, SWAP_SYNC_MS);
        pthread_mutex_lock(&c->load_lock);
        fuji_load_event(&c->loader, SMS_W_SWAP);
        pthread_mutex_unlock(&c->load_lock);
        break;
    case SMS_W_CONFIG:
        to_resident(c);
        pthread_mutex_lock(&c->load_lock);
        fuji_load_event(&c->loader, SMS_W_CONFIG);
        pthread_mutex_unlock(&c->load_lock);
        break;
    case SMS_W_GO:
        c->bus.go_armed = true;
        pthread_mutex_lock(&c->load_lock);
        fuji_load_event(&c->loader, SMS_W_GO);
        pthread_mutex_unlock(&c->load_lock);
        break;
    default:
        break;
    }
}

void sms_cart_write_mapper(sms_cart_t *c, uint16_t a, uint8_t d)
{
    if (sms_write_kind(&c->bus, a, true) == SMS_W_MAPPER)
        mapper_write(c, a, d);
}

void sms_cart_snoop_c000(sms_cart_t *c, uint8_t d)
{
    if (c->bus.bios_phase)
        c->bus.c000 = d;
}

void sms_cart_snoop_iowrite(sms_cart_t *c, uint8_t port, uint8_t d)
{
    sms_io_write(&c->bus, port, d);
}

void sms_cart_snoop_ioread(sms_cart_t *c, uint8_t port)
{
    sms_io_read(&c->bus, port);
}

void sms_cart_frame(sms_cart_t *c)
{
    if (atomic_load_explicit(&c->publish_ready, memory_order_acquire))
        drain_published(c);
}

/*-------------------------------------------------
    push streams: fuji_store's two tiers
-------------------------------------------------*/

static uint8_t c_stream_open(int stream, uint32_t size)
{
    sms_cart_t *c = s_cart;
    uint8_t err;

    if (stream != FN_STREAM_ROM)
    {
        c->cfg_len = 0;
        return 0;
    }
    err = smsmap_gate(size);
    if (err)
        return err;

    pthread_mutex_lock(&c->load_lock);
    fuji_load_unstage(&c->loader);
    c->open_tier = -1;
    if (size <= STORE_RAM_MAX && !fuji_load_busy(&c->loader, c->store[0]))
        c->open_tier = 0;
    else if (size <= STORE_MAX && !fuji_load_busy(&c->loader, c->store[1]))
        c->open_tier = 1;
    pthread_mutex_unlock(&c->load_lock);

    if (c->open_tier < 0)
        return size <= STORE_MAX ? FN_BOOT_ERR_STOREBUSY : FN_BOOT_ERR_TOOBIG;
    c->store_len[c->open_tier] = 0;
    return 0;
}

static void c_stream_write(int stream, const uint8_t *chunk, unsigned len)
{
    sms_cart_t *c = s_cart;

    if (stream != FN_STREAM_ROM)
    {
        if (c->cfg_len + len <= 4096)
        {
            memcpy(c->cfg + c->cfg_len, chunk, len);
            c->cfg_len += len;
        }
        return;
    }
    if (c->open_tier >= 0 && c->store_len[c->open_tier] + len <= STORE_MAX)
    {
        memcpy(c->store[c->open_tier] + c->store_len[c->open_tier], chunk, len);
        c->store_len[c->open_tier] += len;
    }
}

static uint8_t c_stream_close(int stream, uint32_t got, bool aborted)
{
    sms_cart_t *c = s_cart;
    smsmap_plan_t plan;
    int tier = c->open_tier;
    int err;

    if (stream != FN_STREAM_ROM)
    {
        c->cfg_mapper[0] = '\0';
        if (!aborted)
        {
            const char *text = (const char *)c->cfg;
            for (uint32_t i = 0; i + 7 <= c->cfg_len; i++)
            {
                if (memcmp(text + i, "mapper=", 7) == 0)
                {
                    uint32_t j = i + 7, n = 0;
                    while (j < c->cfg_len && text[j] != ' ' && text[j] != '\r' && text[j] != '\n'
                           && n + 1 < sizeof c->cfg_mapper)
                        c->cfg_mapper[n++] = text[j++];
                    c->cfg_mapper[n] = '\0';
                    break;
                }
            }
        }
        return 0;
    }

    c->open_tier = -1;
    if (aborted || tier < 0 || c->store_len[tier] == 0)
    {
        c->cfg_mapper[0] = '\0';
        return 0;
    }
    err = smsmap_plan(c->store[tier], got, c->cfg_mapper[0] ? c->cfg_mapper : NULL, &plan);
    c->cfg_mapper[0] = '\0';
    if (err == SMSMAP_ETOOBIG)
        return FN_BOOT_ERR_TOOBIG;
    if (err != SMSMAP_OK)
    {
        fprintf(stderr, "fujinet: pushed image (%u bytes) is not mappable (%d)\n", got, err);
        return FN_BOOT_ERR_NOMAP;
    }
    if (c->debug)
        fprintf(stderr, "fujinet: staged kind=%s crc=%08X size=%u ram=%u claim=%d\n",
                smsmap_kind_name(plan.kind), plan.crc, plan.size, plan.ram_size, plan.claim);
    pthread_mutex_lock(&c->load_lock);
    fuji_load_stage(&c->loader, c->store[tier], &plan);
    pthread_mutex_unlock(&c->load_lock);
    return 0;
}

static void c_arm_swap(void)
{
    sms_cart_t *c = s_cart;

    pthread_mutex_lock(&c->load_lock);
    fuji_load_arm(&c->loader);
    pthread_mutex_unlock(&c->load_lock);
}

/*-------------------------------------------------
    lifecycle
-------------------------------------------------*/

sms_cart_t *sms_cart_create(void)
{
    sms_cart_t *c;

    if (s_cart)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->sram = calloc(1, SMSMAP_SRAM_SIZE);
    c->store[0] = malloc(STORE_MAX);
    c->store[1] = malloc(STORE_MAX);
    c->cfg = malloc(4096);
    if (!c->sram || !c->store[0] || !c->store[1] || !c->cfg)
    {
        free(c->sram);
        free(c->store[0]);
        free(c->store[1]);
        free(c->cfg);
        free(c);
        return NULL;
    }
    pthread_mutex_init(&c->queue_lock, NULL);
    pthread_cond_init(&c->queue_cond, NULL);
    pthread_mutex_init(&c->mail_lock, NULL);
    pthread_mutex_init(&c->load_lock, NULL);
    pthread_mutex_init(&c->publish_lock, NULL);
    c->open_tier = -1;
    s_cart = c;
    return c;
}

void sms_cart_destroy(sms_cart_t *c)
{
    if (!c)
        return;
    sms_cart_power_off(c);
    pthread_mutex_destroy(&c->queue_lock);
    pthread_cond_destroy(&c->queue_cond);
    pthread_mutex_destroy(&c->mail_lock);
    pthread_mutex_destroy(&c->load_lock);
    pthread_mutex_destroy(&c->publish_lock);
    free(c->sram);
    free(c->store[0]);
    free(c->store[1]);
    free(c->cfg);
    free(c->file);
    if (s_cart == c)
        s_cart = NULL;
    free(c);
}

static const char *plan_error(int err)
{
    switch (err)
    {
    case SMSMAP_ETOOBIG:      return "the image is larger than the cartridge's 1 MB";
    case SMSMAP_EUNSUPPORTED: return "the cartridge has no mapper for this image (a Korean multicart?)";
    case SMSMAP_EEMPTY:       return "the image is empty";
    default:                  return "the cartridge cannot map this image";
    }
}

int sms_cart_check_image(const uint8_t *image, uint32_t len, const char *cfg_mapper,
                         char *why, int whysz)
{
    smsmap_plan_t plan;
    int err = smsmap_plan(image, len, cfg_mapper, &plan);

    if (err != SMSMAP_OK)
    {
        if (why && whysz > 0)
            snprintf(why, (size_t)whysz, "%s", plan_error(err));
        return 0;
    }
    if (why && whysz > 0)
        why[0] = '\0';
    return 1;
}

int sms_cart_power_on(sms_cart_t *c, const uint8_t *image, uint32_t len,
                      const char *cfg_mapper, const char *boip, bool sync,
                      char *why, int whysz)
{
    smsmap_plan_t plan;
    const char *env;

    if (c->powered)
        sms_cart_power_off(c);
    if (why && whysz > 0)
        why[0] = '\0';

    /* device_start */
    c->debug = getenv("FUJINET_DEBUG") != NULL;
    env = getenv("FUJINET_SYNC");
    c->sync = sync || (env && *env && *env != '0');
    c->have_boip = boip != NULL;
    if (boip)
        snprintf(c->boip, sizeof c->boip, "%s", boip);

    memset(c->sram, 0x00, SMSMAP_SRAM_SIZE);
    c->store_len[0] = c->store_len[1] = 0;
    c->cfg_len = 0;
    c->cfg_mapper[0] = '\0';
    c->open_tier = -1;
    memset(c->arena, 0, sizeof c->arena);
    memset(c->window, 0, sizeof c->window);
    memcpy(c->arena + FN_LOADER, _loaderrom, FUJI_LOADERROM_SIZE);
    memset(c->published, 0, sizeof c->published);
    c->pub_lo = FN_R_PAINT_END;
    c->pub_hi = 0;
    atomic_store(&c->publish_ready, false);
    atomic_store(&c->busy, false);
    memset(c->ptab_res, 0, sizeof c->ptab_res);
    memset(c->ptab_app, 0, sizeof c->ptab_app);
    memset(c->ptab_game, 0, sizeof c->ptab_game);
    memset(&c->bus, 0, sizeof c->bus);
    memset(&c->res_map, 0, sizeof c->res_map);
    memset(&c->game_map, 0, sizeof c->game_map);
    c->game = c->mbox = c->ram_we = c->load = false;
    c->direct = false;

    c->load_port.poke = c_poke;
    c->load_port.set_load = c_set_load;
    memset(&c->loader, 0, sizeof c->loader);
    c->loader.port = &c->load_port;
    c->loader.window = c->window;
    c->loader.ptab_resident = c->ptab_res;
    c->loader.resident_map = &c->res_map;
    c->loader.game_map = &c->game_map;
    c->loader.bus = &c->bus;
    fuji_load_init(&c->loader);
    c->live = &c->res_map;

    /* the image, as the slot hands it over */
    c->resident_client = false;
    free(c->file);
    c->file = NULL;
    c->file_len = 0;
    if (image && len)
    {
        int err = smsmap_plan(image, len, cfg_mapper, &plan);
        if (err != SMSMAP_OK)
        {
            if (why && whysz > 0)
                snprintf(why, (size_t)whysz, "%s", plan_error(err));
            return -1;
        }
        c->file = malloc(len);
        if (!c->file)
        {
            if (why && whysz > 0)
                snprintf(why, (size_t)whysz, "out of memory");
            return -1;
        }
        memcpy(c->file, image, len);
        c->file_len = len;
    }

    /* the first device_reset */
    t_on_worker = false;
    fujimail_init(c->debug ? &debug_port : &quiet_port);

    for (int p = 0; p < 4; p++)
    {
        c->ptab_res[(FN_ARENA_BASE >> 10) + p] = c->arena + p * 0x400;
        c->ptab_app[(FN_ARENA_BASE >> 10) + p] = c->arena + p * 0x400;
    }

    memset(c->resident, 0xff, sizeof c->resident);
    memcpy(c->resident, _configrom, FUJI_CONFIGROM_SIZE);
    sms_bus_reset(&c->bus, c->ptab_res);
    c->powered = true;

    if (c->file)
    {
        if (plan.claim && plan.size <= FN_RESIDENT_MAX && plan.offset == 0)
        {
            memset(c->resident, 0xff, sizeof c->resident);
            memcpy(c->resident, c->file, c->file_len);
            c->resident_plan = plan;
            c->resident_client = true;
            if (c->debug)
                fprintf(stderr, "fujinet: %u-byte client, resident\n", c->file_len);
        }
        else
        {
            if (c->debug)
                fprintf(stderr, "fujinet: plan kind=%s crc=%08X size=%u ram=%u claim=%d (direct)\n",
                        smsmap_kind_name(plan.kind), plan.crc, plan.size, plan.ram_size, plan.claim);
            for (int p = 0; p < FN_RESIDENT_MAX / 0x400; p++)
                c->ptab_res[p] = c->resident + p * 0x400;
            /* A game alone needs no FujiNet: no socket */
            if (plan.claim)
                fujitcp_init(c->have_boip ? c->boip : NULL);
            fujimail_paint();
            publish_link(c);
            direct_boot(c, c->file, &plan);
            worker_start(c);
            return 0;
        }
    }

    fujitcp_init(c->have_boip ? c->boip : NULL);
    fujimail_paint();
    publish_link(c);
    for (int p = 0; p < FN_RESIDENT_MAX / 0x400; p++)
        c->ptab_res[p] = c->resident + p * 0x400;
    to_resident(c);
    worker_start(c);
    return 0;
}

void sms_cart_power_off(sms_cart_t *c)
{
    if (!c || !c->powered)
        return;
    worker_stop(c);
    fujitcp_close();
    c->powered = false;
}

void sms_cart_console_reset(sms_cart_t *c)
{
    if (!c->powered)
        return;

    /* A soft reset is the console's /RESET: back to CONFIG, the BIOS runs
     * again and is snooped again. The mailbox's state survives. */
    if (c->direct)
    {
        smsmap_plan_t plan = c->direct_plan;   /* init clears the struct it reads */
        sms_bus_reset(&c->bus, c->ptab_res);
        smsmap_init(&c->game_map, &plan);
        c->loader.next = &c->game_map;
        flip(c);
        return;
    }
    pthread_mutex_lock(&c->load_lock);
    fuji_load_abort(&c->loader);
    pthread_mutex_unlock(&c->load_lock);
    to_resident(c);
    sms_bus_reset(&c->bus, c->ptab_res);
}

/*-------------------------------------------------
    status and debugger access
-------------------------------------------------*/

static uint32_t count24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

void sms_cart_status(sms_cart_t *c, sms_cart_status_t *out)
{
    memset(out, 0, sizeof *out);
    if (!c)
        return;
    out->powered = c->powered;
    if (!c->powered)
        return;
    sms_cart_frame(c);
    out->link_up = fujitcp_active();
    out->busy = atomic_load(&c->busy);
    out->direct = c->direct;
    out->mode = c->bus.mode;
    out->resident_client = c->resident_client;
    out->booted_game = c->direct || c->resident_client || c->bus.mode != FN_MODE_RESIDENT;
    if (c->bus.mode != FN_MODE_RESIDENT && c->live)
    {
        out->mapper = c->live->plan.kind;
        out->mapper_name = smsmap_kind_name(c->live->plan.kind);
        memcpy(out->bank, c->live->bank, sizeof out->bank);
        out->ram_en = c->live->ram_en;
        out->ram_we = c->live->ram_we;
        out->image_size = c->live->plan.size;
        out->image_crc = c->live->plan.crc;
        out->ram_size = c->live->plan.ram_size;
        out->claim = c->live->plan.claim;
    }
    else if (c->resident_client)
    {
        /* an opened FujiNet client of 32K or less, served in CONFIG's
         * place as the cartridge serves its resident image */
        out->mapper = -1;
        out->mapper_name = "resident";
        out->image_size = c->resident_plan.size;
        out->image_crc = c->resident_plan.crc;
        out->claim = true;
    }
    else
    {
        out->mapper = -1;
        out->mapper_name = "CONFIG";
    }
    out->load_state = c->arena[FN_R_LOAD_STATE];
    out->load_win = (int)c->loader.win;
    out->load_nwin = (int)c->loader.nwin;
    out->load_pct = c->arena[FN_R_LOAD_PCT];
    out->boot_state = c->arena[FN_R_BOOT_STATE];
    out->boot_pct = c->arena[FN_R_BOOT_PCT];
    out->boot_err = c->arena[FN_R_BOOT_ERR];
    out->boot_got = count24(c->arena + FN_R_BOOT_GOT0);
    out->boot_total = count24(c->arena + FN_R_BOOT_TOT0);
    out->ackseq = c->arena[FN_R_ACKSEQ];
    out->status = c->arena[FN_R_STATUS];
    out->err = c->arena[FN_R_ERR];
    out->reply_cmd = c->arena[FN_R_REPLY_CMD];
    out->rxlen = (uint16_t)(c->arena[FN_R_RXLEN_LO] | (c->arena[FN_R_RXLEN_HI] << 8));
    out->bios_phase = c->bus.bios_phase;
    out->snoop_c000 = c->bus.c000;
    out->snoop_3e = c->bus.p3e;
    out->snoop_3f = c->bus.p3f;
    memcpy(out->snoop_vdp, c->bus.vdp, sizeof out->snoop_vdp);
    out->queue = queue_depth(c);
}

void sms_cart_page_banks(sms_cart_t *c, int16_t out[48])
{
    for (int p = 0; p < 48; p++)
    {
        const uint16_t a = (uint16_t)(p << 10);
        if (!c || !c->powered)
            out[p] = -1;
        else if (sms_glue_oe(glue(c), a, true, true))
            out[p] = c->live->lut[p];
        else
            out[p] = -1;
    }
}

int sms_cart_peek(sms_cart_t *c, uint16_t a, uint8_t *out)
{
    if (!c || !c->powered || a >= 0xC000)
        return 0;
    *out = sms_cart_read(c, a, false, false);
    return 1;
}

int sms_cart_poke(sms_cart_t *c, uint16_t a, uint8_t d)
{
    if (!c || !c->powered || a >= 0xC000)
        return 0;
    if (sms_glue_oe(glue(c), a, true, true))
    {
        c->sram[smsmap_sram_offset(c->live, a)] = d;
        return 1;
    }
    const uint8_t *p = sms_serve_ptr(&c->bus, a);
    if (p)
    {
        /* the served memory is all this device's own buffers */
        *(uint8_t *)(uintptr_t)p = d;
        if (p >= c->arena && p < c->arena + FN_R_PAINT_END)
            c->published[p - c->arena] = d;
    }
    return 1;
}

uint32_t sms_cart_copy_sram(sms_cart_t *c, uint8_t *dst, uint32_t max)
{
    uint32_t n = max < SMSMAP_SRAM_SIZE ? max : SMSMAP_SRAM_SIZE;
    if (!c)
        return 0;
    memcpy(dst, c->sram, n);
    return n;
}

uint32_t sms_cart_copy_arena(sms_cart_t *c, uint8_t *dst, uint32_t max)
{
    uint32_t n = max < FN_ARENA_SIZE ? max : FN_ARENA_SIZE;
    if (!c)
        return 0;
    sms_cart_frame(c);
    memcpy(dst, c->arena, n);
    return n;
}
