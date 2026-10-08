/*
 * gamepad_map_test -- the gamepad layer end to end with an SDL3 virtual
 * gamepad: hot-plug pickup, the default mapping onto the joypad (South = 1,
 * East = 2, the D-pad and the left stick for directions) and the console's
 * buttons (Start = Pause, Back = the Reset button), and hot-unplug releasing
 * everything the pad held -- with no hardware and no human.
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

#define B(a) (1u << (a))

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
    expect(s, B(SMS_ACT_1), 0, 0, "South is button 1");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect(s, 0, 0, 0, "released");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    expect(s, B(SMS_ACT_2), 0, 0, "East is button 2");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
    expect(s, 0, 0, 0, "released");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, B(SMS_ACT_UP), 0, 0, "D-pad up");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    expect(s, B(SMS_ACT_LEFT), 0, 0, "D-pad left");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    expect(s, 0, 0, 0, "released");

    /* the stick past its threshold drives the D-pad too (a pad that reports
     * its D-pad as axes would otherwise drive nothing) */
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    expect(s, B(SMS_ACT_RIGHT) | B(SMS_ACT_DOWN), 0, 0, "the stick right and down");
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

    /* hot-unplug while held: the player is released, not left holding the
     * last direction forever */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, B(SMS_ACT_RIGHT) | B(SMS_ACT_1), 0, 0, "held before the unplug");
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
    expect(s, 0, 0, 0, "and its player is released");

    smssession_stop(s);
    smssession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
