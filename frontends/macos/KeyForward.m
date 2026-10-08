/*
 * KeyForward -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeyForward.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

#include "smssession.h"

uint32_t SMSKeysymFromEvent(NSEvent *event)
{
    const uint32_t ks = smssession_keysym_from_macos_keycode([event keyCode]);
    if (ks) return ks;

    switch ([event keyCode]) {
    case kVK_UpArrow:    return SMS_KEYSYM_UP;
    case kVK_DownArrow:  return SMS_KEYSYM_DOWN;
    case kVK_LeftArrow:  return SMS_KEYSYM_LEFT;
    case kVK_RightArrow: return SMS_KEYSYM_RIGHT;
    case kVK_Tab:        return SMS_KEYSYM_TAB;
    case kVK_Escape:     return SMS_KEYSYM_ESCAPE;
    case kVK_Return:     return SMS_KEYSYM_RETURN;
    case kVK_Space:      return SMS_KEYSYM_SPACE;
    case kVK_Delete:     return SMS_KEYSYM_BACKSPACE;
    default: break;
    }

    /* Anything the table does not know: the character typed. */
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 1) {
        const unichar c = [chars characterAtIndex:0];
        if (c >= 0x20 && c <= 0x7e) {
            if (c >= 'A' && c <= 'Z') return (uint32_t)(c - 'A' + 'a');
            return (uint32_t)c;
        }
        if (c >= NSF1FunctionKey && c <= NSF12FunctionKey)
            return SMS_KEYSYM_F1 + (uint32_t)(c - NSF1FunctionKey);
    }
    return 0;
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp. The key code
 * says which key; the corresponding flag says whether it went down. */
uint32_t SMSKeysymFromFlagsChange(NSEvent *event, int *down)
{
    const NSEventModifierFlags f = [event modifierFlags];
    switch ([event keyCode]) {
    case kVK_Shift:        *down = (f & NSEventModifierFlagShift) != 0;   return SMS_KEYSYM_LSHIFT;
    case kVK_RightShift:   *down = (f & NSEventModifierFlagShift) != 0;   return SMS_KEYSYM_RSHIFT;
    case kVK_Control:      *down = (f & NSEventModifierFlagControl) != 0; return SMS_KEYSYM_LCTRL;
    case kVK_RightControl: *down = (f & NSEventModifierFlagControl) != 0; return SMS_KEYSYM_RCTRL;
    /* Option is where a Mac keyboard puts the key a PC calls Alt. */
    case kVK_Option:       *down = (f & NSEventModifierFlagOption) != 0;  return SMS_KEYSYM_LALT;
    case kVK_RightOption:  *down = (f & NSEventModifierFlagOption) != 0;  return SMS_KEYSYM_RALT;
    default: *down = 0; return 0;
    }
}

NSColor *SMSAccentColor(void)
{
    return [NSColor colorWithSRGBRed:((SMSSESSION_ACCENT_RGB >> 16) & 0xff) / 255.0
                               green:((SMSSESSION_ACCENT_RGB >> 8) & 0xff) / 255.0
                                blue:(SMSSESSION_ACCENT_RGB & 0xff) / 255.0
                               alpha:1.0];
}
