/*
 * gamepad_map_test -- the gamepad layer end to end with an SDL3 virtual
 * gamepad: hot-plug pickup, the default mapping onto the joypad (South = 1,
 * East = 2, the D-pad -- a bindable button like the others -- and, with
 * "analog sticks drive the D-pad" on, the left stick for directions) and the
 * console's buttons (Start = Pause, Back = the Reset button), a D-pad button
 * rebound to a joypad button, the sources not stepping on each other (a key
 * letting go does not drop a held direction), map mode swallowing a press,
 * two pads on one port, and hot-unplug releasing everything the pad held --
 * with no hardware and no human. Then the pad's port assignment: written to
 * the settings ("pad_assign") when made, restored when a new session (a
 * restart) finds the pad again, found through a stale GUID by the name, kept
 * through garbage, and removed by Automatic.
 *
 * What the pad holds is read back through smssession_buttons_held and
 * smssession_switch_held, which report the same per-source bits the machine
 * is driven from. SKIPs (77) where SDL has no virtual joysticks or a
 * machine's real pads leave no port free.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "smssession.h"
#include "test_tmpdir.h"

#define PAD_NAME "smstest virtual pad"

static int failures;
static int vport;   /* the player the virtual pad was given */

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* The pad thread syncs devices a few times a second and polls state
 * faster; give it two seconds. */
static void expect(smssession *s, unsigned want, int pause, int reset, const char *what)
{
    unsigned held = 0;
    int p = 0, r = 0;
    for (int i = 0; i < 100; i++) {
        held = smssession_buttons_held(s, vport);
        p = smssession_switch_held(s, SMS_SW_PAUSE);
        r = smssession_switch_held(s, SMS_SW_RESET);
        if (held == want && p == pause && r == reset) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (held %02X want %02X, pause %d/%d, reset %d/%d)\n",
           what, held, want, p, pause, r, reset);
    failures++;
}

#define ACTBIT(act) (1u << (act))

