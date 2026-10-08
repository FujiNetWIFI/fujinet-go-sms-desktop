/*
 * session.c -- the frontend contract (core/include/smssession.h) over the
 * Master System host (core/sms/host.h).
 *
 * The C modules beside it (settings, paths, media, roms, audio, gamepads,
 * bindings) share the session struct through session_internal.h; this file
 * owns the lifecycle, the power cycles, the cartridges, the input routing
 * and the cartridge status. Ported from the NES sibling's session.cpp, minus
 * MesenCE: the host here is this repository's own C core.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"
#include "bindings.h"
#include "smsdebug.h"
#include "host.h"
#include "fuji_mailbox.h"

/* The public console order is the core's model order. */
_Static_assert((int)SMS_CONSOLE_SMS1 == (int)SMS_MODEL_SMS1 &&
               (int)SMS_CONSOLE_SMS1_PAL == (int)SMS_MODEL_SMS1_PAL &&
               (int)SMS_CONSOLE_SMS2 == (int)SMS_MODEL_SMS2 &&
               (int)SMS_CONSOLE_SMS2_PAL == (int)SMS_MODEL_SMS2_PAL &&
               (int)SMS_CONSOLE_SMSJ == (int)SMS_MODEL_SMSJ &&
               (int)SMS_CONSOLE_MARK3 == (int)SMS_MODEL_MARK3 &&
               (int)SMS_CONSOLE_COUNT == (int)SMS_MODEL_COUNT,
               "sms_console and sms_model_t must agree");

#define CART_MAX (0x100000u + 512u)   /* the cartridge's 1 MB, plus a copier header */

/* ---- helpers shared with the C modules ----------------------------------- */

void session_set_error(struct smssession *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
    va_end(ap);
}

void session_gamepad_event(struct smssession *s, const char *text)
{
    pthread_mutex_lock(&s->pad_event_mtx);
    snprintf(s->pad_event, sizeof s->pad_event, "%s", text ? text : "");
    pthread_mutex_unlock(&s->pad_event_mtx);
}

int smssession_gamepad_last_event(smssession *s, char *dst, int dstsz)
{
    int n;
    if (!dst || dstsz <= 0)
        return 0;
    pthread_mutex_lock(&s->pad_event_mtx);
    n = snprintf(dst, (size_t)dstsz, "%s", s->pad_event);
    pthread_mutex_unlock(&s->pad_event_mtx);
    return n;
}

#ifdef SMS_GAMEPAD_STUB
int gamepad_start(struct smssession *s) { session_set_error(s, "gamepad support not built"); return -1; }
void gamepad_stop(struct smssession *s) { (void)s; }
int smssession_gamepad_count(smssession *s) { (void)s; return 0; }
int smssession_gamepad_name(smssession *s, int i, char *d, int n) { (void)s; (void)i; if (d && n > 0) d[0] = 0; return 0; }
void smssession_gamepad_assign(smssession *s, int i, int p) { (void)s; (void)i; (void)p; }
int smssession_gamepad_assignment(smssession *s, int i) { (void)s; (void)i; return -1; }
int smssession_gamepad_effective_port(smssession *s, int i) { (void)s; (void)i; return -1; }
unsigned smssession_gamepad_generation(smssession *s) { (void)s; return 0; }
void smssession_gamepad_capture_begin(smssession *s) { (void)s; }
void smssession_gamepad_capture_cancel(smssession *s) { (void)s; }
int smssession_gamepad_capture_poll(smssession *s, int *b) { (void)s; (void)b; return 0; }
#endif

/* ---- the consoles ---------------------------------------------------------- */

static int console_ok(int c)
{
    return c >= 0 && c < SMS_CONSOLE_COUNT;
}

const char *sms_console_name(int console)
{
    return console_ok(console) ? sms_models[console].name : NULL;
}

const char *sms_console_id(int console)
{
    return console_ok(console) ? sms_models[console].id : NULL;
}

int sms_console_is_pal(int console)
{
    return console_ok(console) && sms_models[console].is_pal;
}

int sms_console_has_bios_socket(int console)
{
    return console_ok(console) && sms_models[console].bios_region != 0;
}

int sms_console_has_reset_button(int console)
{
    return console_ok(console) && (sms_models[console].has_reset_button ||
                                   sms_models[console].has_rapid_button);
}

