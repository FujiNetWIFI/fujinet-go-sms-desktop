/*
 * hid_keys -- private interface to core/src/hid_keys.c, for bindings.c's
 * name tables. The native-code -> HID-usage translators the frontends call
 * are public instead, declared in smssession.h beside the keysym bands they
 * feed (SMSSESSION_KEYSYM_HID_BASE and friends).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMS_HID_KEYS_H
#define SMS_HID_KEYS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Human-readable name for a USB HID Keyboard usage ID -- "F16", "Keypad A",
 * "International 3" -- the names the family's siblings use (the
 * Intellivision port took them from jzIntv's event table). NULL for a usage
 * with no name, which is the caller's cue to synthesize one; callers must
 * handle that rather than print NULL. */
const char *sms_hid_usage_name(uint32_t usage);

/* The HID-usage keysym band: a key with no X11 keysym of its own (a media
 * key, an international key) is still bindable through this. */
#define SMSSESSION_KEYSYM_HID_BASE 0x10000u
#define SMSSESSION_HID_USAGE_MAX   0xE7u

/* USB HID keyboard usage IDs from each platform's native key code; 0 when
 * unknown. The public smssession_keysym_from_* wrappers chain these
 * through smssession_keysym_from_hid. */
uint32_t smssession_hid_from_win_scancode(unsigned scancode, int extended);
uint32_t smssession_hid_from_evdev(unsigned code);
uint32_t smssession_hid_from_macos_keycode(unsigned keycode);
uint32_t smssession_keysym_from_hid(uint32_t usage);

#ifdef __cplusplus
}
#endif

#endif /* SMS_HID_KEYS_H */