/* A session on paths' directories with only the gamepad thread. */
static smssession *session_up(smssession_paths *paths)
{
    smssession *ses = smssession_new(paths);
    smssession_start_opts opts;
    if (!ses)
        return NULL;
    smssession_default_opts(ses, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 1;
    if (smssession_start(ses, &opts) != 0) {
        printf("FAIL: start: %s\n", smssession_last_error(ses));
        smssession_free(ses);
        return NULL;
    }
    return ses;
}

/* The virtual pad's slot now, or -1. */
static int pad_slot_now(smssession *ses)
{
    for (int idx = 0; idx < smssession_gamepad_count(ses); idx++) {
        char name[64];
        smssession_gamepad_name(ses, idx, name, sizeof name);
        if (strcmp(name, PAD_NAME) == 0)
            return idx;
    }
    return -1;
}

/* The virtual pad's slot once the pad thread has found it (polled, up to
 * five seconds: a slow runner is slow to enumerate), or -1. */
static int wait_for_pad(smssession *ses)
{
    int idx = pad_slot_now(ses);
    for (int tries = 0; tries < 250 && idx < 0; tries++) {
        sleep_ms(20);
        idx = pad_slot_now(ses);
    }
    return idx;
}

/* The pad_assign setting with the pad's entry in it? */
static int has_entry(smssession *ses, const char *guid, int port)
{
    char want[160];
    snprintf(want, sizeof want, "%s|%s=%d", guid, PAD_NAME, port);
    return strstr(smssession_get_str(ses, "pad_assign", ""), want) != NULL;
}

/* A pad's port assignment survives a restart. The virtual pad stays attached
 * across the sessions (SDL is held open by the caller). */
static void persistence(smssession_paths *paths)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    smssession *ses;
    char guid[64];
    int idx, want, auto_port;

    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = PAD_NAME;
    vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        printf("FAIL: the virtual pad would not attach again: %s\n", SDL_GetError());
        failures++;
        return;
    }
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(vid), guid, sizeof guid);

    ses = session_up(paths);
    if (!ses) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(ses);
    check(idx >= 0, "persistence: the pad is picked up");
    if (idx < 0) { smssession_free(ses); SDL_DetachVirtualJoystick(vid); return; }
    check(smssession_gamepad_assignment(ses, idx) == -1, "a pad nobody assigned is automatic");
    check(strstr(smssession_get_str(ses, "pad_assign", ""), guid) == NULL, "and has no entry");
    auto_port = smssession_gamepad_effective_port(ses, idx);
    want = auto_port == 0 ? 1 : 0;

    smssession_gamepad_assign(ses, idx, want);
    check(smssession_gamepad_assignment(ses, idx) == want
          && smssession_gamepad_effective_port(ses, idx) == want, "assigning drives the other port");
    check(has_entry(ses, guid, want), "and writes <guid>|<name>=<port> to pad_assign");

    /* the file itself, not just the in-memory store */
    {
        char path[600], line[2048];
        int found = 0;
        FILE *fil;
        snprintf(path, sizeof path, "%s/settings.ini", paths->config_dir);
        fil = fopen(path, "r");
        if (fil) {
            while (fgets(line, sizeof line, fil))
                if (!strncmp(line, "pad_assign=", 11) && strstr(line, guid))
                    found = 1;
            fclose(fil);
        }
        check(found, "the settings file on disk holds it");
    }

    /* restart: a new session on the same settings */
    smssession_free(ses);
    ses = session_up(paths);
    if (!ses) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(ses);
    check(idx >= 0, "restart: the pad is picked up again");
    if (idx >= 0) {
        check(smssession_gamepad_assignment(ses, idx) == want, "the assignment is restored on device add");
        check(smssession_gamepad_effective_port(ses, idx) == want, "and the pad drives that port");
    }

    /* a stale GUID (another USB port): the name finds the entry. Rewrite the
     * stored GUID, restart. */
    {
        char text[300];
        snprintf(text, sizeof text, "ffffffffffffffffffffffffffffffff|%s=%d", PAD_NAME, want);
        smssession_set_str(ses, "pad_assign", text);
    }
    smssession_free(ses);
    ses = session_up(paths);
    if (!ses) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(ses);
    check(idx >= 0 && smssession_gamepad_assignment(ses, idx) == want,
          "a different GUID with the same name gets the assignment too");

    /* Automatic removes the entry, and the next run starts automatic */
    smssession_gamepad_assign(ses, idx, want);   /* back under its own GUID */
    check(has_entry(ses, guid, want), "assigned under the pad's own GUID again");
    smssession_gamepad_assign(ses, idx, -1);
    check(smssession_gamepad_assignment(ses, idx) == -1, "Automatic: no explicit port");
    check(strstr(smssession_get_str(ses, "pad_assign", ""), PAD_NAME) == NULL,
          "and the entry is gone from pad_assign");
    smssession_free(ses);
    ses = session_up(paths);
    if (!ses) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(ses);
    check(idx >= 0 && smssession_gamepad_assignment(ses, idx) == -1
          && smssession_gamepad_effective_port(ses, idx) == auto_port, "after a restart it is automatic again");

    /* garbage in the setting is ignored, silently */
    smssession_set_str(ses, "pad_assign", ";;|=;zz|x=9;%%%;" "00000000000000000000000000000000|" PAD_NAME "=7");
    smssession_free(ses);
    ses = session_up(paths);
    if (!ses) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(ses);
    check(idx >= 0 && smssession_gamepad_assignment(ses, idx) == -1,
          "a malformed or out-of-range pad_assign leaves the pad automatic");
    smssession_gamepad_assign(ses, idx, want);
    check(has_entry(ses, guid, want), "and a new assignment still writes");
    smssession_gamepad_assign(ses, idx, -1);

    /* a hot replug within a run restores it too */
    smssession_gamepad_assign(ses, idx, want);
    SDL_DetachVirtualJoystick(vid);
    for (int tries = 0; tries < 250 && pad_slot_now(ses) >= 0; tries++)
        sleep_ms(20);
    check(pad_slot_now(ses) < 0, "unplugged: the pad is gone");
    vid = SDL_AttachVirtualJoystick(&desc);
    idx = vid ? wait_for_pad(ses) : -1;
    check(idx >= 0 && smssession_gamepad_assignment(ses, idx) == want, "unplugged and plugged back in: the port comes back");
    smssession_gamepad_assign(ses, idx, -1);

    smssession_free(ses);
    if (vid)
        SDL_DetachVirtualJoystick(vid);
}

