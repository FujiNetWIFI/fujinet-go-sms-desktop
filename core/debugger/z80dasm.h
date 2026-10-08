/*
 * Z80 disassembler (fresh implementation, from the ColecoVision sibling;
 * see z80dasm.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef Z80DASM_H
#define Z80DASM_H

#include <stdint.h>

#define Z80D_JUMP     0x01
#define Z80D_CALL     0x02   /* CALL and RST */
#define Z80D_RET      0x04
#define Z80D_COND     0x08
#define Z80D_RELATIVE 0x10
#define Z80D_BLOCK    0x20   /* LDIR/CPIR/INIR/OTIR family */
#define Z80D_HALT     0x40

/* One decoded instruction. */
typedef struct {
    uint16_t addr;
    uint8_t len; /* 1..4 */
    uint8_t bytes[4];
    char text[32];
    uint16_t target;   /* jump/call destination when flags say so */
    uint8_t flags;     /* Z80D_* */
} z80d_insn;

/* Decodes the instruction at addr from code[0..3]; returns its length. */
int z80_disassemble(z80d_insn *out, uint16_t addr, const uint8_t code[4]);

#endif /* Z80DASM_H */
