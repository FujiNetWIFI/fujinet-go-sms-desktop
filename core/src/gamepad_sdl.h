/*
 * gamepad_sdl -- private interface: the pure helpers unit-tested without
 * hardware, and the start/stop the session calls.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMS_GAMEPAD_SDL_H
#define SMS_GAMEPAD_SDL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direction bits a stick or D-pad resolves to. */
#define SMS_DIR_UP    1
#define SMS_DIR_DOWN  2
#define SMS_DIR_LEFT  4
#define SMS_DIR_RIGHT 8

/* An analog stick engages a direction at 0.35 of full deflection and lets go
 * below 0.15: low enough that a worn stick still registers, high enough that
 * resting drift does not, and a gap wide enough that jitter across one
 * threshold cannot flutter. */
#define SMS_STICK_ENTER 0.35f
#define SMS_STICK_EXIT  0.15f

/* The stick's (or the D-pad's) direction bits as the joypad's contacts, a
 * mask over sms_action. Opposite directions cancel, so a stick pushed both
 * ways holds neither. Pure. */
unsigned sms_acts_from_dir(int dir);

/* An analog stick as a four-way switch with hysteresis: an axis engages
 * past `enter` and releases below `exit` (both 0..1 of full deflection),
 * so a thumb resting near the threshold cannot flutter. `prev` is the
 * previous result. Pure. */
int sms_dir_from_stick(float x, float y, int prev, float enter, float exit);

/* Which pad drives `port`: an explicit assignment wins; among the rest,
 * connection order (first pad to port 0, second to port 1). `assign[i]` is
 * pad i's assignment (-1 = automatic). Returns the pad index or -1. Pure. */
int sms_pad_for_port(const int *assign, int npads, int port);

/* SDL3's SDL_GamepadButton (a uint8_t) to the public numbering, and back.
 * Explicit tables, never a cast, so an SDL reordering cannot desync them. */
int sms_pad_button_from_sdl(uint8_t button);
int sms_sdl_button_from_pad(int button);

#ifdef __cplusplus
}
#endif

#endif /* SMS_GAMEPAD_SDL_H */
