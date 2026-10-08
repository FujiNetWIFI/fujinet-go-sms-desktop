/*
 * smsdebug -- the debugger contract: a Z80 / 315-5124 debugger engine over
 * the core (core/debugger/) behind a C API the four native debugger windows
 * share. Same shape as the NES sibling's nesdebug.h, so the windows carry
 * over; the engine is this repository's own, the ColecoVision sibling's
 * Z80 engine grown into the NES one's feature set.
 *
 * The engine only engages while a window holds it attached:
 * smsdebug_attach() when the window opens -- which also stops the machine,
 * as on every sibling -- and smsdebug_detach() when it closes, which lets the
 * machine run on at full speed with no hook installed.
 *
 * Calls are safe from the UI thread at any time. Inspection is meant for the
 * stopped state; while running it answers with whatever is current.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMSDEBUG_H
#define SMSDEBUG_H

#include <stdint.h>

#include "smssession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lazily created; lives as long as the session. */
smsdebug *smsdebug_get(smssession *s);

/* ---- attach / stop / go ---------------------------------------------------- */
/* Engage the engine and stop at the next instruction. Idempotent. */
void smsdebug_attach(smsdebug *d);
/* Disengage and let the machine run. Breakpoints are kept for the next
 * attach. */
void smsdebug_detach(smsdebug *d);
int  smsdebug_is_attached(smsdebug *d);

int  smsdebug_is_stopped(smsdebug *d);
void smsdebug_stop(smsdebug *d);         /* break at the next instruction */
void smsdebug_resume(smsdebug *d);       /* "run" */
/* Why the machine last stopped ("stopped", "breakpoint: exec $0038",
 * "write $C000 = $12", "step", "frame", ...) and the PC, -1 if none.
 * Returns length. */
int  smsdebug_stop_reason(smsdebug *d, char *dst, int dstsz, int *address);
/* Bumped whenever a command ran or the machine stopped/resumed, so a window
 * knows when to refresh. */
unsigned smsdebug_generation(smsdebug *d);

/* ---- the prompt ------------------------------------------------------------ */
/* Run one command; "help" lists them: step [n], over, out, frame [n],
 * scanline [n], run, stop, runto <addr>, break <addr> [cond], bpr/bpw
 * <addr>[-<end>] [cond], bpin/bpout <port>[-<end>] [cond], delete <id|addr>,
 * enable/disable <id>, clear, breaks, print <expr>, regs, set <reg> <expr>,
 * mem <addr> [len], poke <addr> <val...>, vram <addr> [len], vpoke <addr>
 * <val...>, cram, vdp, sprites, psg, fm, io, cart, mailbox, label <addr>
 * <name>, labels [file], disasm [addr] [n], trace [on|off|n], save <kind>
 * <file>. Expressions: numbers ($C000, 0xC000, C000h, 49152), registers
 * (a f b c d e h l af bc de hl ix iy sp pc i r, primed af' ...), flags (cf
 * zf sf hf pf nf), [addr] byte and {addr} word reads, labels, vpos hpos
 * vcount hcount frame cycles, and in a breakpoint condition `value` and
 * `addr` for the access; C operators with C precedence.
 * Output text into dst; returns its length. */
int  smsdebug_command(smsdebug *d, const char *command, char *dst, int dstsz);
/* Completions for a prefix (commands, registers and labels), one per line.
 * Returns the count. */
int  smsdebug_completions(smsdebug *d, const char *prefix, char *dst, int dstsz);

/* ---- stepping shortcuts (the toolbar) -------------------------------------- */
void smsdebug_step(smsdebug *d);          /* F7: one instruction */
void smsdebug_step_over(smsdebug *d);     /* F8: over a CALL/RST/DJNZ/block op */
void smsdebug_step_out(smsdebug *d);      /* Shift+F8: to the RET */
void smsdebug_scanline(smsdebug *d, int n);
void smsdebug_frame(smsdebug *d, int n);
/* Run until PC == addr. */
void smsdebug_run_to(smsdebug *d, uint16_t addr);

