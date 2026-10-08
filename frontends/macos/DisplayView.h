/*
 * The emulator display.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "smssession.h"

@interface SMSDisplayView : NSView
- (instancetype)initWithSession:(smssession *)session;
/* YES: the TV's pixel aspect -- 8:7 on NTSC, 1.386:1 on PAL -- (setting
 * "aspect" 0, the default); NO: square pixels. */
- (void)setTvAspect:(BOOL)tv;
- (void)setSmooth:(BOOL)smooth;
- (void)stop;
@end
