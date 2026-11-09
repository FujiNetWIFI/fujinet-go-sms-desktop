/*
 * The Controllers window (F9): the console's top strip (Pause, and Reset or
 * Rapid where the model has one) and both Master System control pads drawn as
 * a player sees them -- clickable, lit with whatever the keyboard, a gamepad
 * or the mouse holds -- the app's own actions, the gamepads with their player
 * assignments, and Map mode for rebinding any control. Hidden at start; a
 * fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "smssession.h"

/* Shows the window, or hides it when it is showing. */
void smsControllersToggle(QWidget *parent, smssession *session);
/* Shows (raising) the window, for Preferences' "Open Controllers". */
void smsControllersShow(QWidget *parent, smssession *session);
bool smsControllersVisible();
/* Preferences closed or the session restarted: redraw. */
void smsControllersRefresh();
