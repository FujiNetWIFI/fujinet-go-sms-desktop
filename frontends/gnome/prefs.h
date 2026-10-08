/*
 * Preferences dialog for the GNOME frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "smssession.h"

G_BEGIN_DECLS

typedef struct _SMSWindow SMSWindow;

/* Shows the preferences dialog. The analog switch, the volume and the aspect
 * apply live; the console, its BIOS, the FM options and the host options
 * (FujiNet, audio, gamepads) are read when the machine powers up, so the
 * dialog power-cycles the session on close if one of those changed. */
void sms_prefs_show(SMSWindow *parent, smssession *session,
                    void (*restart)(SMSWindow *parent));

G_END_DECLS
