/*
 * The Win32 Controllers window: both Master System joypads on screen, the
 * console's buttons, the Map row and the gamepads, in a fixed-size tool
 * window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "smssession.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void sms_controller_window_toggle(HWND parent, smssession *session);

/* Called when the gamepad set changed, so the per-port pad lines and the
 * gamepad rows refresh. */
void sms_controller_window_gamepads_changed(void);

/* Give the window first refusal on a message from the main loop. Returns 1
 * if consumed. */
int sms_controller_pretranslate(MSG *msg);
