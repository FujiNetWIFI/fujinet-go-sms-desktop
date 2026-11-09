/*
 * The Controllers panel (F9 / Cmd+J): the Master System's console strip and its
 * two control pads drawn from the session's layout (smssession_controls_*),
 * the gamepads with their assignments, and Map mode for rebinding. Hidden at
 * start; a fixed-size floating NSPanel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "smssession.h"

/* The keys the app itself owns besides F9 (F11, F12): called with the keysym
 * of one typed with the panel key; returns YES when it took it. */
typedef BOOL (^SMSAppKeyHandler)(uint32_t keysym);

@interface SMSControllersWindow : NSObject
+ (void)setAppKeyHandler:(SMSAppKeyHandler)handler;
/* Show, or hide when showing. */
+ (void)toggleWithSession:(smssession *)session;
/* Show (and raise) it; Settings' Open Controllers. */
+ (void)showWithSession:(smssession *)session;
+ (BOOL)isVisible;
/* The bindings changed elsewhere (Settings' Reset Controls): repaint. */
+ (void)bindingsChanged;
@end
