/*
 * symbols_builtin.c -- the labels every session starts with: the Master
 * System's vectors, header and mapper registers, and the FujiNet
 * cartridge's mailbox and loader entry points.
 *
 * The mailbox labels are built from the staged fuji_mailbox.h's own macros
 * (the cartridge firmware's header, staged verbatim), so they cannot drift
 * from the protocol the cartridge speaks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "symbols.h"
#include "fuji_mailbox.h"

#define MB(off) ((uint16_t)(FN_ARENA_BASE + (off)))

void symtab_add_builtins(symtab *t)
{
    static const struct { uint16_t addr; const char *name; } fixed[] = {
        { 0x0000, "RESET" },
        { 0x0038, "IRQ" },
        { 0x0066, "NMI_PAUSE" },
        { 0x7ff0, "TMR_SEGA" },
        { 0x7ffa, "ROM_CHECKSUM" },
        { 0x7ffc, "ROM_PRODUCT" },
        { 0x7fff, "ROM_REGION_SIZE" },
        { 0xc000, "RAM" },
        { 0xdffc, "RAM_MAPPER_COPY" },
        { 0xfffc, "MAPPER_CTRL" },
        { 0xfffd, "MAPPER_SLOT0" },
        { 0xfffe, "MAPPER_SLOT1" },
        { 0xffff, "MAPPER_SLOT2" },
    };
    static const struct { uint16_t off; const char *name; } mailbox[] = {
        { FN_R_DATA,         "FN_REPLY" },
        { FN_R_ACKSEQ,       "FN_ACKSEQ" },
        { FN_R_STATUS,       "FN_STATUS" },
        { FN_R_ERR,          "FN_ERR" },
        { FN_R_REPLY_CMD,    "FN_REPLY_CMD" },
        { FN_R_RXLEN_LO,     "FN_RXLEN" },
        { FN_R_BOOT_STATE,   "FN_BOOT_STATE" },
        { FN_R_BOOT_PCT,     "FN_BOOT_PCT" },
        { FN_R_BOOT_ERR,     "FN_BOOT_ERR" },
        { FN_R_MAGIC0,       "FN_MAGIC" },
        { FN_R_PROTO_VER,    "FN_PROTO_VER" },
        { FN_R_SLICE_ECHO,   "FN_SLICE_ECHO" },
        { FN_R_LOAD_STATE,   "FN_LOAD_STATE" },
        { FN_R_LOAD_K,       "FN_LOAD_K" },
        { FN_R_LOAD_N,       "FN_LOAD_N" },
        { FN_R_LOAD_SEQ,     "FN_LOAD_SEQ" },
        { FN_R_LOAD_PCT,     "FN_LOAD_PCT" },
        { FN_R_MODE,         "FN_MODE" },
        { FN_R_MAPPER,       "FN_MAPPER" },
        { FN_R_LINK,         "FN_LINK" },
        { FN_R_BOOT_GOT0,    "FN_BOOT_GOT" },
        { FN_R_BOOT_TOT0,    "FN_BOOT_TOT" },
        { FN_R_HO_C000,      "FN_HO_C000" },
        { FN_R_HO_3E,        "FN_HO_3E" },
        { FN_R_HO_3F,        "FN_HO_3F" },
        { FN_R_HO_VDP,       "FN_HO_VDP" },
        { FN_R_HO_MIRROR,    "FN_HO_MIRROR" },
        { FN_R_HO_FILL,      "FN_HO_FILL" },
        { FN_H_REGSEL,       "FN_REGSEL" },
        { FN_H_REGSEL + FN_REG_DEVICE,   "FN_REG_DEVICE" },
        { FN_H_REGSEL + FN_REG_CMD,      "FN_REG_CMD" },
        { FN_H_REGSEL + FN_REG_NPARAM,   "FN_REG_NPARAM" },
        { FN_H_REGSEL + FN_REG_DATA_RST, "FN_REG_DATA_RST" },
        { FN_H_REGSEL + FN_REG_RXSLICE,  "FN_REG_RXSLICE" },
        { FN_H_REGSEL + FN_REG_SEQ,      "FN_REG_SEQ" },
        { FN_H_REGSEL + FN_REG_BOOTLOCK, "FN_REG_BOOTLOCK" },
        { FN_H_REGSEL + FN_REG_SLICE_ACK, "FN_REG_SLICE_ACK" },
        { FN_H_REGSEL + FN_HOT_CONFIG,   "FN_HOT_CONFIG" },
        { FN_H_REGSEL + FN_HOT_GO,       "FN_HOT_GO" },
        { FN_H_REGSEL + FN_HOT_SWAP,     "FN_HOT_SWAP" },
        { FN_H_REGDATA,      "FN_REGDATA" },
        { FN_H_DATA,         "FN_TXDATA" },
    };

    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        symtab_add(t, fixed[i].addr, SYM_BANK_ANY, fixed[i].name, 1);
    for (unsigned i = 0; i < sizeof mailbox / sizeof mailbox[0]; i++)
        symtab_add(t, MB(mailbox[i].off), SYM_BANK_ANY, mailbox[i].name, 1);
    symtab_add(t, FN_LOADER_BOOT, SYM_BANK_ANY, "FN_LOADER_BOOT", 1);
    symtab_add(t, FN_LOADER_CONFIG, SYM_BANK_ANY, "FN_LOADER_CONFIG", 1);
    symtab_add(t, FN_LOADWIN_BASE, SYM_BANK_ANY, "FN_LOADWIN", 1);
}