int sms_console_has_fm(int console)
{
    return console_ok(console) && (sms_models[console].is_smsj ||
                                   sms_models[console].is_mark_iii);
}

/* ---- lifecycle ------------------------------------------------------------- */

smssession *smssession_new(const smssession_paths *paths)
{
    struct smssession *s;

    paths_capture_launch_dir();
    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;

    pthread_mutex_init(&s->settings_mtx, NULL);
    pthread_mutex_init(&s->sysact_mtx, NULL);
    pthread_mutex_init(&s->pad_event_mtx, NULL);

    if (paths_init(s, paths ? paths->config_dir : NULL,
                   paths ? paths->data_dir : NULL) != 0) {
        smssession_free(s);
        return NULL;
    }
    settings_init(s);
    bindings_init(s);
    roms_provision_embedded(s);

    if (paths && paths->fujinet_lib) {
        snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s", paths->fujinet_lib);
        s->fujinet_disabled = !paths->fujinet_lib[0];
    }
    if (paths && paths->fujinet_runtime_src)
        snprintf(s->fujinet_runtime_src, sizeof s->fujinet_runtime_src, "%s",
                 paths->fujinet_runtime_src);
    return s;
}

void smssession_free(smssession *s)
{
    if (!s)
        return;
    smssession_stop(s);
    smssession_settings_flush(s);
    settings_free_all(s);
    smsdebug_destroy(s);
    pthread_mutex_destroy(&s->settings_mtx);
    pthread_mutex_destroy(&s->sysact_mtx);
    pthread_mutex_destroy(&s->pad_event_mtx);
    free(s);
}

void smssession_default_opts(smssession *s, smssession_start_opts *opts)
{
    memset(opts, 0, sizeof *opts);
    opts->console = smssession_get_int(s, "console", SMS_CONSOLE_SMS1);
    if (!console_ok(opts->console))
        opts->console = SMS_CONSOLE_SMS1;
    opts->bios = smssession_console_bios(s, opts->console);
    opts->fm_unit = smssession_get_int(s, "fm_unit", 1);
    opts->fm_unit_mutes_psg = smssession_get_int(s, "fm_unit_mutes_psg", 0);
    opts->analog_joystick = smssession_get_int(s, "analog_joystick", 1);
    opts->enable_fujinet = smssession_get_int(s, "enable_fujinet", 1);
    opts->enable_audio = smssession_get_int(s, "enable_audio", 1);
    opts->enable_gamepad = smssession_get_int(s, "enable_gamepad", 1);
    opts->cart_path = smssession_get_str(s, "cart", NULL);
    if (opts->cart_path && !opts->cart_path[0])
        opts->cart_path = NULL;
}

static uint8_t *read_cart(const char *path, uint32_t *len, char *why, int whysz)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    size_t got;

    if (!f) {
        snprintf(why, (size_t)whysz, "cannot open %s", path);
        return NULL;
    }
    buf = malloc(CART_MAX + 1);
    if (!buf) {
        fclose(f);
        snprintf(why, (size_t)whysz, "out of memory");
        return NULL;
    }
    got = fread(buf, 1, CART_MAX + 1, f);
    fclose(f);
    if (got == 0) {
        free(buf);
        snprintf(why, (size_t)whysz, "%s is empty", path);
        return NULL;
    }
    if (got > CART_MAX) {
        free(buf);
        snprintf(why, (size_t)whysz, "the image is larger than the cartridge's 1 MB");
        return NULL;
    }
    *len = (uint32_t)got;
    return buf;
}

/* A .cfg beside the image (game.sms -> game.cfg) with a mapper= line
 * overrides the cartridge's mapper choice, as the pushed sibling does on
 * the hardware. */
