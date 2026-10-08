/*
 * The main application window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "smssession.h"

#define SMS_TYPE_WINDOW (sms_window_get_type())
G_DECLARE_FINAL_TYPE(SMSWindow, sms_window, SMS, WINDOW,
                     AdwApplicationWindow)

GtkWidget *sms_window_new(AdwApplication *app, smssession *session);
/* main.c: the icon name to use (the installed id, or the in-tree art). */
const char *sms_icon_name(void);
void sms_window_toast(SMSWindow *self, const char *text);
/* The display's shape: 0 = the television's pixel aspect, 1 = square. */
void sms_window_apply_aspect(SMSWindow *self, int aspect);
/* A dropped or command-line file: a BIOS is imported, a cartridge opened,
 * anything else copied to FujiNet's SD folder. */
void sms_window_load_media(SMSWindow *self, const char *path);
/* A system action (SMS_SYSACT_*) from any window's keys, with the main
 * window's feedback: Reset to CONFIG toasts, Debugger Stop opens the
 * debugger. */
void sms_window_run_sysaction(SMSWindow *self, int sysact);
/* Import BIOS: imports `path` and shows smssession_import_bios's message,
 * which always says what happened, over `over`. Returns the image's index,
 * or -1. */
int sms_show_bios_import(GtkWidget *over, smssession *session, const char *path);

/* The accent colour (SMSSESSION_ACCENT_RGB) as a CSS class every window
 * can use: ".sms-accent" paints a widget's background in it, and
 * ".sms-accent-text" its label. Installed once by the main window. */
void sms_install_accent_css(void);