/* ---- CPU --------------------------------------------------------------------- */
typedef struct {
    int pc, sp, af, bc, de, hl, ix, iy, wz;
    int af2, bc2, de2, hl2;
    int i, r, im, iff1, iff2, halted;
    int sf, zf, yf, hf, xf, pf, nf, cf;    /* the flags in F */
    int int_line, nmi_line;                /* the VDP's /INT and /NMI as the CPU sees them */
    uint64_t cycles;                       /* T-states since power-on */
    int vpos, hpos;                        /* where the beam is */
    int vcount, hcount;                    /* what $7E / the H counter would read */
    uint32_t frame;
} smsdebug_cpu;
void smsdebug_cpu_get(smsdebug *d, smsdebug_cpu *out);
typedef enum {
    SMS_REG_PC = 0, SMS_REG_SP, SMS_REG_AF, SMS_REG_BC, SMS_REG_DE, SMS_REG_HL,
    SMS_REG_IX, SMS_REG_IY, SMS_REG_AF2, SMS_REG_BC2, SMS_REG_DE2, SMS_REG_HL2,
    SMS_REG_I, SMS_REG_R, SMS_REG_IM, SMS_REG_IFF1, SMS_REG_IFF2,
    SMS_REG_A, SMS_REG_F, SMS_REG_B, SMS_REG_C, SMS_REG_D, SMS_REG_E,
    SMS_REG_H, SMS_REG_L,
    SMS_FLAG_S, SMS_FLAG_Z, SMS_FLAG_H, SMS_FLAG_PV, SMS_FLAG_N, SMS_FLAG_C,
    SMS_REG_COUNT
} smsdebug_reg;
void smsdebug_cpu_set(smsdebug *d, int reg, int value);

/* ---- VDP --------------------------------------------------------------------- */
typedef struct {
    uint8_t reg[11];              /* R0-R10 as written */
    uint8_t status;               /* the status register (flags not yet read) */
    uint16_t addr;                /* the address register (14 bits) */
    uint8_t code;                 /* the last command code (0-3) */
    uint8_t buffer;               /* the read-ahead buffer */
    int second_byte;              /* a control write is half done */
    uint8_t line_counter, hcounter;
    int mode;                     /* 0-3 (TMS9918) or 4 */
    const char *mode_name;        /* "Mode 4 (SMS)", "Graphics II", ... */
    int y_pixels;                 /* 192, 224, 240 */
    int kind_5246;                /* 315-5246 (SMS2) rather than 315-5124 */
    int is_pal;
    int display_on, vint_on, hint_on, sprites_16, sprites_zoom;
    int left_column_blank, hscroll_lock_top, vscroll_lock_right, sprite_shift;
    uint16_t name_base, sat_base, sprite_pattern_base;  /* mode 4 */
    uint16_t color_base, pattern_base;                   /* TMS modes */
    uint8_t scroll_x, scroll_y, backdrop;
    int vint_pending, hint_pending;
    int int_line, nmi_line, pause_held;
    int vpos, hpos, vcount;
    uint32_t frame;
    uint8_t cram[32];
    uint32_t cram_rgb[32];        /* through the VDP's own palette */
} smsdebug_vdp;
void smsdebug_vdp_get(smsdebug *d, smsdebug_vdp *out);
/* One line of decoded detail for VDP register reg (0-10): the bit fields by
 * name, or the table it points at. Returns length. */
int  smsdebug_vdp_describe_register(smsdebug *d, int reg, char *dst, int dstsz);

/* The VDP's views, as XRGB images. Returns 1 when drawn.
 *   NAMETABLE: mode 4: 256x224 (256x256 in the 224/240-line modes), the
 *              whole name table with the visible window outlined; TMS
 *              modes: 256x192 (240x192 text).
 *   TILES:     mode 4: 256x128, all 512 tiles under palette `palette` (0 the
 *              background palette, 1 the sprite palette); TMS modes: the
 *              pattern table, 256x64 (256x192 in Graphics II).
 *   SPRITES:   256x256, every sprite in the table where it sits, over the
 *              backdrop, with the visible area outlined.
 *   PALETTE:   256x32, the 32 CRAM entries as 16x16 swatches (background
 *              top row, sprites bottom); TMS modes: the 16 fixed colours. */
typedef enum { SMSDEBUG_VIEW_NAMETABLE = 0, SMSDEBUG_VIEW_TILES,
               SMSDEBUG_VIEW_SPRITES, SMSDEBUG_VIEW_PALETTE } smsdebug_view;
