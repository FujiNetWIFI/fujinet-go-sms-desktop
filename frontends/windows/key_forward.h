/*
 * Win32 key messages to X11/xkb keysyms, by hardware scan code through the
 * session's HID table -- the same physical-key route the other frontends
 * take, so a binding names the key whatever Shift or the layout is doing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>
#include <windows.h>

#include "smssession.h"

static inline uint32_t sms_keysym_from_msg(WPARAM vk, LPARAM lp)
{
    const unsigned scancode = (unsigned)((lp >> 16) & 0xff);
    const int extended = (lp >> 24) & 1;
    uint32_t ks = smssession_keysym_from_win_scancode(scancode, extended);
    if (ks) return ks;
    /* Fallbacks for keys the scan-code table has no usage for. */
    switch (vk) {
    case VK_UP: return SMS_KEYSYM_UP;
    case VK_DOWN: return SMS_KEYSYM_DOWN;
    case VK_LEFT: return SMS_KEYSYM_LEFT;
    case VK_RIGHT: return SMS_KEYSYM_RIGHT;
    case VK_ESCAPE: return SMS_KEYSYM_ESCAPE;
    case VK_RETURN: return SMS_KEYSYM_RETURN;
    case VK_SPACE: return SMS_KEYSYM_SPACE;
    case VK_TAB: return SMS_KEYSYM_TAB;
    case VK_BACK: return SMS_KEYSYM_BACKSPACE;
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return SMS_KEYSYM_F1 + (uint32_t)(vk - VK_F1);
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk - 'A' + 'a');
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (uint32_t)('0' + (vk - VK_NUMPAD0));
    return 0;
}

/* The app's own keys, never the console's: F9, F11, F12 and Ctrl+O (open). */
static inline int sms_app_key(WPARAM vkey)
{
    if (vkey == VK_F9 || vkey == VK_F11 || vkey == VK_F12) return 1;
    return (GetKeyState(VK_CONTROL) & 0x8000) && vkey == 'O';
}

/* A key press or release for the console from any of the app's windows: 1
 * when the console took it (it is bound). A system key (Alt held) that is
 * bound is the console's; the rest (Alt+F4, the menu mnemonics) stay
 * Windows'. An auto-repeat of a bound key is swallowed without being pressed
 * again. The session's own actions (Escape, F3) are the caller's. */
static inline int sms_forward_key(smssession *session, UINT msg, WPARAM vkey, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const uint32_t keysym = sms_keysym_from_msg(vkey, lparam);
    if (!keysym) return 0;
    if (!down)
        return smssession_key(session, keysym, 0) != 0;    /* always: a key let go after a shortcut must not stick */
    if (sms_app_key(vkey) || (msg == WM_SYSKEYDOWN && vkey == VK_F4)) return 0;
    /* Ctrl is the app's: a key pressed with it is a shortcut, never the console's */
    if ((GetKeyState(VK_CONTROL) & 0x8000) && vkey != VK_CONTROL && vkey != VK_LCONTROL && vkey != VK_RCONTROL)
        return 0;
    if (lparam & (1 << 30)) return smssession_target_for_key(session, keysym) >= 0;
    return smssession_key(session, keysym, 1) != 0;
}
