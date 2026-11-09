/*
 * input -- every way a joypad contact or a console button gets held, and the
 * one result the machine sees.
 *
 * The inputs are held by sources (session_internal.h SRC_*: the keyboard and
 * the controllers window's console buttons, the window's pointer, and for each
 * gamepad its buttons and its stick), one bit per source in an array per
 * contact, so letting go of one source never releases another's. After every
 * change the result is handed to the host (sms_host_pad_set,
 * sms_host_pause_set, sms_host_reset_button_set), which latches it as the
 * console's input ports do.
 *
 * A joypad's contacts are two opposed pairs (UP/DOWN, LEFT/RIGHT): a thumb
 * that holds both of a pair holds neither. What the console sees is what
 * smssession_buttons_held reports.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "host.h"
#include "session_internal.h"
#include "smsdebug.h"

void input_init(struct smssession *ses)
{
    pthread_mutex_init(&ses->input_mtx, NULL);
}

/* ---- what the console sees ---------------------------------------------------- */

static const unsigned k_pairs[2][2] = {
    { 1u << SMS_ACT_UP, 1u << SMS_ACT_DOWN },
    { 1u << SMS_ACT_LEFT, 1u << SMS_ACT_RIGHT },
};

/* input_mtx held. */
static unsigned held_locked(const struct smssession *ses, int port)
{
    unsigned mask = 0;

    for (int act = 0; act < SMS_ACT_PER_PORT; act++)
        if (ses->pad_src[port][act])
            mask |= 1u << act;
    for (int pair = 0; pair < 2; pair++)
        if ((mask & k_pairs[pair][0]) && (mask & k_pairs[pair][1]))
            mask &= ~(k_pairs[pair][0] | k_pairs[pair][1]);
    return mask;
}

static void push_controller_locked(struct smssession *ses, int port)
{
    static const uint8_t bit[SMS_ACT_PER_PORT] = {
        SMS_PAD_UP, SMS_PAD_DOWN, SMS_PAD_LEFT, SMS_PAD_RIGHT, SMS_PAD_1, SMS_PAD_2,
    };
    const unsigned held = held_locked(ses, port);
    uint8_t mask = 0;

    for (int act = 0; act < SMS_ACT_PER_PORT; act++)
        if (held & (1u << act))
            mask |= bit[act];
    sms_host_pad_set(port, mask);
}

static void push_switch_locked(struct smssession *ses, int swnum)
{
    if (swnum == SMS_SW_PAUSE)
        sms_host_pause_set(ses->sw_src[swnum] != 0);
    else if (swnum == SMS_SW_RESET)
        sms_host_reset_button_set(ses->sw_src[swnum] != 0);
}

/* A start: nothing is held, and the machine is told. */
void input_clear(struct smssession *ses)
{
    pthread_mutex_lock(&ses->input_mtx);
    memset(ses->pad_src, 0, sizeof ses->pad_src);
    memset(ses->sw_src, 0, sizeof ses->sw_src);
    sms_host_release_all();
    pthread_mutex_unlock(&ses->input_mtx);
}

static void set_src(uint32_t *word, uint32_t src, int down)
{
    if (down)
        *word |= src;
    else
        *word &= ~src;
}

/* One source of one target; system actions are not held inputs and are left
 * to the caller. */
static void source_press(struct smssession *ses, int target, uint32_t src, int down)
{
    pthread_mutex_lock(&ses->input_mtx);
    if (target < SMS_TARGET_SWITCH(0)) {
        const int port = target / SMS_ACT_PER_PORT;
        set_src(&ses->pad_src[port][target % SMS_ACT_PER_PORT], src, down);
        push_controller_locked(ses, port);
    } else if (target < SMS_TARGET_SYSACT(0)) {
        const int swnum = target - SMS_TARGET_SWITCH(0);
        set_src(&ses->sw_src[swnum], src, down);
        push_switch_locked(ses, swnum);
    }
    pthread_mutex_unlock(&ses->input_mtx);
}

/* ---- the public input API ------------------------------------------------------- */

void smssession_press(smssession *ses, int target, int down)
{
    if (!ses->running || target < 0 || target >= SMS_TARGET_COUNT)
        return;
    if (target >= SMS_TARGET_SYSACT(0)) {
        if (down)
            smssession_sysaction(ses, target - SMS_TARGET_SYSACT(0));
        return;
    }
    source_press(ses, target, SRC_KEYBOARD, down);
}

void smssession_sysaction(smssession *ses, int sysact)
{
    switch (sysact) {
    case SMS_SYSACT_RESET_CONFIG:
        smssession_reset_to_config(ses);
        break;
    case SMS_SYSACT_SOFT_RESET:
        smssession_soft_reset(ses);
        break;
    case SMS_SYSACT_DEBUG_STOP:
        if (ses->running) {
            smsdebug *dbg = smsdebug_get(ses);
            smsdebug_attach(dbg);
            smsdebug_stop(dbg);
        }
        break;
    default:
        break;
    }
}

