/*
 * debug.c -- the session's side of the debugger: one engine per session
 * (core/debugger/debugger.c), told when a power cycle takes the machine
 * away and brings a new one up.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session_internal.h"
#include "debugger_internal.h"

smsdebug *smsdebug_get(smssession *s)
{
    if (!s)
        return NULL;
    if (!s->debugger)
        s->debugger = smsdebug_create();
    return s->debugger;
}

void smsdebug_before_power_cycle(struct smssession *s)
{
    if (s->debugger)
        smsdebug_power_down(s->debugger);
}

void smsdebug_after_power_cycle(struct smssession *s)
{
    if (!s->debugger)
        return;
    smsdebug_set_cart_path(s->debugger, s->cart_path);
    smsdebug_power_up(s->debugger);
}

void smsdebug_destroy(struct smssession *s)
{
    smsdebug_free(s->debugger);
    s->debugger = NULL;
}
