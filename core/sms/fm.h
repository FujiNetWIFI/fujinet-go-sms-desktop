/* fm.h -- the YM2413 (fm_ym2413.cpp, an extern "C" shim over ymfm).
 *
 * Kept apart from sms_internal.h so the C++ shim never includes z80.h
 * (whose anonymous unions are C, not C++).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMS_FM_H
#define SMS_FM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_FM_INSTRUMENTS_SIZE 0x90   /* ym2413_instruments.bin */
#define SMS_FM_CLOCKS_PER_SAMPLE 72    /* ymfm OPLL: one sample per 72 clocks */

typedef struct sms_fm sms_fm_t;

sms_fm_t *sms_fm_create(const uint8_t *instruments /* 0x90 bytes or NULL */);
void sms_fm_free(sms_fm_t *f);
void sms_fm_reset(sms_fm_t *f);
void sms_fm_write(sms_fm_t *f, unsigned offset, uint8_t data);
/* One ymfm sample (call every SMS_FM_CLOCKS_PER_SAMPLE CPU clocks); the sum
 * of its outputs, which MAME routes together to the mono speaker. */
int32_t sms_fm_generate(sms_fm_t *f);
/* The registers as last written (for the debugger). */
void sms_fm_regs(const sms_fm_t *f, uint8_t regs[0x40]);

#ifdef __cplusplus
}
#endif

#endif /* SMS_FM_H */
