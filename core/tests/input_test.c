/*
 * input_test -- the input source merge, through the session's private entry
 * points the gamepad thread uses (session_gamepad_press / _stick), no
 * hardware: a key letting go never drops a held pad direction; two pads on
 * one contact, or a key and a pad, do not release each other; the window's
 * pointer is a source of its own; UP+DOWN and LEFT+RIGHT held together cancel
 * to neither; release_all lets go of the keyboard's holds only; two threads
 * working different sources of one contact leave it deterministic; the
 * console's Pause button is merged the same way; system actions queue once.
 * What the merge hands the machine is checked in the host's live mask.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "session_internal.h"
#include "test_tmpdir.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static void sleep_ms(int msec)
{
    struct timespec span = { msec / 1000, (long)(msec % 1000) * 1000000L };
    nanosleep(&span, NULL);
}

#define BIT(act) (1u << (act))
#define TGT_UP    SMS_TARGET_PORT(0, SMS_ACT_UP)
#define TGT_DOWN  SMS_TARGET_PORT(0, SMS_ACT_DOWN)
#define TGT_LEFT  SMS_TARGET_PORT(0, SMS_ACT_LEFT)
#define TGT_RIGHT SMS_TARGET_PORT(0, SMS_ACT_RIGHT)
#define TGT_BTN1  SMS_TARGET_PORT(0, SMS_ACT_1)
#define TGT_PAUSE SMS_TARGET_SWITCH(SMS_SW_PAUSE)

/* The mask the machine latched at its last frame boundary, polled: the
 * machine thread takes it once a frame. */
static int host_mask_is(int port, unsigned want)
{
    unsigned got = ~want;
    for (int tries = 0; tries < 500; tries++) {
        sms_host_lock();
        got = sms_host_machine()->pad_live[port];
        sms_host_unlock();
        if (got == want)
            return 1;
        sleep_ms(10);
    }
    printf("  (host mask %02X, want %02X)\n", got, want);
    return 0;
}

static int host_pause_is(int want)
{
    int got = !want;
    for (int tries = 0; tries < 500; tries++) {
        sms_host_lock();
        got = sms_host_machine()->pause_live;
        sms_host_unlock();
        if (got == want)
            return 1;
        sleep_ms(10);
    }
    return 0;
}

/* the contacts as the machine's bits */
#define HOST_UP    SMS_PAD_UP
#define HOST_DOWN  SMS_PAD_DOWN
#define HOST_LEFT  SMS_PAD_LEFT
#define HOST_RIGHT SMS_PAD_RIGHT
#define HOST_1     SMS_PAD_1

/* ---- the concurrency case ------------------------------------------------ */

#define ROUNDS 20000

typedef struct {
    struct smssession *ses;
    int owner;
    int final_down;
} worker;

static void *toggler(void *arg)
{
    worker *wrk = arg;

    for (int round = 0; round < ROUNDS; round++)
        session_gamepad_press(wrk->ses, wrk->owner, TGT_BTN1, round & 1);
    session_gamepad_press(wrk->ses, wrk->owner, TGT_BTN1, wrk->final_down);
    return NULL;
}

static void *keyer(void *arg)
{
    worker *wrk = arg;

    for (int round = 0; round < ROUNDS; round++)
        smssession_press(wrk->ses, TGT_BTN1, round & 1);
    smssession_press(wrk->ses, TGT_BTN1, wrk->final_down);
    return NULL;
}