#define SMSDEBUG_VIEW_MAX_PIXELS (256 * 256)
int  smsdebug_vdp_view(smsdebug *d, int view, int palette, uint32_t *dst,
                       int *width, int *height);
/* One sprite table entry, for the sprite list (64 in mode 4, 32 in the TMS
 * modes, where `color` and `early_clock` apply). Returns the count. */
typedef struct { int y, x, tile, color, early_clock, visible; } smsdebug_sprite;
int  smsdebug_sprites_get(smsdebug *d, smsdebug_sprite out[64]);

/* ---- sound and I/O ------------------------------------------------------------ */
typedef struct {
    int tone_period[3], tone_volume[3];     /* PSG channels 0-2 (volume 0 loudest, 15 off) */
    int noise_mode, noise_rate, noise_volume;
    uint16_t lfsr;
    int fm_present;                         /* a YM2413 (Japan, Mark III FM unit) */
    uint8_t fm_regs[0x40];
    int psg_audible, fm_audible;            /* the mute matrix / FM unit control */
    uint8_t mem_ctrl;                       /* port $3E */
    uint8_t io_ctrl;                        /* port $3F */
    int cart_enabled, bios_enabled, ram_enabled, io_enabled;
    int bios_present;
    uint8_t bios_page[3];                   /* the 16K BIOS banks behind 0400/4000/8000 */
    uint8_t mapper[4];                      /* the RAM copy at $FFFC-$FFFF */
    uint8_t port_dc, port_dd;               /* what $DC / $DD read now */
    uint8_t pad[2];                         /* joypad bits held (SMS_PAD_* order) */
    int pause_held, reset_held;
    int japanese;                           /* TH reads 0 when driven (nationality) */
    const char *console_name;
} smsdebug_io;
void smsdebug_io_get(smsdebug *d, smsdebug_io *out);

/* ---- memory ------------------------------------------------------------------- */
/* CPU-bus reads without side effects. */
int  smsdebug_read(smsdebug *d, uint16_t addr, uint8_t *dst, int n);
/* A debugger edit: RAM takes it as a bus write ($FFFC-$FFFF also bank);
 * below $C000 it goes into the memory behind the address (the cartridge's
 * SRAM, its resident image or the mailbox arena), not onto the bus. */
void smsdebug_write(smsdebug *d, uint16_t addr, uint8_t value);
/* The 8K of console RAM ($C000-$DFFF). */
void smsdebug_ram_get(smsdebug *d, uint8_t out[8192]);
/* VRAM ($0000-$3FFF) and CRAM, read and written without touching the VDP's
 * address register or read buffer. */
int  smsdebug_vram_read(smsdebug *d, uint16_t addr, uint8_t *dst, int n);
int  smsdebug_vram_write(smsdebug *d, uint16_t addr, const uint8_t *src, int n);
void smsdebug_cram_write(smsdebug *d, int index, uint8_t value);

/* ---- disassembly -------------------------------------------------------------- */
typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;
    int length;
    char bytes[16];
    char label[48];
    char disasm[64];
    char comment[64];        /* a jump/call target's label, a port's name */
} smsdebug_line;
/* Up to `max` lines starting at `addr`. Returns the count; *pc_line is the
 * index of the PC's line in `out`, -1 when not among them. */
int  smsdebug_disassemble(smsdebug *d, uint16_t addr, smsdebug_line *out,
                          int max, int *pc_line);
/* The address `rows` disassembly rows before (negative) or after `addr` --
 * for scrolling, and for "Follow PC" (PC a third of the way down). Backwards
 * is a best guess, as on every Z80 debugger. */
int  smsdebug_row_address(smsdebug *d, uint16_t addr, int rows);
/* Address of a label, or -1. Label of an address into dst (may be empty);
 * labels from a banked symbol file are matched against the bank the
 * cartridge has mapped there now. */
int  smsdebug_label_address(smsdebug *d, const char *label);
int  smsdebug_address_label(smsdebug *d, uint16_t addr, char *dst, int dstsz);
int  smsdebug_set_label(smsdebug *d, uint16_t addr, const char *label);
/* Load a symbol file: WLA-DX .sym ([labels], "BB:AAAA name"), z88dk .map
 * ("name = $AAAA ; addr, ..."), SDCC .noi ("DEF name 0xAAAA") or a plain
 * "AAAA name" list. With path NULL, <cart>.sym/.map/.noi next to the opened
 * cartridge. Message into msg; returns labels loaded or -1. */