int main(void)
{
    char cfg[512], data[512];
    smssession_paths p;
    smssession *s;
    smssession_start_opts o;
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    SDL_Joystick *vj;
    int i, baseline;

    /* held open for the whole run: a session stopping must not take the
     * virtual pads with it (the persistence tests restart sessions) */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        return 1;
    test_tmpdir(cfg, sizeof cfg, "gmcfg");
    test_tmpdir(data, sizeof data, "gmdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    p.fujinet_lib = "";      /* the pad only */
    s = smssession_new(&p);
    if (!s)
        return 1;
    smssession_default_opts(s, &o);
    o.enable_fujinet = 0;
    o.enable_audio = 0;
    o.enable_gamepad = 1;
    if (smssession_start(s, &o) != 0) {
        printf("FAIL: start: %s\n", smssession_last_error(s));
        return 1;
    }

    /* Real pads may already be attached: let the first device scan find
     * them, so the virtual one is known by the slot it adds. */
    sleep_ms(700);
    baseline = smssession_gamepad_count(s);

    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = PAD_NAME;
    vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        smssession_free(s);
        return 77;
    }
    vj = SDL_OpenJoystick(vid);
    if (!vj)
        return 1;

    for (i = 0; i < 100 && smssession_gamepad_count(s) <= baseline; i++)
        sleep_ms(20);
    vport = -1;
    for (i = 0; i < smssession_gamepad_count(s); i++) {
        char name[64];
        smssession_gamepad_name(s, i, name, sizeof name);
        if (strcmp(name, PAD_NAME) == 0)
            vport = smssession_gamepad_effective_port(s, i);
    }
    if (vport < 0 || vport > 1) {
        printf("%s: the virtual pad got no player (%d; real pads attached?)\n",
               vport < 0 && smssession_gamepad_count(s) <= baseline ? "FAIL" : "SKIP", vport);
        SDL_CloseJoystick(vj);
        SDL_DetachVirtualJoystick(vid);
        smssession_free(s);
        return smssession_gamepad_count(s) <= baseline ? 1 : 77;
    }
    printf("ok: the virtual pad was picked up as player %d\n", vport + 1);

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, ACTBIT(SMS_ACT_1), 0, 0, "South is button 1");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect(s, 0, 0, 0, "released");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    expect(s, ACTBIT(SMS_ACT_2), 0, 0, "East is button 2");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
    expect(s, 0, 0, 0, "released");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, ACTBIT(SMS_ACT_UP), 0, 0, "D-pad up");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    expect(s, ACTBIT(SMS_ACT_LEFT), 0, 0, "D-pad left");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    expect(s, 0, 0, 0, "released");

    /* the stick past its threshold drives the D-pad too (a pad that reports
     * its D-pad as axes would otherwise drive nothing) */
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    expect(s, ACTBIT(SMS_ACT_RIGHT) | ACTBIT(SMS_ACT_DOWN), 0, 0, "the stick right and down");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 0);
    expect(s, 0, 0, 0, "centred");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    expect(s, 0, 1, 0, "Start is the Pause button");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_BACK, true);
    expect(s, 0, 0, 1, "Back is the Reset button");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_BACK, false);
    expect(s, 0, 0, 0, "released");

    /* with "analog sticks drive the D-pad" off the stick holds nothing, at
     * once, and the D-pad still works; back on, the held stick works again */
    smssession_set_analog(s, 0);
    check(smssession_get_int(s, "analog_joystick", 1) == 0, "the setting is kept");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 30000);
    sleep_ms(300);
    expect(s, 0, 0, 0, "analog off: the stick holds nothing");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    expect(s, ACTBIT(SMS_ACT_LEFT), 0, 0, "while the D-pad still steers");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    smssession_set_analog(s, 1);
    expect(s, ACTBIT(SMS_ACT_RIGHT), 0, 0, "switched back on, the stick (still held) steers");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    expect(s, 0, 0, 0, "centred");

    /* the sources are separate: a key letting go does not drop a held D-pad
     * direction, nor a D-pad letting go a held key, nor stick and D-pad each other */
    {
        const uint32_t vkey = vport ? 'i' : SMS_KEYSYM_UP;      /* this player's Up key */

        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
        expect(s, ACTBIT(SMS_ACT_UP), 0, 0, "the D-pad holds Up");
        smssession_key(s, vkey, 1);
        smssession_key(s, vkey, 0);
        expect(s, ACTBIT(SMS_ACT_UP), 0, 0, "a tap of the Up key does not release the D-pad's Up");
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, -30000);
        expect(s, ACTBIT(SMS_ACT_UP), 0, 0, "the stick also pushing Up changes nothing");
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
        expect(s, ACTBIT(SMS_ACT_UP), 0, 0, "and the D-pad letting go leaves the stick's Up");
        smssession_key(s, vkey, 1);
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 0);
        expect(s, ACTBIT(SMS_ACT_UP), 0, 0, "and the stick letting go leaves the key's Up");
        smssession_key(s, vkey, 0);
        expect(s, 0, 0, 0, "released");
    }

    /* a D-pad direction rebound to a joypad button stops steering, and is
     * that button (the binding table is the only thing between them) */
    smssession_binding_set_button(s, SMS_TARGET_PORT(vport, SMS_ACT_1), SMS_PAD_BTN_DPAD_UP, NULL, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, ACTBIT(SMS_ACT_1), 0, 0, "D-pad up, bound to button 1, is button 1 and not Up any more");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    expect(s, 0, 0, 0, "released");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    sleep_ms(200);
    expect(s, 0, 0, 0, "South, which button 1 gave up, holds nothing");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    sleep_ms(100);      /* the pad thread must see the release before the next press */
    smssession_bindings_reset(s);

    /* a button changing its binding while held lets go of what it pressed */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, ACTBIT(SMS_ACT_1), 0, 0, "South holds button 1");
    smssession_binding_set_button(s, SMS_TARGET_PORT(vport, SMS_ACT_1), SMS_PAD_BTN_NONE, NULL, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect(s, 0, 0, 0, "unbound while held, its release still lets go");
    sleep_ms(100);
    smssession_bindings_reset(s);

    /* Map mode swallows every button press while armed: the one that is
     * captured does not also do what it did */
    smssession_gamepad_capture_begin(s);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    {
        int btn = SMS_PAD_BTN_NONE, got = 0;

        for (int tries = 0; tries < 100 && !got; tries++) {
            got = smssession_gamepad_capture_poll(s, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == SMS_PAD_BTN_SOUTH, "map mode captures South");
    }
    sleep_ms(100);
    expect(s, 0, 0, 0, "and the captured press held nothing");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    sleep_ms(100);
    expect(s, ACTBIT(SMS_ACT_2), 0, 0, "after the capture the pad drives again");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
    expect(s, 0, 0, 0, "released");

    /* two pads assigned to one port: the first to connect drives it and the
     * second holds nothing, whatever it presses */
    {
        SDL_VirtualJoystickDesc desc_two;
        SDL_JoystickID vid_two;
        SDL_Joystick *vj_two;
        int idx_one = pad_slot_now(s), idx_two = -1;

        SDL_INIT_INTERFACE(&desc_two);
        desc_two.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc_two.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc_two.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc_two.name = PAD_NAME;
        vid_two = SDL_AttachVirtualJoystick(&desc_two);
        vj_two = vid_two ? SDL_OpenJoystick(vid_two) : NULL;
        if (vj_two) {
            for (int tries = 0; tries < 250 && idx_two < 0; tries++) {
                for (int idx = 0; idx < smssession_gamepad_count(s); idx++) {
                    char name[96];
                    smssession_gamepad_name(s, idx, name, sizeof name);
                    /* SDL names a virtual pad by its GUID's mapping: both read PAD_NAME */
                    if (idx != idx_one && !strcmp(name, PAD_NAME))
                        idx_two = idx;
                }
                if (idx_two < 0) sleep_ms(20);
            }
        }
        check(idx_one >= 0 && idx_two >= 0 && idx_one < idx_two, "a second virtual pad connects after the first");
        if (idx_one >= 0 && idx_two >= 0) {
            smssession_gamepad_assign(s, idx_one, vport);
            smssession_gamepad_assign(s, idx_two, vport);
            check(smssession_gamepad_effective_port(s, idx_one) == vport
                  && smssession_gamepad_effective_port(s, idx_two) == -1,
                  "both on one port: the first drives it, the second drives nothing");
            SDL_SetJoystickVirtualButton(vj_two, SDL_GAMEPAD_BUTTON_SOUTH, true);
            sleep_ms(200);
            expect(s, 0, 0, 0, "so the second pad's press holds nothing");
            SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
            SDL_SetJoystickVirtualButton(vj_two, SDL_GAMEPAD_BUTTON_SOUTH, false);
            expect(s, ACTBIT(SMS_ACT_2), 0, 0, "and its release does not disturb the first pad's hold");
            SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
            expect(s, 0, 0, 0, "released");
            smssession_gamepad_assign(s, idx_two, -1);
            smssession_gamepad_assign(s, idx_one, -1);
        }
        if (vj_two)
            SDL_CloseJoystick(vj_two);
        if (vid_two)
            SDL_DetachVirtualJoystick(vid_two);
        for (int tries = 0; tries < 250 && smssession_gamepad_count(s) != baseline + 1; tries++)
            sleep_ms(20);
        check(smssession_gamepad_count(s) == baseline + 1, "the second pad unplugs");
    }

    /* A trigger past half travel used to deadlock the gamepad thread on its
     * own lock, and the Controllers panel's timers (same lock) froze the UI
     * with it. A hang here is a ctest timeout. */
    smssession_binding_set_button(s, SMS_TARGET_SWITCH(SMS_SW_RESET), SMS_PAD_BTN_LEFT_TRIGGER, NULL, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect(s, 0, 0, 1, "left trigger presses its binding");
    (void)smssession_gamepad_generation(s);
    (void)smssession_gamepad_effective_port(s, 0);
    printf("ok: session lock still free with a trigger held\n");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect(s, 0, 0, 0, "left trigger released");
    smssession_gamepad_capture_begin(s);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    {
        int btn = SMS_PAD_BTN_NONE, got = 0;
        for (i = 0; i < 100 && !got; i++) {
            got = smssession_gamepad_capture_poll(s, &btn);
            if (!got) sleep_ms(20);
        }
        if (got && btn == SMS_PAD_BTN_RIGHT_TRIGGER)
            printf("ok: map mode captures a trigger\n");
        else {
            printf("FAIL: map mode captures a trigger (%d)\n", btn);
            failures++;
        }
    }
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    smssession_bindings_reset(s);

    /* hot-unplug while held: the player is released, not left holding the
     * last direction forever */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, -30000);
    expect(s, ACTBIT(SMS_ACT_RIGHT) | ACTBIT(SMS_ACT_1) | ACTBIT(SMS_ACT_UP), 1, 0, "held before the unplug");
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    for (i = 0; i < 100 && smssession_gamepad_count(s) != baseline; i++)
        sleep_ms(20);
    if (smssession_gamepad_count(s) == baseline)
        printf("ok: the unplugged pad is gone\n");
    else {
        printf("FAIL: the backend kept the detached pad (%d, baseline %d)\n",
               smssession_gamepad_count(s), baseline);
        failures++;
    }
    expect(s, 0, 0, 0, "and its player (the D-pad, the stick, a button and Pause) is released");

    smssession_stop(s);
    smssession_free(s);

    persistence(&p);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
