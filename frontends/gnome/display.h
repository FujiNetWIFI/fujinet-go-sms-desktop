/*
 * The emulator display: a GtkWidget that pulls frames from the session on
 * the compositor's own frame clock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "smssession.h"

#define SMS_TYPE_DISPLAY (sms_display_get_type())
G_DECLARE_FINAL_TYPE(SMSDisplay, sms_display, SMS, DISPLAY, GtkWidget)

GtkWidget *sms_display_new(smssession *session);
/* The pixels a television showed (8:7 on NTSC, about 1.386:1 on PAL), or
 * square pixels. */
void sms_display_set_tv_aspect(SMSDisplay *self, gboolean tv);
void sms_display_set_smooth(SMSDisplay *self, gboolean smooth);