void smssession_sysaction_post(smssession *ses, int sysact)
{
    if (sysact < 0 || sysact >= SMS_SYSACT_COUNT)
        return;
    pthread_mutex_lock(&ses->sysact_mtx);
    ses->sysact_pending |= 1u << sysact;
    pthread_mutex_unlock(&ses->sysact_mtx);
}

int smssession_sysaction_take(smssession *ses, int *out)
{
    int found = 0;
    pthread_mutex_lock(&ses->sysact_mtx);
    for (int idx = 0; idx < SMS_SYSACT_COUNT; idx++) {
        if (ses->sysact_pending & (1u << idx)) {
            ses->sysact_pending &= ~(1u << idx);
            if (out)
                *out = idx;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&ses->sysact_mtx);
    return found;
}

unsigned smssession_buttons_held(smssession *ses, int port)
{
    unsigned held;
    if (port < 0 || port >= SMS_PLAYERS)
        return 0;
    pthread_mutex_lock(&ses->input_mtx);
    held = held_locked(ses, port);
    pthread_mutex_unlock(&ses->input_mtx);
    return held;
}

void smssession_pointer_set(smssession *ses, int port, unsigned mask)
{
    if (!ses->running || port < 0 || port >= SMS_PLAYERS)
        return;
    pthread_mutex_lock(&ses->input_mtx);
    for (int act = 0; act < SMS_ACT_PER_PORT; act++)
        set_src(&ses->pad_src[port][act], SRC_POINTER, (mask >> act) & 1u);
    push_controller_locked(ses, port);
    pthread_mutex_unlock(&ses->input_mtx);
}

int smssession_switch_held(smssession *ses, int swnum)
{
    int held;
    if (swnum < 0 || swnum >= SMS_SW_COUNT)
        return 0;
    pthread_mutex_lock(&ses->input_mtx);
    held = ses->sw_src[swnum] != 0;
    pthread_mutex_unlock(&ses->input_mtx);
    return held;
}

/* Everything the keyboard and the controllers window's console buttons hold,
 * let go (focus loss): what a key's press asserted, whichever way it was
 * asserted. A gamepad's own holds are left to the gamepad, the pointer's to
 * smssession_pointer_set. */
void smssession_release_all(smssession *ses)
{
    if (!ses)
        return;
    memset(ses->held_keysym, 0, sizeof ses->held_keysym);
    pthread_mutex_lock(&ses->input_mtx);
    for (int port = 0; port < SMS_PLAYERS; port++) {
        for (int act = 0; act < SMS_ACT_PER_PORT; act++)
            ses->pad_src[port][act] &= ~SRC_KEYBOARD;
        push_controller_locked(ses, port);
    }
    for (int swnum = 0; swnum < SMS_SW_COUNT; swnum++) {
        ses->sw_src[swnum] &= ~SRC_KEYBOARD;
        push_switch_locked(ses, swnum);
    }
    pthread_mutex_unlock(&ses->input_mtx);
}

/* The gamepad thread's entry points: the same routing, from the pad's own
 * source bits, never touching the settings or the debugger. */
void session_gamepad_press(struct smssession *ses, int owner, int target, int down)
{
    if (!ses->running || owner < 0 || owner >= SMS_MAX_PADS || target < 0
        || target >= SMS_TARGET_SYSACT(0))
        return;
    source_press(ses, target, SRC_PAD(owner), down);
}

void session_gamepad_stick(struct smssession *ses, int owner, int port, int act, int down)
{
    if (!ses->running || owner < 0 || owner >= SMS_MAX_PADS || port < 0 || port >= SMS_PLAYERS
        || act < 0 || act >= SMS_ACT_PER_PORT)
        return;
    source_press(ses, SMS_TARGET_PORT(port, act), SRC_STICK(owner), down);
}

void smssession_set_analog(smssession *ses, int joystick)
{
    pthread_mutex_lock(&ses->input_mtx);
    ses->opts.analog_joystick = joystick ? 1 : 0;
    pthread_mutex_unlock(&ses->input_mtx);
    smssession_set_int(ses, "analog_joystick", joystick ? 1 : 0);
}

/* The pad thread's read of the live flag (the UI thread writes it). */
int session_analog_joystick(struct smssession *ses)
{
    int value;
    pthread_mutex_lock(&ses->input_mtx);
    value = ses->opts.analog_joystick;
    pthread_mutex_unlock(&ses->input_mtx);
    return value;
}