int main(void)
{
    char cfg[512], data[512];
    smssession_paths paths;
    smssession *ses;
    smssession_start_opts opts;

    test_tmpdir(cfg, sizeof cfg, "icfg");
    test_tmpdir(data, sizeof data, "idata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";      /* no runtime: only the input matters */
    ses = smssession_new(&paths);
    if (!ses)
        return 1;
    smssession_default_opts(ses, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    if (smssession_start(ses, &opts) != 0) {
        printf("FAIL: start: %s\n", smssession_last_error(ses));
        return 1;
    }

    check(smssession_buttons_held(ses, 0) == 0 && smssession_buttons_held(ses, 1) == 0
          && smssession_buttons_held(ses, -1) == 0 && smssession_buttons_held(ses, 2) == 0,
          "a started session holds nothing (and out-of-range ports hold nothing)");

    /* (1) a key letting go does not drop a held pad direction */
    session_gamepad_stick(ses, 0, 0, SMS_ACT_UP, 1);
    check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_UP), "a pad's stick holds Up");
    check(smssession_key(ses, SMS_KEYSYM_UP, 1) == 1 && smssession_buttons_held(ses, 0) == BIT(SMS_ACT_UP),
          "and the Up key on top of it still holds just Up");
    check(smssession_key(ses, SMS_KEYSYM_UP, 0) == 1 && smssession_buttons_held(ses, 0) == BIT(SMS_ACT_UP),
          "the key letting go does not drop the stick's Up");
    check(host_mask_is(0, HOST_UP), "the machine still sees Up");
    session_gamepad_stick(ses, 0, 0, SMS_ACT_UP, 0);
    check(smssession_buttons_held(ses, 0) == 0 && host_mask_is(0, 0), "the stick letting go releases it");

    /* the other way: the pad letting go does not drop a held key */
    smssession_key(ses, SMS_KEYSYM_UP, 1);
    session_gamepad_stick(ses, 0, 0, SMS_ACT_UP, 1);
    session_gamepad_stick(ses, 0, 0, SMS_ACT_UP, 0);
    check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_UP), "the stick letting go does not drop the key's Up");
    smssession_key(ses, SMS_KEYSYM_UP, 0);
    check(smssession_buttons_held(ses, 0) == 0, "and the key letting go releases it");

    /* (2) two owners on one contact */
    session_gamepad_press(ses, 0, TGT_BTN1, 1);
    session_gamepad_press(ses, 1, TGT_BTN1, 1);
    check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_1), "two pads hold button 1");
    session_gamepad_press(ses, 0, TGT_BTN1, 0);
    check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_1), "one of them letting go keeps it held");
    session_gamepad_press(ses, 1, TGT_BTN1, 0);
    check(smssession_buttons_held(ses, 0) == 0, "both letting go releases it");
    /* a pad's button and its stick are different sources, too */
    session_gamepad_press(ses, 2, TGT_UP, 1);
    session_gamepad_stick(ses, 2, 0, SMS_ACT_UP, 1);
    session_gamepad_press(ses, 2, TGT_UP, 0);
    check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_UP), "one pad's stick holds what its button let go of");
    session_gamepad_stick(ses, 2, 0, SMS_ACT_UP, 0);
    check(smssession_buttons_held(ses, 0) == 0, "and releases it");

    /* (3) the window's pointer is its own source */
    smssession_pointer_set(ses, 0, BIT(SMS_ACT_UP) | BIT(SMS_ACT_1));
    check(smssession_buttons_held(ses, 0) == (BIT(SMS_ACT_UP) | BIT(SMS_ACT_1)), "the pointer holds Up and button 1");
    check(host_mask_is(0, HOST_UP | HOST_1), "the machine sees both");
    smssession_key(ses, 'z', 1);
    smssession_key(ses, 'z', 0);
    check(smssession_buttons_held(ses, 0) == (BIT(SMS_ACT_UP) | BIT(SMS_ACT_1)),
          "a tap of the key for button 1 does not release the pointer's hold");
    smssession_pointer_set(ses, 0, BIT(SMS_ACT_LEFT));
    check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_LEFT), "a new pointer mask replaces the old whole");
    smssession_pointer_set(ses, 1, BIT(SMS_ACT_2));
    check(smssession_buttons_held(ses, 1) == BIT(SMS_ACT_2) && smssession_buttons_held(ses, 0) == BIT(SMS_ACT_LEFT),
          "a port's pointer mask is that port's alone");
    smssession_pointer_set(ses, 0, 0);
    smssession_pointer_set(ses, 1, 0);
    check(smssession_buttons_held(ses, 0) == 0 && smssession_buttons_held(ses, 1) == 0, "mask 0 lets go");
    smssession_pointer_set(ses, 2, BIT(SMS_ACT_UP));
    smssession_pointer_set(ses, -1, BIT(SMS_ACT_UP));
    check(smssession_buttons_held(ses, 0) == 0 && smssession_buttons_held(ses, 1) == 0, "a bad port is ignored");

    /* (4) opposed pairs cancel */
    session_gamepad_stick(ses, 0, 0, SMS_ACT_UP, 1);
    session_gamepad_stick(ses, 0, 0, SMS_ACT_DOWN, 1);
    check(smssession_buttons_held(ses, 0) == 0, "Up and Down together hold neither");
    check(host_mask_is(0, 0), "and the machine sees neither");
    session_gamepad_stick(ses, 0, 0, SMS_ACT_UP, 0);
    check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_DOWN) && host_mask_is(0, HOST_DOWN),
          "Up letting go leaves Down");
    session_gamepad_stick(ses, 0, 0, SMS_ACT_DOWN, 0);
    session_gamepad_press(ses, 0, TGT_LEFT, 1);
    smssession_key(ses, SMS_KEYSYM_RIGHT, 1);
    check(smssession_buttons_held(ses, 0) == 0, "Left from a pad and Right from a key hold neither");
    smssession_key(ses, SMS_KEYSYM_RIGHT, 0);
    check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_LEFT) && host_mask_is(0, HOST_LEFT), "and Left is back when Right lets go");
    session_gamepad_press(ses, 0, TGT_LEFT, 0);
    smssession_pointer_set(ses, 0, BIT(SMS_ACT_UP) | BIT(SMS_ACT_DOWN) | BIT(SMS_ACT_1));
    check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_1), "a diagonal drag cannot hold Up and Down, but button 1 stays");
    smssession_pointer_set(ses, 0, 0);
    /* the players are independent */
    session_gamepad_stick(ses, 0, 0, SMS_ACT_UP, 1);
    session_gamepad_stick(ses, 0, 1, SMS_ACT_DOWN, 1);
    check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_UP) && smssession_buttons_held(ses, 1) == BIT(SMS_ACT_DOWN),
          "Player 1's Up and Player 2's Down do not cancel");
    session_gamepad_stick(ses, 0, 0, SMS_ACT_UP, 0);
    session_gamepad_stick(ses, 0, 1, SMS_ACT_DOWN, 0);
    check(smssession_buttons_held(ses, 0) == 0 && smssession_buttons_held(ses, 1) == 0, "released");

    /* (5) release_all lets go of the keyboard only */
    smssession_key(ses, 'z', 1);                                  /* keyboard: button 1 */
    session_gamepad_press(ses, 0, TGT_RIGHT, 1);                      /* a pad: Right */
    smssession_pointer_set(ses, 0, BIT(SMS_ACT_UP));              /* the pointer: Up */
    smssession_press(ses, TGT_PAUSE, 1);                              /* the window's Pause */
    check(smssession_buttons_held(ses, 0) == (BIT(SMS_ACT_1) | BIT(SMS_ACT_RIGHT) | BIT(SMS_ACT_UP))
          && smssession_switch_held(ses, SMS_SW_PAUSE), "three sources and Pause held");
    smssession_release_all(ses);
    check(smssession_buttons_held(ses, 0) == (BIT(SMS_ACT_RIGHT) | BIT(SMS_ACT_UP)),
          "release_all drops the key's button 1 and leaves the pad's Right and the pointer's Up");
    check(!smssession_switch_held(ses, SMS_SW_PAUSE), "and the Pause the window held");
    check(host_mask_is(0, HOST_RIGHT | HOST_UP) && host_pause_is(0), "the machine agrees");
    smssession_key(ses, 'z', 0);
    check(smssession_buttons_held(ses, 0) == (BIT(SMS_ACT_RIGHT) | BIT(SMS_ACT_UP)),
          "a key release after release_all releases nothing more");
    session_gamepad_press(ses, 0, TGT_RIGHT, 0);
    smssession_pointer_set(ses, 0, 0);
    check(smssession_buttons_held(ses, 0) == 0, "all let go");

    /* (7) the console's Pause button merges its sources */
    session_gamepad_press(ses, 0, TGT_PAUSE, 1);
    check(smssession_switch_held(ses, SMS_SW_PAUSE) && host_pause_is(1), "a pad holds Pause");
    smssession_key(ses, SMS_KEYSYM_RETURN, 1);
    smssession_key(ses, SMS_KEYSYM_RETURN, 0);
    check(smssession_switch_held(ses, SMS_SW_PAUSE), "the Return key tapping does not release the pad's Pause");
    session_gamepad_press(ses, 0, TGT_PAUSE, 0);
    check(!smssession_switch_held(ses, SMS_SW_PAUSE) && host_pause_is(0), "and the pad letting go releases it");
    smssession_press(ses, TGT_PAUSE, 1);
    session_gamepad_press(ses, 1, TGT_PAUSE, 1);
    session_gamepad_press(ses, 1, TGT_PAUSE, 0);
    check(smssession_switch_held(ses, SMS_SW_PAUSE), "a pad's tap does not release the window's Pause");
    smssession_press(ses, TGT_PAUSE, 0);
    check(!smssession_switch_held(ses, SMS_SW_PAUSE), "released");
    check(!smssession_switch_held(ses, -1) && !smssession_switch_held(ses, SMS_SW_COUNT), "an out-of-range switch is not held");

    /* a target no source drives is ignored, not a crash */
    session_gamepad_press(ses, 0, -1, 1);
    session_gamepad_press(ses, 0, SMS_TARGET_SYSACT(0), 1);
    session_gamepad_press(ses, SMS_MAX_PADS, TGT_BTN1, 1);
    session_gamepad_press(ses, -1, TGT_BTN1, 1);
    session_gamepad_stick(ses, 0, 2, SMS_ACT_UP, 1);
    session_gamepad_stick(ses, 0, 0, SMS_ACT_PER_PORT, 1);
    check(smssession_buttons_held(ses, 0) == 0 && smssession_buttons_held(ses, 1) == 0,
          "out-of-range targets, owners, ports and contacts hold nothing");

    /* (8) system actions queue once, in order, and only valid ones */
    {
        int act = -1;

        check(!smssession_sysaction_take(ses, &act), "nothing is queued");
        smssession_sysaction_post(ses, SMS_SYSACT_SOFT_RESET);
        smssession_sysaction_post(ses, SMS_SYSACT_SOFT_RESET);
        smssession_sysaction_post(ses, -1);
        smssession_sysaction_post(ses, SMS_SYSACT_COUNT);
        check(smssession_sysaction_take(ses, &act) && act == SMS_SYSACT_SOFT_RESET,
              "a posted action is taken");
        check(!smssession_sysaction_take(ses, &act), "twice posted is once taken; a bad action never queued");
        smssession_sysaction_post(ses, SMS_SYSACT_DEBUG_STOP);
        smssession_sysaction_post(ses, SMS_SYSACT_RESET_CONFIG);
        check(smssession_sysaction_take(ses, &act) && act == SMS_SYSACT_RESET_CONFIG
              && smssession_sysaction_take(ses, &act) && act == SMS_SYSACT_DEBUG_STOP
              && !smssession_sysaction_take(ses, NULL), "two actions come out lowest first");
    }
    /* releasing a system action's key runs nothing: only the down edge fires */
    smssession_press(ses, SMS_TARGET_SYSACT(SMS_SYSACT_DEBUG_STOP), 0);
    check(smssession_buttons_held(ses, 0) == 0, "the up edge of a system action target is ignored");

    /* (6) two threads, different sources, one contact. The keyboard's last
     * word is "held", the pad's is "released": the contact must end held;
     * then the keyboard lets go too. */
    {
        pthread_t thr_pad, thr_key;
        worker pad_work = { ses, 3, 0 }, key_work = { ses, 0, 1 };

        pthread_create(&thr_pad, NULL, toggler, &pad_work);
        pthread_create(&thr_key, NULL, keyer, &key_work);
        pthread_join(thr_pad, NULL);
        pthread_join(thr_key, NULL);
        check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_1) && host_mask_is(0, HOST_1),
              "after 20000 toggles from two threads the key's final hold is the one left");
        smssession_press(ses, TGT_BTN1, 0);
        check(smssession_buttons_held(ses, 0) == 0 && host_mask_is(0, 0),
              "and releasing it leaves nothing held, in the merge and in the machine");

        pad_work.final_down = 1;
        key_work.final_down = 0;
        pthread_create(&thr_pad, NULL, toggler, &pad_work);
        pthread_create(&thr_key, NULL, keyer, &key_work);
        pthread_join(thr_pad, NULL);
        pthread_join(thr_key, NULL);
        check(smssession_buttons_held(ses, 0) == BIT(SMS_ACT_1), "the pad's final hold is the one left, the other way round");
        session_gamepad_press(ses, 3, TGT_BTN1, 0);
        check(smssession_buttons_held(ses, 0) == 0 && host_mask_is(0, 0), "released");
    }

    /* a stopped session ignores input (and the held state is gone at the next start) */
    smssession_stop(ses);
    session_gamepad_press(ses, 0, TGT_BTN1, 1);
    smssession_press(ses, TGT_BTN1, 1);
    smssession_pointer_set(ses, 0, BIT(SMS_ACT_UP));
    check(smssession_buttons_held(ses, 0) == 0, "a stopped session holds nothing");

    smssession_free(ses);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
