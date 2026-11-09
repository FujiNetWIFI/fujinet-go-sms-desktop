/*
 * bindings_test -- the remappable table and the keyboard translator, pure:
 * defaults, steal-and-describe, persistence through the settings store, and
 * names. No machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "smssession.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    char cfg[512], data[512], stolen[64], name[64];
    smssession_paths p;
    smssession *s;
    sms_binding b;

    test_tmpdir(cfg, sizeof cfg, "bcfg");
    test_tmpdir(data, sizeof data, "bdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = smssession_new(&p);
    if (!s) return 1;

    /* defaults */
    b = smssession_binding_get(s, SMS_TARGET_PORT(0, SMS_ACT_1));
    check(b.keysym == 'z' && b.button == SMS_PAD_BTN_SOUTH, "player 1 button 1 defaults to Z / South");
    b = smssession_binding_get(s, SMS_TARGET_PORT(0, SMS_ACT_2));
    check(b.keysym == 'x' && b.button == SMS_PAD_BTN_EAST, "player 1 button 2 defaults to X / East");
    b = smssession_binding_get(s, SMS_TARGET_PORT(0, SMS_ACT_UP));
    check(b.keysym == SMS_KEYSYM_UP && b.button == SMS_PAD_BTN_DPAD_UP,
          "player 1 Up defaults to Up / D-pad Up (the D-pad is an ordinary bindable button)");
    b = smssession_binding_get(s, SMS_TARGET_PORT(1, SMS_ACT_UP));
    check(b.keysym == 'i', "player 2 Up defaults to I");
    b = smssession_binding_get(s, SMS_TARGET_PORT(1, SMS_ACT_1));
    check(b.keysym == 'n', "player 2 button 1 defaults to N");
    b = smssession_binding_get(s, SMS_TARGET_SWITCH(SMS_SW_PAUSE));
    check(b.keysym == SMS_KEYSYM_RETURN && b.button == SMS_PAD_BTN_START, "Pause defaults to Return / Start");
    b = smssession_binding_get(s, SMS_TARGET_SWITCH(SMS_SW_RESET));
    check(b.keysym == SMS_KEYSYM_BACKSPACE && b.button == SMS_PAD_BTN_BACK,
          "the Reset button defaults to Backspace / Back");
    b = smssession_binding_get(s, SMS_TARGET_SYSACT(SMS_SYSACT_SOFT_RESET));
    check(b.keysym == SMS_KEYSYM_F3, "Soft Reset defaults to F3");
    check(smssession_target_for_key(s, 'Z') == SMS_TARGET_PORT(0, SMS_ACT_1), "lookup folds case");
    check(smssession_target_for_button(s, 1, SMS_PAD_BTN_SOUTH) == SMS_TARGET_PORT(1, SMS_ACT_1),
          "button lookup is scoped to the port");
    check(smssession_target_for_button(s, 0, SMS_PAD_BTN_START) == SMS_TARGET_SWITCH(SMS_SW_PAUSE),
          "Start is Pause");

    check(SMS_TARGET_COUNT == 17 && SMS_PLAYERS == 2, "17 targets: 12 contacts, Pause, Reset, three actions");
    for (int port = 0; port < SMS_PLAYERS; port++) {
        static const int dpad[4] = { SMS_PAD_BTN_DPAD_UP, SMS_PAD_BTN_DPAD_DOWN,
                                     SMS_PAD_BTN_DPAD_LEFT, SMS_PAD_BTN_DPAD_RIGHT };
        int good = 1;
        for (int act = 0; act < 4; act++)
            if (smssession_binding_get(s, SMS_TARGET_PORT(port, act)).button != dpad[act]
                || smssession_target_for_button(s, port, dpad[act]) != SMS_TARGET_PORT(port, act))
                good = 0;
        snprintf(name, sizeof name, "player %d: the four directions default to the D-pad's, per port", port + 1);
        check(good, name);
    }
    check(smssession_target_for_button(s, 7, SMS_PAD_BTN_DPAD_UP) == SMS_TARGET_PORT(0, SMS_ACT_UP)
          && smssession_target_for_button(s, -1, SMS_PAD_BTN_DPAD_UP) == SMS_TARGET_PORT(0, SMS_ACT_UP),
          "a port out of range is port 1 for a button lookup");
    b = smssession_binding_get(s, SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG));
    check(b.keysym == SMS_KEYSYM_ESCAPE && b.button == SMS_PAD_BTN_NONE, "Reset to CONFIG defaults to Escape, no pad button");
    b = smssession_binding_get(s, SMS_TARGET_SYSACT(SMS_SYSACT_DEBUG_STOP));
    check(b.keysym == 0 && b.button == SMS_PAD_BTN_NONE, "Debugger Stop has no default");
    check(smssession_binding_get(s, -1).keysym == 0 && smssession_binding_get(s, SMS_TARGET_COUNT).button == SMS_PAD_BTN_NONE,
          "an out-of-range target is unbound");

    /* steal */
    smssession_binding_set_key(s, SMS_TARGET_PORT(1, SMS_ACT_2), 'x', stolen, sizeof stolen);
    check(strstr(stolen, "Player 1: Button 2") != NULL, "rebinding X reports the holder it displaced");
    b = smssession_binding_get(s, SMS_TARGET_PORT(0, SMS_ACT_2));
    check(b.keysym == 0, "the old holder lost the key");
    check(smssession_target_for_key(s, 'x') == SMS_TARGET_PORT(1, SMS_ACT_2), "the new holder has it");

    /* names */
    smssession_keysym_name(SMS_KEYSYM_F12, name, sizeof name);
    check(strcmp(name, "F12") == 0, "F12 names itself");
    smssession_keysym_name('a', name, sizeof name);
    check(strcmp(name, "A") == 0, "letters name upper-case");
    check(strcmp(sms_pad_button_name(SMS_PAD_BTN_DPAD_LEFT), "D-pad Left") == 0, "pad button names");
    check(strcmp(sms_target_name(SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG)), "Reset to CONFIG") == 0,
          "system action names");
    check(strcmp(sms_target_name(SMS_TARGET_PORT(1, SMS_ACT_RIGHT)), "Player 2: Right") == 0, "target names");
    check(strcmp(sms_target_short_name(SMS_TARGET_SWITCH(SMS_SW_PAUSE)), "Pause") == 0, "short names");
    {
        static const char *const expect[SMS_TARGET_COUNT] = {
            "Player 1: Up", "Player 1: Down", "Player 1: Left", "Player 1: Right",
            "Player 1: Button 1", "Player 1: Button 2",
            "Player 2: Up", "Player 2: Down", "Player 2: Left", "Player 2: Right",
            "Player 2: Button 1", "Player 2: Button 2",
            "Pause", "Reset Button", "Reset to CONFIG", "Soft Reset", "Debugger Stop",
        };
        int good = 1;
        for (int tgt = 0; tgt < SMS_TARGET_COUNT; tgt++)
            if (strcmp(sms_target_name(tgt), expect[tgt]) != 0 || !sms_target_short_name(tgt)[0])
                good = 0;
        check(good && sms_target_name(-1)[0] == 0 && sms_target_name(SMS_TARGET_COUNT)[0] == 0,
              "all 17 target names are stable, and out-of-range ones are empty");
    }

    /* native key codes: evdev KEY_1 is 2, Windows scancode 0x02 is '1', macOS 0x12 is '1' */
    check(smssession_keysym_from_evdev(2) == '1', "evdev KEY_1 -> '1'");
    check(smssession_keysym_from_win_scancode(0x02, 0) == '1', "Windows scancode 02 -> '1'");
    check(smssession_keysym_from_macos_keycode(0x12) == '1', "macOS keycode 0x12 -> '1'");
    check(smssession_keysym_from_evdev(103) == SMS_KEYSYM_UP, "evdev KEY_UP -> Up");
    check(smssession_keysym_from_win_scancode(0x48, 1) == SMS_KEYSYM_UP, "Windows E0 48 -> Up");
    check(smssession_keysym_from_macos_keycode(0x7E) == SMS_KEYSYM_UP, "macOS 0x7E -> Up");

    /* persistence */
    smssession_free(s);
    s = smssession_new(&p);
    check(s && smssession_target_for_key(s, 'x') == SMS_TARGET_PORT(1, SMS_ACT_2), "the rebinding persisted");
    smssession_bindings_reset(s);
    check(smssession_target_for_key(s, 'x') == SMS_TARGET_PORT(0, SMS_ACT_2), "reset restores the defaults");
    smssession_free(s);

    s = smssession_new(&p);
    /* a button steals only within its port: D-pad Up on Player 1's button 1
     * takes it from Player 1's Up, not Player 2's; on Pause (machine-wide) it
     * takes it from both */
    smssession_bindings_reset(s);
    smssession_binding_set_button(s, SMS_TARGET_PORT(0, SMS_ACT_1), SMS_PAD_BTN_DPAD_UP, stolen, sizeof stolen);
    check(strstr(stolen, "Player 1: Up") != NULL && !strstr(stolen, "Player 2"), "the report names Player 1's Up only");
    check(smssession_binding_get(s, SMS_TARGET_PORT(0, SMS_ACT_UP)).button == SMS_PAD_BTN_NONE
          && smssession_binding_get(s, SMS_TARGET_PORT(1, SMS_ACT_UP)).button == SMS_PAD_BTN_DPAD_UP,
          "Player 1's Up lost the button, Player 2's kept it");
    check(smssession_target_for_button(s, 0, SMS_PAD_BTN_DPAD_UP) == SMS_TARGET_PORT(0, SMS_ACT_1)
          && smssession_target_for_button(s, 1, SMS_PAD_BTN_DPAD_UP) == SMS_TARGET_PORT(1, SMS_ACT_UP),
          "each port's pad resolves D-pad Up to its own target");
    smssession_binding_set_button(s, SMS_TARGET_SWITCH(SMS_SW_PAUSE), SMS_PAD_BTN_DPAD_UP, stolen, sizeof stolen);
    check(strstr(stolen, "Player 1: Button 1") && strstr(stolen, "Player 2: Up"),
          "a machine-wide target takes it from both ports");
    smssession_binding_set_button(s, SMS_TARGET_PORT(1, SMS_ACT_UP), SMS_PAD_BTN_NONE, NULL, 0);
    check(smssession_binding_get(s, SMS_TARGET_PORT(1, SMS_ACT_UP)).button == SMS_PAD_BTN_NONE
          && smssession_binding_get(s, SMS_TARGET_PORT(1, SMS_ACT_UP)).keysym == 'i',
          "unbinding a button leaves the key (Clear does both calls)");
    smssession_bindings_reset(s);
    smssession_free(s);

    /* old stored bindings: the D-pad used to be a hard-wired role that a
     * rebinding of the same button switched off, so a stored binding that
     * puts a D-pad button elsewhere must clear the direction now seeded on
     * it, in the same scope and only there */
    {
        static const struct {
            const char *stored;
            const char *what;
            int up_one, up_two;      /* expected buttons of Player 1's / 2's Up */
            int down_one;            /* ... and Player 1's Down */
        } cases[] = {
            { "4.b:11", "a stored Player 1 button 1 on D-pad Up clears Player 1's Up only",
              SMS_PAD_BTN_NONE, SMS_PAD_BTN_DPAD_UP, SMS_PAD_BTN_DPAD_DOWN },
            { "10.b:11", "a stored Player 2 button 1 on D-pad Up clears Player 2's Up only",
              SMS_PAD_BTN_DPAD_UP, SMS_PAD_BTN_NONE, SMS_PAD_BTN_DPAD_DOWN },
            { "12.b:11", "a stored Pause on D-pad Up clears both ports' Up",
              SMS_PAD_BTN_NONE, SMS_PAD_BTN_NONE, SMS_PAD_BTN_DPAD_DOWN },
            { "4.b:11 0.b:11", "a direction with a stored button of its own keeps it, duplicate or not",
              SMS_PAD_BTN_DPAD_UP, SMS_PAD_BTN_DPAD_UP, SMS_PAD_BTN_DPAD_DOWN },
            { "4.b:5 0.b:-1", "an unrelated stored button and a stored Clear change nothing else",
              SMS_PAD_BTN_NONE, SMS_PAD_BTN_DPAD_UP, SMS_PAD_BTN_DPAD_DOWN },
            { "4.b:-1 1.b:11", "Down stored on D-pad Up clears Up, and Down keeps what it stored",
              SMS_PAD_BTN_NONE, SMS_PAD_BTN_DPAD_UP, SMS_PAD_BTN_DPAD_UP },
            { "garbage 99.b:11 4.x:11 ", "garbage and out-of-range entries are skipped",
              SMS_PAD_BTN_DPAD_UP, SMS_PAD_BTN_DPAD_UP, SMS_PAD_BTN_DPAD_DOWN },
        };

        for (size_t num = 0; num < sizeof cases / sizeof cases[0]; num++) {
            smssession_set_str(s = smssession_new(&p), "bindings", cases[num].stored);
            smssession_free(s);
            s = smssession_new(&p);
            check(s && smssession_binding_get(s, SMS_TARGET_PORT(0, SMS_ACT_UP)).button == cases[num].up_one
                  && smssession_binding_get(s, SMS_TARGET_PORT(1, SMS_ACT_UP)).button == cases[num].up_two
                  && smssession_binding_get(s, SMS_TARGET_PORT(0, SMS_ACT_DOWN)).button == cases[num].down_one,
                  cases[num].what);
            smssession_free(s);
        }
        s = smssession_new(&p);
        smssession_bindings_reset(s);
        check(!smssession_get_str(s, "bindings", "x")[0], "Defaults writes an empty bindings setting");
        smssession_free(s);
        s = smssession_new(&p);
        check(smssession_binding_get(s, SMS_TARGET_PORT(0, SMS_ACT_UP)).button == SMS_PAD_BTN_DPAD_UP,
              "and the next run has the D-pad defaults");
        /* what a stolen default persists as: only the differences */
        smssession_binding_set_button(s, SMS_TARGET_PORT(0, SMS_ACT_1), SMS_PAD_BTN_DPAD_UP, NULL, 0);
        check(strstr(smssession_get_str(s, "bindings", ""), "0.b:-1") != NULL
              && strstr(smssession_get_str(s, "bindings", ""), "4.b:11") != NULL
              && !strstr(smssession_get_str(s, "bindings", ""), "6.b"),
              "a steal persists both ends and nothing of the other port");
        smssession_free(s);
        s = smssession_new(&p);
        check(smssession_binding_get(s, SMS_TARGET_PORT(0, SMS_ACT_UP)).button == SMS_PAD_BTN_NONE
              && smssession_binding_get(s, SMS_TARGET_PORT(0, SMS_ACT_1)).button == SMS_PAD_BTN_DPAD_UP,
              "and it reloads as made (the migration does not undo it)");
        smssession_bindings_reset(s);
        smssession_free(s);
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