static int read_cfg_mapper(const char *path, char *out, int outsz)
{
    char cfg[SMS_PATH_MAX];
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    const char *dot = strrchr(slash ? slash : path, '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    char text[4096];
    size_t n;
    FILE *f;
    const char *m;

    out[0] = '\0';
    if (stem + 5 > sizeof cfg)
        return 0;
    memcpy(cfg, path, stem);
    memcpy(cfg + stem, ".cfg", 5);
    f = fopen(cfg, "rb");
    if (!f)
        return 0;
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    m = strstr(text, "mapper=");
    if (!m)
        return 0;
    m += 7;
    {
        int i = 0;
        while (m[i] && m[i] != ' ' && m[i] != '\r' && m[i] != '\n' && i + 1 < outsz) {
            out[i] = m[i];
            i++;
        }
        out[i] = '\0';
    }
    return out[0] != '\0';
}

/* Bring the machine up with s->opts and the given image (NULL: CONFIG). */
static int host_up(struct smssession *s, const char *cart_path)
{
    sms_host_opts_t ho;
    uint8_t *cart = NULL, *bios = NULL, *inst = NULL;
    uint32_t cart_len = 0, bios_len = 0;
    char mapper[64] = "";
    char boip[32];
    char why[256];
    int rc;

    memset(&ho, 0, sizeof ho);
    if (cart_path && *cart_path) {
        cart = read_cart(cart_path, &cart_len, why, sizeof why);
        if (!cart) {
            session_set_error(s, "%s", why);
            return -1;
        }
        read_cfg_mapper(cart_path, mapper, sizeof mapper);
    }
    if (s->opts.bios && s->opts.bios[0] && sms_console_has_bios_socket(s->opts.console)) {
        bios = roms_load_bios(s, s->opts.bios, &bios_len);
        if (!bios)
            fprintf(stderr, "sms: BIOS \"%s\" is not in %s; booting the cartridge "
                    "directly\n", s->opts.bios, s->roms_dir);
    }
    if (sms_console_has_fm(s->opts.console))
        inst = roms_load_instruments(s);

    snprintf(boip, sizeof boip, "127.0.0.1:%d", SMSSESSION_BOIP_PORT);
    ho.model = (sms_model_t)s->opts.console;
    ho.bios = bios;
    ho.bios_size = bios_len;
    ho.instruments = inst;
    ho.fm_unit = s->opts.fm_unit != 0;
    ho.fm_unit_mutes_psg = s->opts.fm_unit_mutes_psg != 0;
    ho.cart = cart;
    ho.cart_size = cart_len;
    ho.cart_mapper = mapper[0] ? mapper : NULL;
    ho.boip_hostport = boip;
    ho.cart_sync = getenv("SMS_CART_SYNC") != NULL;

    rc = sms_host_start(&ho);
    if (rc != 0)
        session_set_error(s, "%s", sms_host_last_error());
    free(cart);
    free(bios);
    free(inst);
    return rc;
}

/* A power cycle: the machine and the cartridge come up fresh; FujiNet stays
 * up. `cart_path` NULL boots CONFIG. */
static int power_cycle(struct smssession *s, const char *cart_path)
{
    int rc;

    /* the console powers up with the BIOS and FM options its settings name
     * now: an Import BIOS or a Preferences change takes effect here */
    snprintf(s->bios_name, sizeof s->bios_name, "%s",
             smssession_console_bios(s, s->opts.console));
    s->opts.bios = s->bios_name;
    s->opts.fm_unit = smssession_get_int(s, "fm_unit", 1);
    s->opts.fm_unit_mutes_psg = smssession_get_int(s, "fm_unit_mutes_psg", 0);

    smsdebug_before_power_cycle(s);
    sms_host_stop();
    rc = host_up(s, cart_path);
    if (rc != 0 && cart_path) {
        /* whatever was running is gone: put CONFIG back */
        char err[256];
        snprintf(err, sizeof err, "%s", s->last_error);
        host_up(s, NULL);
        snprintf(s->last_error, sizeof s->last_error, "%s", err);
    }
    smsdebug_after_power_cycle(s);
    return rc;
}

static void set_cart(struct smssession *s, const char *path)
{
    if (path && *path)
        snprintf(s->cart_path, sizeof s->cart_path, "%s", path);
    else
        s->cart_path[0] = '\0';
    s->opts.cart_path = s->cart_path[0] ? s->cart_path : NULL;
    smssession_set_str(s, "cart", s->cart_path);
}

static void adopt_opts(struct smssession *s, const smssession_start_opts *opts)
{
    s->opts = *opts;
    if (!console_ok(s->opts.console))
        s->opts.console = SMS_CONSOLE_SMS1;
    snprintf(s->bios_name, sizeof s->bios_name, "%s", opts->bios ? opts->bios : "");
    s->opts.bios = s->bios_name;
    if (opts->cart_path) {
        char abs[SMS_PATH_MAX];
        snprintf(s->cart_path, sizeof s->cart_path, "%s",
                 paths_resolve(opts->cart_path, abs, sizeof abs));
    } else
        s->cart_path[0] = '\0';
    s->opts.cart_path = s->cart_path[0] ? s->cart_path : NULL;
}

int smssession_start(smssession *s, const smssession_start_opts *opts)
{
    smssession_start_opts local;

    if (s->running)
        return 0;
    s->last_error[0] = '\0';

    if (!opts) {
        smssession_default_opts(s, &local);
        opts = &local;
    }
    adopt_opts(s, opts);
    memset(s->pad_src, 0, sizeof s->pad_src);
    memset(s->sw_src, 0, sizeof s->sw_src);
    sms_host_release_all();

    /* FujiNet FIRST: it listens and the cartridge dials in, so the listener
     * has to exist before the machine's first transaction or the CONFIG
     * client boots reporting no link. Failing to start is NOT fatal (the
     * cartridge also redials when a transaction finds the link down). */
    if (s->opts.enable_fujinet && !s->fujinet_disabled) {
        if (fujinet_start(s) == 0)
            fujinet_wait_for_boip(s, 3000);
    }

    /* A remembered cartridge that no longer loads (moved, or an image the
     * cartridge cannot map) must not leave the app unbootable: fall back to
     * CONFIG and say why. */
    if (host_up(s, s->opts.cart_path) != 0) {
        if (s->opts.cart_path) {
            fprintf(stderr, "sms: %s: %s; booting CONFIG\n", s->cart_path, s->last_error);
            set_cart(s, NULL);
            if (host_up(s, NULL) != 0) {
                fujinet_stop(s);
                return -1;
            }
        } else {
            fujinet_stop(s);
            return -1;
        }
    }

    s->running = 1;
    smsdebug_after_power_cycle(s);

    if (s->opts.enable_gamepad && gamepad_start(s) != 0) {
        fprintf(stderr, "sms: gamepads unavailable (%s)\n", s->last_error);
        s->last_error[0] = '\0';
    }
    if (s->opts.enable_audio && audio_start(s) != 0) {
        fprintf(stderr, "sms: audio unavailable (%s); continuing silent\n", s->last_error);
        s->last_error[0] = '\0';
    }
    return 0;
}

void smssession_stop(smssession *s)
{
    if (!s->running)
        return;
    audio_stop(s);
    gamepad_stop(s);
    smsdebug_before_power_cycle(s);
    sms_host_stop();
    fujinet_stop(s);
    s->running = 0;
}

int smssession_is_running(const smssession *s)
{
    return s->running;
}

const char *smssession_last_error(const smssession *s)
{
    return s->last_error;
}

int smssession_restart(smssession *s)
{
    smssession_start_opts opts;

    smssession_settings_flush(s);
    smssession_default_opts(s, &opts);
    if (!s->running)
        return smssession_start(s, &opts);

    /* FujiNet or the device toggles change what runs beside the machine:
     * a full restart. Anything else is a machine option: a power cycle. */
    if (!!opts.enable_fujinet != !!s->opts.enable_fujinet ||
        !!opts.enable_audio != !!s->opts.enable_audio ||
        !!opts.enable_gamepad != !!s->opts.enable_gamepad) {
        smssession_stop(s);
        return smssession_start(s, &opts);
    }
    adopt_opts(s, &opts);
    return power_cycle(s, s->opts.cart_path);
}

/* ---- cartridges ------------------------------------------------------------ */

int smssession_check_cart(const char *path, char *why, int whysz)
{
    uint8_t *img;
    uint32_t len = 0;
    char mapper[64];
    char err[256];
    char abs[SMS_PATH_MAX];
    int ok;

    if (why && whysz > 0)
        why[0] = '\0';
    if (!path || !*path)
        return 0;
    path = paths_resolve(path, abs, sizeof abs);
    img = read_cart(path, &len, err, sizeof err);
    if (!img) {
        if (why && whysz > 0)
            snprintf(why, (size_t)whysz, "%s", err);
        return 0;
    }
    read_cfg_mapper(path, mapper, sizeof mapper);
    ok = sms_cart_check_image(img, len, mapper[0] ? mapper : NULL, why, whysz);
    free(img);
    return ok;
}

int smssession_load_cart(smssession *s, const char *path)
{
    char why[256];
    char abs[SMS_PATH_MAX];

    if (!s->running || !path || !*path)
        return -1;
    path = paths_resolve(path, abs, sizeof abs);
    if (!smssession_check_cart(path, why, sizeof why)) {
        session_set_error(s, "%s", why);
        return -1;
    }
    if (power_cycle(s, path) != 0) {
        set_cart(s, NULL);
        return -1;
    }
    set_cart(s, path);
    return 0;
}

const char *smssession_cart_path(const smssession *s)
{
    return s->cart_path;
}

int smssession_soft_reset(smssession *s)
{
    if (!s->running)
        return -1;
    sms_host_soft_reset();
    return 0;
}

int smssession_reset_to_config(smssession *s)
{
    if (!s->running)
        return -1;
    set_cart(s, NULL);
    return power_cycle(s, NULL);
}

int smssession_eject(smssession *s)
{
    return smssession_reset_to_config(s);
}

int smssession_console(const smssession *s)
{
    if (s->running)
        return s->opts.console;
    return smssession_get_int((smssession *)s, "console", SMS_CONSOLE_SMS1);
}

/* ---- video / audio --------------------------------------------------------- */

int smssession_copy_frame(smssession *s, uint32_t *dst, int *height, uint64_t *serial_inout)
{
    if (!s->running)
        return 0;
    return sms_host_frame_copy(dst, height, serial_inout);
}

int smssession_refresh_rate(smssession *s)
{
    if (!s->running)
        return sms_console_is_pal(smssession_get_int(s, "console", SMS_CONSOLE_SMS1)) ? 50 : 60;
    return sms_host_frame_rate() < 55.0 ? 50 : 60;
}

void smssession_notify_vsync(smssession *s, int64_t frame_time_ns)
{
    if (s->running)
        sms_host_notify_vsync(frame_time_ns);
}

int smssession_render_audio(smssession *s, float *out, int nframes)
{
    int got = 0;

    if (nframes <= 0)
        return 0;
    if (s->running)
        got = sms_host_audio_copy(out, nframes);
    if (got < nframes)
        memset(out + 2 * got, 0, sizeof(float) * 2 * (size_t)(nframes - got));
    {
        const float gain = (float)smssession_get_int(s, "volume", 100) / 100.0f;
        if (gain < 0.999f)
            for (int i = 0; i < 2 * got; i++)
                out[i] *= gain;
    }
    return nframes;
}

void smssession_set_volume(smssession *s, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    smssession_set_int(s, "volume", percent);
}

/* ---- input ----------------------------------------------------------------- */

static void apply_pad(struct smssession *s, int port)
{
    uint8_t mask = 0;
    static const uint8_t bit[SMS_ACT_PER_PORT] = {
        SMS_PAD_UP, SMS_PAD_DOWN, SMS_PAD_LEFT, SMS_PAD_RIGHT, SMS_PAD_1, SMS_PAD_2,
    };
    for (int a = 0; a < SMS_ACT_PER_PORT; a++)
        if (s->pad_src[port][a])
            mask |= bit[a];
    sms_host_pad_set(port, mask);
}

static void pad_src(struct smssession *s, int port, int act, int down, uint8_t src)
{
    if (down)
        s->pad_src[port][act] |= src;
    else
        s->pad_src[port][act] &= (uint8_t)~src;
    apply_pad(s, port);
}

static void switch_src(struct smssession *s, int sw, int down, uint8_t src)
{
    if (down)
        s->sw_src[sw] |= src;
    else
        s->sw_src[sw] &= (uint8_t)~src;
    if (sw == SMS_SW_PAUSE)
        sms_host_pause_set(s->sw_src[sw] != 0);
    else if (sw == SMS_SW_RESET)
        sms_host_reset_button_set(s->sw_src[sw] != 0);
}

void smssession_press(smssession *s, int target, int down)
{
    if (!s->running || target < 0 || target >= SMS_TARGET_COUNT)
        return;

    if (target < 2 * SMS_ACT_PER_PORT) {
        pad_src(s, target / SMS_ACT_PER_PORT, target % SMS_ACT_PER_PORT, down, 1);
        return;
    }
    target -= 2 * SMS_ACT_PER_PORT;
    if (target < SMS_SW_COUNT) {
        switch_src(s, target, down, 1);
        return;
    }
    if (down)
        smssession_sysaction(s, target - SMS_SW_COUNT);
}

void smssession_sysaction(smssession *s, int sysact)
{
    switch (sysact) {
    case SMS_SYSACT_RESET_CONFIG:
        smssession_reset_to_config(s);
        break;
    case SMS_SYSACT_SOFT_RESET:
        smssession_soft_reset(s);
        break;
    case SMS_SYSACT_DEBUG_STOP:
        if (s->running) {
            smsdebug *d = smsdebug_get(s);
            smsdebug_attach(d);
            smsdebug_stop(d);
        }
        break;
    default:
        break;
    }
}

void smssession_sysaction_post(smssession *s, int sysact)
{
    if (sysact < 0 || sysact >= SMS_SYSACT_COUNT)
        return;
    pthread_mutex_lock(&s->sysact_mtx);
    s->sysact_pending |= 1u << sysact;
    pthread_mutex_unlock(&s->sysact_mtx);
}

int smssession_sysaction_take(smssession *s, int *out)
{
    int found = 0;
    pthread_mutex_lock(&s->sysact_mtx);
    for (int i = 0; i < SMS_SYSACT_COUNT; i++) {
        if (s->sysact_pending & (1u << i)) {
            s->sysact_pending &= ~(1u << i);
            if (out)
                *out = i;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s->sysact_mtx);
    return found;
}

unsigned smssession_buttons_held(smssession *s, int port)
{
    unsigned held = 0;
    if (port < 0 || port > 1)
        return 0;
    for (int a = 0; a < SMS_ACT_PER_PORT; a++)
        if (s->pad_src[port][a])
            held |= 1u << a;
    return held;
}

int smssession_switch_held(smssession *s, int sw)
{
    if (sw < 0 || sw >= SMS_SW_COUNT)
        return 0;
    return s->sw_src[sw] != 0;
}

/* The gamepad thread's entry point: the same routing as press, from the
 * pad's own source bit, never touching the settings or the debugger. */
void session_gamepad_apply(struct smssession *s, int port, int act, int down)
{
    if (!s->running || port < 0 || port > 1 || act < 0 || act >= SMS_ACT_PER_PORT)
        return;
    pad_src(s, port, act, down, 2);
}

void smssession_set_analog(smssession *s, int joystick)
{
    s->opts.analog_joystick = joystick ? 1 : 0;
    smssession_set_int(s, "analog_joystick", s->opts.analog_joystick);
}

/* ---- FujiNet --------------------------------------------------------------- */

int smssession_fujinet_running(const smssession *s)
{
    return s->fujinet_running;
}

const char *smssession_fujinet_webui_url(const smssession *s)
{
    return s->webui_url;
}

int smssession_cart_link_up(smssession *s)
{
    sms_cart_status_t st;
    if (!s->running)
        return -1;
    sms_host_cart_status(&st);
    return st.link_up ? 1 : 0;
}

int smssession_cart_status(smssession *s, char *dst, int dstsz)
{
    sms_cart_status_t st;

    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!s->running)
        return snprintf(dst, (size_t)dstsz, "stopped");
    sms_host_cart_status(&st);
    if (!st.powered)
        return snprintf(dst, (size_t)dstsz, "starting");
    if (st.load_state == FN_LOAD_WINDOW)
        return snprintf(dst, (size_t)dstsz, "loading %d%%", st.load_pct);
    if (st.boot_state == FN_BOOT_XFER)
        return snprintf(dst, (size_t)dstsz, "receiving %d%%", st.boot_pct);
    if (st.booted_game && st.mode == FN_MODE_GAME)
        return snprintf(dst, (size_t)dstsz, "game running; mailbox closed");
    if (st.link_up)
        return snprintf(dst, (size_t)dstsz, st.busy ? "connected (busy)" : "connected");
    return snprintf(dst, (size_t)dstsz, "link down");
}

int smssession_cart_booted_game(smssession *s)
{
    sms_cart_status_t st;
    if (!s->running)
        return 0;
    sms_host_cart_status(&st);
    return st.booted_game ? 1 : 0;
}

/* ---- paths ----------------------------------------------------------------- */

const char *smssession_config_path(const smssession *s) { return s->config_dir; }
const char *smssession_data_path(const smssession *s) { return s->data_dir; }
const char *smssession_carts_path(const smssession *s) { return s->carts_dir; }
const char *smssession_sd_path(const smssession *s) { return s->fujinet_sd; }

/* ---- debugger -------------------------------------------------------------- */

smsdebug *smssession_debugger(smssession *s)
{
    return smsdebug_get(s);
}