int  smsdebug_load_symbols(smsdebug *d, const char *path, char *msg, int msgsz);

/* ---- the cartridge ------------------------------------------------------------ */
typedef struct {
    int present, link_up, busy, direct, booted_game;
    int resident_client;     /* an opened FujiNet client (<= 32K) in CONFIG's place */
    int mode;                /* FN_MODE_*: 0 CONFIG (or a resident client), 1 game,
                                2 app (mailbox live) */
    const char *mode_name;
    int mapper;              /* SMSMAP_*, -1 for CONFIG */
    char mapper_name[32];
    uint8_t bank[6];
    int ram_enabled, ram_writable;
    uint32_t image_size, image_crc, ram_size;
    int claim;
    int16_t page_bank[48];   /* the 8K SRAM bank behind each 1K page, -1 = cart memory */
    int load_state, load_win, load_nwin, load_pct;
    int boot_state, boot_pct, boot_err;
    uint32_t boot_got, boot_total;
    uint8_t ackseq, status, last_error, reply_cmd;
    uint16_t rxlen;
    int bios_phase;
    uint8_t snoop_c000, snoop_3e, snoop_3f, snoop_vdp[11];
    uint32_t queue_depth;
} smsdebug_cart;
void smsdebug_cart_get(smsdebug *d, smsdebug_cart *out);
/* One line: "FujiNet cartridge: CONFIG, link up". */
int  smsdebug_cart_info(smsdebug *d, char *dst, int dstsz);

/* ---- breakpoints -------------------------------------------------------------- */
typedef enum {
    SMSDEBUG_BP_EXEC = 1, SMSDEBUG_BP_READ = 2, SMSDEBUG_BP_WRITE = 4,
    SMSDEBUG_BP_IN = 8, SMSDEBUG_BP_OUT = 16
} smsdebug_bp_type;
typedef struct {
    int id;
    int type;                /* smsdebug_bp_type bits */
    uint16_t start, end;     /* a port range for IN/OUT (0-255) */
    int enabled;
    unsigned hits;
    char condition[128];
} smsdebug_breakpoint;
/* Execute breakpoint at addr: added if absent, removed if present. Returns
 * 1 when one is now set. */
int  smsdebug_breakpoint_toggle(smsdebug *d, uint16_t addr);
int  smsdebug_breakpoint_check(smsdebug *d, uint16_t addr);
/* Returns the new id, or -1 if the condition does not parse. */
int  smsdebug_breakpoint_add(smsdebug *d, int type, uint16_t start, uint16_t end,
                             const char *condition);
void smsdebug_breakpoint_remove(smsdebug *d, int id);
void smsdebug_breakpoint_enable(smsdebug *d, int id, int enabled);
int  smsdebug_breakpoint_list(smsdebug *d, smsdebug_breakpoint *out, int max);
void smsdebug_breakpoint_clear(smsdebug *d);

/* ---- the trace ring ----------------------------------------------------------- */
typedef struct {
    uint16_t pc, sp, af, bc, de, hl;
    uint8_t len, bytes[4];
    uint64_t cycles;
    int16_t vpos, hpos;
} smsdebug_trace;
void smsdebug_trace_enable(smsdebug *d, int on);
int  smsdebug_trace_enabled(smsdebug *d);
/* Newest first; returns entries copied. */
int  smsdebug_trace_read(smsdebug *d, smsdebug_trace *out, int max);
void smsdebug_trace_clear(smsdebug *d);

/* ---- files -------------------------------------------------------------------- */
/* Save with the path a native file picker supplied. kind: "dis" (the
 * disassembly of $0000-$BFFF as mapped now), "ram" (console RAM), "vram",
 * "cram", "sram" (the cartridge's SRAM as far as the image fills it),
 * "arena" (the mailbox page), "regs" (CPU and VDP registers as text).
 * Message into msg; returns 0 or -1. */
int  smsdebug_save(smsdebug *d, const char *kind, const char *path,
                   char *msg, int msgsz);

#ifdef __cplusplus
}
#endif

#endif /* SMSDEBUG_H */
