/*
 * bindings -- private interface to core/src/bindings.c: the session seeds
 * the table at smssession_new (process-global storage, like gamepad_sdl.c's
 * pad table, because the machine is a process singleton and there is only
 * ever one session's settings store live at a time). smssession.h carries
 * the frontend-facing API.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMS_BINDINGS_H
#define SMS_BINDINGS_H

#include "smssession.h"

#ifdef __cplusplus
extern "C" {
#endif

struct smssession;

/* Reloads the table: every slot to its default, then overridden by whatever
 * "bindings" holds in s's settings store. */
void bindings_init(struct smssession *s);

#ifdef __cplusplus
}
#endif

#endif /* SMS_BINDINGS_H */
