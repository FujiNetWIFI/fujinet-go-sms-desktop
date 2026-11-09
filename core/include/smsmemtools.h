/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the debugger windows only lay them out.
 *
 * A watch is text the user typed (an address, a label, an expression, or a
 * range lo-hi / lo..hi) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a label
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SMS_MEMTOOLS_H
#define SMS_MEMTOOLS_H

#include <stdint.h>

#include "smsdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "lo..hi", "lo-hi" (plain numbers or labels on both sides), or one
 * expression (an address, label, label+3, [ptr]). A bare token of 1-4 hex
 * digits ("C000") is hex unless it names a label; "$C000" and expressions
 * work as in the console. Returns 1 and fills lo and hi (inclusive,
 * lo <= hi), 0 if the text means nothing. */
int smssym_parse_range(smsdebug *d, const char *text, uint16_t *lo,
                       uint16_t *hi);

/* ---- watches ---- */
#define SMSWATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  smswatch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). */
int  smswatch_add(const char *text);
void smswatch_remove(int index);
void smswatch_clear(void);
const char *smswatch_text(int index);
unsigned smswatch_generation(void);
/* "score  $C010  $42 (66)". While the machine runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  smswatch_format(smsdebug *d, int index, char *dst, int dstsz);
/* The range a watch covers (for "show in Memory"). 1 if it resolves. */
int  smswatch_range(smsdebug *d, int index, uint16_t *lo, uint16_t *hi);

/* ---- memory search ---- */
enum { SMSSEARCH_HEX = 0, SMSSEARCH_TEXT };
#define SMSSEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, n, mask (0xFF) and flags for
 * smsdebug_search. Returns 1, or 0 for empty / malformed text. */
int smssearch_parse(int kind, const char *text, uint8_t *pat, int *n,
                    uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  $C010 (score)  if value != old"; VRAM breakpoints show the VRAM
 * address ("VRAM write  $1800-$1AFF"), CRAM ones the index ("CRAM write  5"). */
int smsbp_describe(smsdebug *d, const smsdebug_breakpoint *b, char *dst,
                   int dstsz);

/* ---- memory map ---- */
/* The name of the region of `map` (from smsdebug_memory_map) holding addr
 * ("Console RAM (8K)", "cartridge bank 3"), for labelling Memory rows. Fetch
 * the map once per refresh and look rows up in it. Returns the length, 0 and
 * "" when no region covers addr. A watch's range for "Show in Memory" is
 * smswatch_range above. */
int smsmem_region_name(const smsdebug_region *map, int count, uint16_t addr,
                       char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* SMS_MEMTOOLS_H */
