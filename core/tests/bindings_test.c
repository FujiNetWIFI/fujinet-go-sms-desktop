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
    check(b.keysym == SMS_KEYSYM_UP && b.button == SMS_PAD_BTN_NONE,
          "player 1 Up defaults to Up (pads steer with the D-pad and stick, unbound)");
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

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
