/*
 * SMSControllersWindow -- both Master System joypads on screen, held buttons
 * lit, the console's buttons, Map mode for rebinding any control to a key or
 * a gamepad button, and the gamepads' port assignments.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "smssession.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is only used the first
 * time, to set the transient-for relationship. */
void sms_controllers_window_toggle(GtkWindow *parent, smssession *session);
gboolean sms_controllers_window_is_visible(void);
