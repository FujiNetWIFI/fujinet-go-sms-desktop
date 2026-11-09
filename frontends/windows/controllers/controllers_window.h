/*
 * The Win32 Controllers window (F9): the Master System console strip and both
 * Control Pads drawn as a player sees them -- clickable, lit while held --
 * the gamepads with their assignments, and Map mode, in a fixed-size floating
 * tool window. Hidden at start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "smssession.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void sms_controller_window_toggle(HWND parent, smssession *session);

/* Show it (and bring it forward) if it is hidden: Preferences' Open button. */
void sms_controller_window_show(HWND parent, smssession *session);
int  sms_controller_window_visible(void);

/* The gamepad set or the console changed: refresh the pad rows and re-fetch
 * the layout. */
void sms_controller_window_gamepads_changed(void);

/* Map mode is armed: the next host key is a binding, not typing. The main
 * window gives its keys here first; 1 if consumed. */
int sms_controller_map_key(UINT msg, WPARAM vkey, LPARAM lparam);

/* Give the window first refusal on a message from the main loop. Returns 1
 * if consumed. */
int sms_controller_pretranslate(MSG *msg);
