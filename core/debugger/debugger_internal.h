/*
 * debugger_internal -- the engine's side of the session glue and the
 * prompt's view of the engine. Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMS_DEBUGGER_INTERNAL_H
#define SMS_DEBUGGER_INTERNAL_H

#include "smsdebug.h"
#include "symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

smsdebug *smsdebug_create(void);
void smsdebug_free(smsdebug *d);
/* The machine is about to go away (a power cycle or a stop): resume and
 * unhook, remembering whether a window held the engine. */
void smsdebug_power_down(smsdebug *d);
/* A new machine is up: hook it again if a window held the engine (which
 * stops it at its first instruction, so a power cycle can be debugged from
 * the reset vector). */
void smsdebug_power_up(smsdebug *d);
/* The opened cartridge, for symbols next to it ("" for CONFIG). */
void smsdebug_set_cart_path(smsdebug *d, const char *path);

/* For the prompt (prompt.c). */
symtab *smsdebug_symbols(smsdebug *d);
/* Evaluate an expression in the machine's context; 0 or -1 with err. */
int smsdebug_eval(smsdebug *d, const char *text, long *out, char *err, int errsz);
/* The 16K ROM bank mapped at addr now, or SYM_BANK_ANY. */
int smsdebug_bank_at(smsdebug *d, uint16_t addr);
/* Register names the evaluator knows, for completion. */
extern const char *const smsdebug_reg_names[];

#ifdef __cplusplus
}
#endif

#endif /* SMS_DEBUGGER_INTERNAL_H */
