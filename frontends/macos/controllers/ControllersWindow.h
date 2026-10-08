/*
 * The controllers panel: an on-screen Master System joypad per player, the
 * console's buttons and the session's actions, the connected gamepads and
 * the Map row, in a fixed-size floating panel.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "smssession.h"

@interface SMSControllersWindow : NSWindowController <NSWindowDelegate>
+ (void)toggleWithSession:(smssession *)session;
+ (BOOL)isVisible;
@end
