/*
 * debug_test -- the debugger contract against a running machine: attach
 * stops it, registers, step / run-to / scanline / frame, disassembly with
 * the PC line and port names, breakpoints of every class (execute, memory
 * read and write, I/O in and out) with conditions, memory, VRAM and CRAM,
 * the VDP views, sound and I/O, the cartridge tab, built-in and loaded
 * symbols in every format, the trace ring, Save, the prompt and its
 * completion, the `old` variable, VRAM / CRAM write breakpoints, memory
 * search and the memory map, and detach lets it run again. The breakpoints survive a
 * power cycle.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "smssession.h"
#include "smsdebug.h"
#include "smsmemtools.h"
#include "smsvidtab.h"
#include "host.h"
#include "test_tmpdir.h"
#include "test_rom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_stopped(smsdebug *d, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (!!smsdebug_is_stopped(d) == want) return 1;
        sleep_ms(5); waited += 5;
    }
    return 0;
}

/* Resume and wait for the next stop. */
static int run_until_stop(smsdebug *d, int timeout_ms)
{
    unsigned g = smsdebug_generation(d);
    int waited = 0;
    smsdebug_resume(d);
    while (waited < timeout_ms) {
        if (smsdebug_is_stopped(d) && smsdebug_generation(d) != g) return 1;
        sleep_ms(2); waited += 2;
    }
    return 0;
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static int cpu_pc(smsdebug *d)
{
    smsdebug_cpu c;
    smsdebug_cpu_get(d, &c);
    return c.pc;
}

int main(void)
{
    char cfg[512], data[512], rom[700], out[8192], path[800], msg[256];
    smssession_paths p;
    smssession *s;
    smssession_start_opts o;
    smsdebug *d;
    smsdebug_cpu c, c2;
    smsdebug_line lines[48];
    int pc_line = -1, n, i, w, h;
    static uint32_t img[SMSDEBUG_VIEW_MAX_PIXELS];

    test_tmpdir(cfg, sizeof cfg, "dcfg");
    test_tmpdir(data, sizeof data, "ddata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    snprintf(rom, sizeof rom, "%s/counter.sms", cfg);
    test_rom_write(rom, TEST_ROM_COUNTER);

    s = smssession_new(&p);
    if (!s) return 1;
    smssession_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    o.cart_path = rom;
    check(smssession_start(s, &o) == 0, "session starts with the counter image");
    sleep_ms(300);

    d = smssession_debugger(s);
    check(d != NULL, "debugger handle");
    smsdebug_attach(d);
    check(smsdebug_is_attached(d), "attached");
    check(wait_stopped(d, 1, 3000), "attaching stops the machine");

    smsdebug_cpu_get(d, &c);
    check(c.pc >= 0x0006 && c.pc <= 0x000F, "the PC is in the counter loop");
    check(c.sp == 0xDFF0, "the stack pointer was set by the reset code");
    check(c.im == 1 && c.iff1 == 0, "IM 1, interrupts off");
    check(c.vpos >= 0 && c.vpos < 262 && c.hpos >= 0, "the beam position is reported");

    smsdebug_step(d);
    check(wait_stopped(d, 1, 2000), "stopped after a step");
    {
        int waited = 0;
        smsdebug_cpu_get(d, &c2);
        while (c2.pc == c.pc && waited < 2000) {
            sleep_ms(5); waited += 5;
            smsdebug_cpu_get(d, &c2);
        }
    }
    check(c2.pc != c.pc, "a step moves the PC");
    check(c2.cycles > c.cycles, "and the cycle count");

    /* disassembly around the PC */
    n = smsdebug_disassemble(d, (uint16_t)smsdebug_row_address(d, (uint16_t)c2.pc, -4), lines, 48, &pc_line);
    check(n > 8, "disassembly lines");
    check(pc_line >= 0 && lines[pc_line].address == c2.pc && lines[pc_line].is_pc, "the PC line is marked");
    n = smsdebug_disassemble(d, 0x0006, lines, 5, NULL);
    check(n == 5 && strcmp(lines[0].disasm, "IN A,($DC)") == 0, "the loop starts IN A,($DC)");
    check(lines[0].comment[0] != '\0', "and the port is named");
    check(strcmp(lines[3].disasm, "INC (HL)") == 0 && strcmp(lines[4].disasm, "JR $0006") == 0,
          "the loop disassembles as INC (HL) / JR");
    n = smsdebug_disassemble(d, 0x0066, lines, 1, NULL);
    check(n == 1 && strcmp(lines[0].label, "NMI_PAUSE") == 0, "the NMI vector is labelled");

    /* run to an address */
    smsdebug_run_to(d, 0x000E);
    check(wait_stopped(d, 1, 3000) && cpu_pc(d) == 0x000E, "run to $000E");

    /* an execute breakpoint hits */
    check(smsdebug_breakpoint_toggle(d, 0x000F) == 1, "breakpoint set on the JR");
    check(smsdebug_breakpoint_check(d, 0x000F), "and reported");
    check(run_until_stop(d, 3000), "the breakpoint hit");
    check(cpu_pc(d) == 0x000F, "stopped at the breakpoint's address");
    {
        char why[128]; int addr;
        smsdebug_stop_reason(d, why, sizeof why, &addr);
        check(strstr(why, "reakpoint") != NULL, "the stop reason says breakpoint");
    }
    n = smsdebug_disassemble(d, 0x000E, lines, 2, NULL);
    check(n == 2 && lines[1].address == 0x000F && lines[1].has_breakpoint, "the line shows the breakpoint");
    check(smsdebug_breakpoint_toggle(d, 0x000F) == 0, "toggled off");

    /* conditions */
    check(smsdebug_breakpoint_add(d, SMSDEBUG_BP_EXEC, 0x000E, 0x000E, "this bogus ((") == -1,
          "an unparsable condition is refused");
    {
        int id = smsdebug_breakpoint_add(d, SMSDEBUG_BP_EXEC, 0x000E, 0x000E, "hl == $1234");
        check(id > 0, "a conditional breakpoint is added");
        smsdebug_resume(d);
        sleep_ms(200);
        check(!smsdebug_is_stopped(d), "a false condition never stops");
        smsdebug_stop(d);
        check(wait_stopped(d, 1, 3000), "stop");
        smsdebug_breakpoint_remove(d, id);
        id = smsdebug_breakpoint_add(d, SMSDEBUG_BP_EXEC, 0x000E, 0x000E, "hl == $C010 && [$C010] == $40");
        check(id > 0 && run_until_stop(d, 5000), "a true condition stops");
        {
            uint8_t v = 0;
            smsdebug_read(d, 0xC010, &v, 1);
            check(cpu_pc(d) == 0x000E && v == 0x40, "with the memory the condition named");
        }
        smsdebug_breakpoint_clear(d);
    }

    /* memory and I/O breakpoints */
    {
        smsdebug_breakpoint list[8];
        int id = smsdebug_breakpoint_add(d, SMSDEBUG_BP_WRITE, 0xC010, 0xC010, "");
        check(id > 0, "write breakpoint added");
        check(smsdebug_breakpoint_list(d, list, 8) == 1 && list[0].type == SMSDEBUG_BP_WRITE, "and listed");
        check(run_until_stop(d, 3000) && cpu_pc(d) == 0x000F, "the write breakpoint hits after INC (HL)");
        smsdebug_breakpoint_enable(d, id, 0);
        check(smsdebug_breakpoint_list(d, list, 8) == 1 && !list[0].enabled && list[0].hits >= 1,
              "disabled, with its hit count");
        smsdebug_breakpoint_clear(d);

        smsdebug_breakpoint_add(d, SMSDEBUG_BP_READ, 0xC010, 0xC010, "");
        check(run_until_stop(d, 3000), "the read breakpoint hits");
        smsdebug_breakpoint_clear(d);

        smsdebug_breakpoint_add(d, SMSDEBUG_BP_IN, 0xDC, 0xDC, "");
        check(run_until_stop(d, 3000) && cpu_pc(d) == 0x0008, "the IN breakpoint hits after IN A,($DC)");
        smsdebug_breakpoint_clear(d);

        smsdebug_breakpoint_add(d, SMSDEBUG_BP_OUT, 0x00, 0xFF, "");
        smsdebug_resume(d);
        sleep_ms(150);
        check(!smsdebug_is_stopped(d), "the program never OUTs: the OUT breakpoint stays quiet");
        smsdebug_stop(d);
        wait_stopped(d, 1, 3000);
        smsdebug_breakpoint_clear(d);
        check(smsdebug_breakpoint_list(d, list, 8) == 0, "cleared");
    }

    /* memory: read, and debugger writes */
    {
        uint8_t ram[8192], v = 0, hdr[8];
        smsdebug_write(d, 0xC300, 0x5A);
        smsdebug_ram_get(d, ram);
        check(ram[0x300] == 0x5A, "a RAM write lands");
        smsdebug_read(d, 0x7FF0, hdr, 8);
        check(memcmp(hdr, "TMR SEGA", 8) == 0, "the header reads through the cartridge");
        smsdebug_write(d, 0x0020, 0xEA);
        smsdebug_read(d, 0x0020, &v, 1);
        check(v == 0xEA, "a debugger write reaches the cartridge's SRAM");
        smsdebug_write(d, 0x0020, 0xFF);
    }

    /* registers */
    smsdebug_cpu_set(d, SMS_REG_A, 0x42);
    smsdebug_cpu_set(d, SMS_FLAG_C, 1);
    smsdebug_cpu_set(d, SMS_REG_BC2, 0x1234);
    smsdebug_cpu_get(d, &c);
    check((c.af >> 8) == 0x42 && c.cf == 1, "registers and flags are editable");
    check(c.bc2 == 0x1234, "the shadow registers too");

    /* VRAM and CRAM */
    {
        static const uint8_t pat[4] = { 1, 2, 3, 4 };
        uint8_t back[4] = { 0 };
        smsdebug_vdp v;
        smsdebug_vram_write(d, 0x3800, pat, 4);
        smsdebug_vram_read(d, 0x3800, back, 4);
        check(memcmp(pat, back, 4) == 0, "VRAM round-trips");
        smsdebug_cram_write(d, 5, 0x3F);
        smsdebug_vdp_get(d, &v);
        check(v.cram[5] == 0x3F, "CRAM takes a write");
        check(v.mode_name && v.mode_name[0], "the VDP mode is named");
        check(smsdebug_vdp_describe_register(d, 1, out, sizeof out) > 0 && strstr(out, "R1"), "R1 is described");

        /* the views, sized for the mode the VDP is in */
        check(smsdebug_vdp_view(d, SMSDEBUG_VIEW_NAMETABLE, 0, img, &w, &h) && w >= 240 && h >= 192,
              "name table view");
        check(smsdebug_vdp_view(d, SMSDEBUG_VIEW_TILES, 0, img, &w, &h) && w == 256, "tiles view");
        check(smsdebug_vdp_view(d, SMSDEBUG_VIEW_SPRITES, 0, img, &w, &h) && w == 256 && h == 256, "sprites view");
        check(smsdebug_vdp_view(d, SMSDEBUG_VIEW_PALETTE, 0, img, &w, &h) && w == 256 && h == 32, "palette view");
        {
            smsdebug_sprite spr[64];
            int ns = smsdebug_sprites_get(d, spr);
            check(ns == (v.mode == 4 ? 64 : 32), "the sprite list matches the mode");
        }
    }

    /* sound and I/O */
    {
        smsdebug_io io;
        smsdebug_io_get(d, &io);
        check(io.cart_enabled && !io.bios_present && io.ram_enabled && io.io_enabled,
              "no BIOS; the cartridge, RAM and I/O chip enabled");
        check(io.port_dc == 0xFF, "nothing held: $DC reads $FF");
        check(!io.fm_present, "the export Master System has no YM2413");
        check(io.console_name && strstr(io.console_name, "Master System"), "the console is named");
    }

    /* the cartridge */
    {
        smsdebug_cart cart;
        smsdebug_cart_get(d, &cart);
        check(cart.present && cart.direct && cart.booted_game, "cartridge tab: an opened image");
        check(cart.mode == 1 && cart.image_size == 0x8000, "running as GAME, 32K");
        check(cart.page_bank[0] == 0, "page 0 is SRAM bank 0");
        check(smsdebug_cart_info(d, out, sizeof out) > 0 && strstr(out, "FujiNet"), "cart info line");
    }

    /* symbols: every format, and the built-in tables */
    check(smsdebug_label_address(d, "MAPPER_SLOT2") == 0xFFFF, "built-in mapper labels");
    check(smsdebug_label_address(d, "FN_ACKSEQ") == 0xB400, "built-in mailbox labels (from fuji_mailbox.h)");
    snprintf(path, sizeof path, "%s/t.sym", cfg);
    write_text(path, "; WLA-DX\n[labels]\n00:000e bump\n00:0006 loop\n");
    check(smsdebug_load_symbols(d, path, msg, sizeof msg) == 2, "a WLA-DX .sym loads");
    check(smsdebug_label_address(d, "bump") == 0x000E, "and its labels resolve");
    snprintf(path, sizeof path, "%s/t.map", cfg);
    write_text(path, "_counter                        = $C010 ; addr, public, , main, , main.c:3\n");
    check(smsdebug_load_symbols(d, path, msg, sizeof msg) == 1, "a z88dk .map loads");
    check(smsdebug_label_address(d, "_counter") == 0xC010, "and its labels resolve");
    snprintf(path, sizeof path, "%s/t.noi", cfg);
    write_text(path, "DEF _pad 0xC012\n");
    check(smsdebug_load_symbols(d, path, msg, sizeof msg) == 1, "an SDCC .noi loads");
    check(smsdebug_label_address(d, "_pad") == 0xC012, "and its labels resolve");
    snprintf(path, sizeof path, "%s/t.txt", cfg);
    write_text(path, "C011 nmis\n");
    check(smsdebug_load_symbols(d, path, msg, sizeof msg) == 1, "a plain list loads");
    smsdebug_address_label(d, 0xC011, out, sizeof out);
    check(strcmp(out, "nmis") == 0, "and the address finds its label");
    n = smsdebug_disassemble(d, 0x000E, lines, 1, NULL);
    check(n == 1 && strcmp(lines[0].label, "bump") == 0, "loaded labels show in the disassembly");

    /* the trace ring */
    {
        smsdebug_trace tr[16];
        char line[128];
        int branches = 0, in_range = 0;
        uint64_t serial;

        check(smsdebug_trace_count(d) == 0 && smsdebug_trace_read(d, tr, 16) == 0,
              "the trace is empty (and unallocated) until enabled");
        smsdebug_trace_enable(d, 1);
        serial = smsdebug_stop_serial(d);
        for (i = 0; i < 10; i++) {
            smsdebug_step(d);
            wait_stopped(d, 1, 2000);
        }
        check(smsdebug_stop_serial(d) >= serial + 10, "every stop bumps the stop serial");
        n = smsdebug_trace_read(d, tr, 16);
        check(n >= 10, "the trace holds the stepped instructions");
        check(smsdebug_trace_count(d) == n, "trace_count agrees");
        check(n > 0 && tr[0].pc >= 0x0006 && tr[0].pc <= 0x000F, "newest first, in the loop");
        check(n > 0 && tr[0].bank == 0, "an entry carries the ROM bank mapped at its pc");
        check(n > 0 && smsdebug_trace_read(d, tr, 1) == 1, "reading fewer than held");
        n = smsdebug_trace_read(d, tr, 16);
        for (i = 0; i < n; i++) {
            branches += smsdebug_trace_match(&tr[i], SMSDEBUG_TRACE_BRANCHES, 0, 0);
            in_range += smsdebug_trace_match(&tr[i], SMSDEBUG_TRACE_RANGE, 0x000F, 0x0006) &&
                        tr[i].pc >= 0x0006 && tr[i].pc <= 0x000F;
            check(smsdebug_trace_match(&tr[i], SMSDEBUG_TRACE_ALL, 0, 0), "ALL passes everything");
        }
        check(branches >= 1 && branches < n, "BRANCHES keeps the JR and drops the rest");
        check(in_range == n, "RANGE (given backwards) keeps the loop");
        check(!smsdebug_trace_match(&tr[0], SMSDEBUG_TRACE_RANGE, 0x4000, 0x5000), "RANGE drops others");
        smsdebug_trace_format(&tr[0], line, sizeof line);
        check(strstr(line, "AF=") != NULL && strstr(line, "SP=DFF0") != NULL && line[4] == ' ',
              "trace_format shows pc, instruction and registers");
        smsdebug_trace_enable(d, 0);
        smsdebug_trace_clear(d);
        check(smsdebug_trace_count(d) == 0, "Clear empties the ring");
    }

    /* the stop reason names its address; the stop serial moves with stops */
    {
        char why[128]; int addr = -1;
        uint64_t before = smsdebug_stop_serial(d);
        smsdebug_step(d);
        wait_stopped(d, 1, 2000);
        smsdebug_stop_reason(d, why, sizeof why, &addr);
        check(strstr(why, "step at $") != NULL && addr == cpu_pc(d), "the stop reason says where");
        check(smsdebug_stop_serial(d) > before, "stop_serial moved");
        smsdebug_resume(d);
        smsdebug_stop_reason(d, why, sizeof why, &addr);
        check(strcmp(why, "running") == 0 && addr == -1, "running has no address");
        smsdebug_stop(d);
        wait_stopped(d, 1, 3000);
        smsdebug_stop_reason(d, why, sizeof why, &addr);
        check(strstr(why, "stopped at $") != NULL, "a stop request is also located");
    }

    /* disabled exec breakpoints reach the gutter; Clear Hits */
    {
        smsdebug_breakpoint list[8];
        int id = smsdebug_breakpoint_add(d, SMSDEBUG_BP_EXEC, 0x000E, 0x000E, "");
        smsdebug_breakpoint_enable(d, id, 0);
        smsdebug_disassemble(d, 0x000E, lines, 1, NULL);
        check(!lines[0].has_breakpoint && lines[0].bp_disabled, "a disabled breakpoint is the grey dot");
        smsdebug_breakpoint_enable(d, id, 1);
        smsdebug_disassemble(d, 0x000E, lines, 1, NULL);
        check(lines[0].has_breakpoint && !lines[0].bp_disabled, "an enabled one is the red dot");
        smsdebug_breakpoint_clear(d);
        smsdebug_disassemble(d, 0x000E, lines, 1, NULL);
        check(!lines[0].has_breakpoint && !lines[0].bp_disabled, "and none is none");

        id = smsdebug_breakpoint_add(d, SMSDEBUG_BP_EXEC, 0x000F, 0x000F, "");
        check(run_until_stop(d, 3000), "an execute breakpoint hits again");
        check(smsdebug_breakpoint_list(d, list, 8) == 1 && list[0].hits >= 1, "it counted");
        smsdebug_breakpoint_clear_hits(d);
        check(smsdebug_breakpoint_list(d, list, 8) == 1 && list[0].hits == 0, "Clear Hits zeroes it");
        smsdebug_breakpoint_clear(d);
    }

    /* `old`: the byte before a write, though the hook runs after the store */
    {
        smsdebug_breakpoint list[8];
        uint8_t v = 0;
        char why[128];
        int id = smsdebug_breakpoint_add(d, SMSDEBUG_BP_WRITE, 0xC010, 0xC010,
                                         "value != old + 1 && value != 0");
        check(id > 0, "a condition may use old");
        smsdebug_resume(d);
        sleep_ms(200);
        check(!smsdebug_is_stopped(d), "the counter always writes old + 1 (or wraps): never stops");
        smsdebug_stop(d);
        wait_stopped(d, 1, 3000);
        smsdebug_breakpoint_remove(d, id);

        id = smsdebug_breakpoint_add(d, SMSDEBUG_BP_WRITE, 0xC010, 0xC010, "old == $FF");
        check(id > 0 && run_until_stop(d, 5000), "old == $FF stops on the wrap");
        smsdebug_read(d, 0xC010, &v, 1);
        check(v == 0, "the write that wrapped it to 0");
        smsdebug_breakpoint_remove(d, id);

        /* a poke while stopped is seen on release */
        id = smsdebug_breakpoint_add(d, SMSDEBUG_BP_WRITE, 0xC010, 0xC010, "old == $40");
        smsdebug_write(d, 0xC010, 0x40);
        check(id > 0 && run_until_stop(d, 5000), "a poke while stopped reseeds old");
        smsdebug_read(d, 0xC010, &v, 1);
        check(v == 0x41, "the next write was $40 -> $41");
        smsdebug_stop_reason(d, why, sizeof why, NULL);
        check(strstr(why, "wrote $C010 = $41 (was $40)") != NULL, "the reason says what it replaced");
        check(smsdebug_breakpoint_list(d, list, 8) == 1 && list[0].hits == 1, "one hit");
        smsdebug_breakpoint_clear(d);
    }

    /* VRAM and CRAM write breakpoints: a little program in RAM writes both */
    {
        static const uint8_t prog[] = {
            0x3E, 0x00, 0xD3, 0xBF, 0x3E, 0x40, 0xD3, 0xBF,   /* VDP address 0, write */
            0x3E, 0x77, 0xD3, 0xBE,                           /* VRAM[0] = $77 */
            0x3E, 0x05, 0xD3, 0xBF, 0x3E, 0xC0, 0xD3, 0xBF,   /* CRAM index 5 */
            0x3E, 0x2A, 0xD3, 0xBE,                           /* CRAM[5] = $2A */
            0x18, 0xFE,
        };
        smsdebug_breakpoint list[8];
        uint8_t byte = 0x11;
        int id;
        char why[128];

        for (i = 0; i < (int)sizeof prog; i++)
            smsdebug_write(d, (uint16_t)(0xC100 + i), prog[i]);
        smsdebug_breakpoint_clear(d);

        id = smsdebug_breakpoint_add(d, SMSDEBUG_BP_VRAM, 0x0000, 0x0000, "old == $11 && value == $77");
        check(id > 0, "a VRAM write breakpoint is added");
        check(smsdebug_breakpoint_list(d, list, 8) == 1 && list[0].type == SMSDEBUG_BP_VRAM, "and listed");
        smsdebug_vram_write(d, 0, &byte, 1);
        smsdebug_cpu_set(d, SMS_REG_PC, 0xC100);
        check(run_until_stop(d, 3000), "the VRAM write stops");
        smsdebug_stop_reason(d, why, sizeof why, NULL);
        check(strstr(why, "vram write $0000 = $77 (was $11)") != NULL, "the reason names the VRAM write");
        check(cpu_pc(d) == 0xC10C, "at the instruction after the OUT");
        smsdebug_vram_read(d, 0, &byte, 1);
        check(byte == 0x77, "and the store landed");
        check(smsdebug_breakpoint_list(d, list, 8) == 1 && list[0].hits == 1, "counted");
        smsdebug_breakpoint_remove(d, id);

        /* a false condition on old: never */
        byte = 0x11;
        smsdebug_vram_write(d, 0, &byte, 1);
        id = smsdebug_breakpoint_add(d, SMSDEBUG_BP_VRAM, 0x0000, 0x3FFF, "old == $99");
        smsdebug_cpu_set(d, SMS_REG_PC, 0xC100);
        smsdebug_resume(d);
        sleep_ms(150);
        check(!smsdebug_is_stopped(d), "a false VRAM condition never stops");
        smsdebug_stop(d);
        wait_stopped(d, 1, 3000);
        smsdebug_breakpoint_remove(d, id);

        /* CRAM, through the prompt */
        smsdebug_cram_write(d, 5, 0x07);
        smsdebug_command(d, "bpc 5 old == $07 && value == $2A", out, sizeof out);
        check(strstr(out, "breakpoint #") != NULL, "bpc adds a breakpoint");
        smsdebug_command(d, "bpc 40", out, sizeof out);
        check(strstr(out, "error") != NULL, "CRAM has 32 entries");
        smsdebug_command(d, "bpv $4000", out, sizeof out);
        check(strstr(out, "error") != NULL, "VRAM ends at $3FFF");
        smsdebug_command(d, "breaks", out, sizeof out);
        check(strstr(out, " c ") != NULL, "breaks lists it");
        smsdebug_cpu_set(d, SMS_REG_PC, 0xC100);
        check(run_until_stop(d, 3000), "the CRAM write stops");
        smsdebug_stop_reason(d, why, sizeof why, NULL);
        check(strstr(why, "cram write 5 = $2A (was $07)") != NULL && cpu_pc(d) == 0xC118, "named, after the OUT");
        smsdebug_command(d, "clear", out, sizeof out);
        smsdebug_command(d, "bpv $0000-$00FF value == $77", out, sizeof out);
        check(strstr(out, "breakpoint #") != NULL, "bpv adds one");
        smsdebug_breakpoint_list(d, list, 8);
        {
            char text[160];
            smsbp_describe(d, &list[0], text, sizeof text);
            check(strstr(text, "VRAM write  $0000-$00FF  if value == $77") != NULL, "and describes it");
        }
        smsdebug_command(d, "clear", out, sizeof out);
        smsdebug_cpu_set(d, SMS_REG_PC, 0x0006);
    }

    /* Access = read + write in one entry */
    {
        smsdebug_breakpoint list[8];
        char text[160], why[128];
        smsdebug_command(d, "bpa $C010", out, sizeof out);
        check(strstr(out, "breakpoint #") != NULL, "bpa adds an Access breakpoint");
        check(smsdebug_breakpoint_list(d, list, 8) == 1 && list[0].type == SMSDEBUG_BP_ACCESS, "READ|WRITE");
        smsbp_describe(d, &list[0], text, sizeof text);
        check(strncmp(text, "Access  $C010", 13) == 0, "described as Access");
        smsdebug_command(d, "breaks", out, sizeof out);
        check(strstr(out, " a ") != NULL, "listed as a");
        smsdebug_cpu_set(d, SMS_REG_PC, 0x0006);
        check(run_until_stop(d, 3000), "an access stops");
        smsdebug_stop_reason(d, why, sizeof why, NULL);
        check(strstr(why, "read $C010 = $") != NULL || strstr(why, "wrote $C010 = $") != NULL,
              "read or wrote, named");
        smsdebug_command(d, "clear", out, sizeof out);
    }

    /* symbols for a Symbols tab */
    {
        int idx[512], total = smsdebug_symbols_count(d), cnt, builtin;
        uint16_t addr = 0;
        char name[64];
        int bank = 0;
        check(total > 0, "built-in symbols exist");
        check(smsdebug_symbols_get(d, 0, &addr, name, sizeof name, &bank) == 1 && name[0], "symbols_get");
        check(smsdebug_symbols_get(d, total, &addr, name, sizeof name, &bank) == 0, "past the end");
        check(smsdebug_load_symbols(d, NULL, msg, sizeof msg) >= -1, "load_symbols tolerated");
        smsdebug_set_label(d, 0xC0DE, "My_Probe_Label");
        check(smsdebug_symbols_count(d) == total + 1 || smsdebug_symbols_count(d) >= total, "a label adds");
        cnt = smsdebug_symbols_filter(d, "probe_LABEL", idx, 512);
        check(cnt == 1, "filter is a case-insensitive substring");
        check(cnt == 1 && smsdebug_symbols_get(d, idx[0], &addr, name, sizeof name, &bank) &&
              addr == 0xC0DE && !strcmp(name, "My_Probe_Label"), "and finds the label");
        cnt = smsdebug_symbols_filter(d, "", idx, 512);
        check(cnt > 1, "an empty filter matches all");
        {
            uint16_t prev = 0, a2;
            int ordered = 1;
            for (i = 0; i < cnt; i++) {
                smsdebug_symbols_get(d, idx[i], &a2, name, sizeof name, &bank);
                ordered &= a2 >= prev;
                prev = a2;
            }
            check(ordered, "ordered by address");
        }
        smsdebug_clear_symbols(d);
        builtin = smsdebug_symbols_count(d);
        check(builtin > 0 && smsdebug_symbols_filter(d, "My_Probe_Label", idx, 512) == 0,
              "Clear Symbols drops loaded labels, keeps built-ins");
        check(smsdebug_label_address(d, "VDP_CTRL") >= 0 || builtin > 0, "built-ins remain");
    }

    /* memory map labels; the console io prints the map once */
    {
        smsdebug_region map[16];
        char label[64];
        int count = smsdebug_memory_map(d, map, 16);
        check(smsmem_region_name(map, count, 0xC123, label, sizeof label) > 0 && strstr(label, "RAM"),
              "region name of a RAM address");
        check(smsmem_region_name(map, count, 0xFFFE, label, sizeof label) > 0 && strstr(label, "Mapper"),
              "and of a mapper register");
        check(smsmem_region_name(map, 0, 0x1000, label, sizeof label) == 0 && !label[0], "none for an empty map");
        smsdebug_command(d, "io", out, sizeof out);
        check(strstr(out, "Memory map") != NULL && strstr(strstr(out, "Memory map") + 10, "Memory map") == NULL,
              "console io prints the memory map once");
    }

    /* memory search over the CPU's 64K */
    {
        static const uint8_t loop[] = { 0xDB, 0xDC, 0x32, 0x12 };
        uint8_t pat[SMSSEARCH_MAX], mask;
        uint16_t hit[16], found = 0;
        int cnt, plen, flags;

        cnt = smsdebug_search(d, loop, 4, 0xFF, 0, 0, 0xFFFF, hit, 16);
        check(cnt >= 1 && hit[0] == 0x0006, "search finds the loop at $0006");
        check(smsdebug_search(d, loop, 4, 0xFF, 0, 0, 0x0005, hit, 16) == 0, "`to` bounds the start: not before $0006");
        check(smsdebug_search(d, loop, 4, 0xFF, 0, 0x0006, 0x0006, hit, 16) == 1, "and the start itself is in");
        check(smsdebug_search(d, loop, 4, 0xFF, 0, 0x0007, 0x0006, hit, 16) == 1, "from > to swaps");
        {
            static const uint8_t high_nibble = 0xD0;
            check(smsdebug_search(d, &high_nibble, 1, 0xF0, 0, 0x0006, 0x0006, hit, 16) == 1, "a mask");
        }
        check(smssearch_parse(SMSSEARCH_TEXT, "tmr sega", pat, &plen, &mask, &flags) &&
              (flags & SMSDEBUG_SEARCH_NOCASE), "text parses");
        cnt = smsdebug_search(d, pat, plen, mask, flags, 0, 0xFFFF, hit, 16);
        {
            int at_7ff0 = 0;
            for (i = 0; i < cnt; i++)
                at_7ff0 |= hit[i] == 0x7FF0;
            check(at_7ff0, "case-insensitive text search finds the header signature");
        }
        check(smsdebug_search_next(d, loop, 4, 0xFF, 0, 0x0000, 0, &found) == 1 && found == 0x0006,
              "next from $0000 finds $0006");
        check(smsdebug_search_next(d, pat, plen, mask, flags, 0x7FF0, 0, &found) == 1,
              "next wraps past the end");
        check(smsdebug_search_next(d, loop, 4, 0xFF, 0, 0x0006, 1, &found) == 1 && found >= 0x0006,
              "previous from the first match wraps to the last");
        check(smsdebug_search_next(d, pat, plen, mask, flags, 0x7FF0, 1, &found) == 1 && found < 0x7FF0 + 1 + 0x8000,
              "previous finds one");
        {
            static const uint8_t none[] = { 0x12, 0x34, 0x56, 0x78, 0x9A };
            check(smsdebug_search_next(d, none, 5, 0xFF, 0, 0x0000, 0, &found) == 0, "no match is 0");
        }
    }

    /* the memory map: ordered, gap-free, $0000-$FFFF */
    {
        smsdebug_region map[16];
        int ok = 1;
        n = smsdebug_memory_map(d, map, 16);
        check(n >= 6, "the memory map has its regions");
        check(n > 0 && map[0].lo == 0x0000 && map[n - 1].hi == 0xFFFF, "from $0000 to $FFFF");
        for (i = 1; i < n; i++)
            ok &= map[i].lo == map[i - 1].hi + 1;
        for (i = 0; i < n; i++)
            ok &= map[i].lo <= map[i].hi && map[i].name[0] != '\0';
        check(ok, "contiguous, ordered, all named");
        for (i = 0; i < n; i++)
            if (map[i].lo == 0xC000)
                check(map[i].readable && map[i].writable && strstr(map[i].name, "RAM") != NULL, "console RAM at $C000");
        check(n > 0 && map[n - 1].lo == 0xFFFC && strstr(map[n - 1].name, "Mapper") != NULL,
              "the mapper registers close the map");
        check(n > 1 && map[0].readable && strstr(map[0].name, "cartridge") != NULL, "the cartridge is mapped at $0000");
        check(smsdebug_memory_map(d, map, 3) == 3, "the map honours max");
    }

    /* the watch list (process-wide), through the prompt */
    smswatch_clear();
    smsdebug_command(d, "watch $C010", out, sizeof out);
    check(strstr(out, "$C010") != NULL && smswatch_count() == 1, "watch adds");
    smsdebug_command(d, "find DB DC 32", out, sizeof out);
    check(strstr(out, "$0006") != NULL, "find lists the match");
    smsdebug_command(d, "find text TMR SEGA", out, sizeof out);
    check(strstr(out, "$7FF0") != NULL, "find text");
    smsdebug_command(d, "unwatch all", out, sizeof out);
    check(smswatch_count() == 0, "unwatch all");
    smsdebug_command(d, "cart", out, sizeof out);
    check(strstr(out, "file ") != NULL, "cart names the file");
    smsdebug_completions(d, "wat", out, sizeof out);
    check(strstr(out, "watch") != NULL, "watch completes");
    smsdebug_completions(d, "bp", out, sizeof out);
    check(strstr(out, "bpv") != NULL && strstr(out, "bpc") != NULL, "bpv / bpc complete");
    smsdebug_completions(d, "print ol", out, sizeof out);
    check(strstr(out, "old") != NULL, "old completes in expressions");

    /* the beam: a scanline, a frame */
    {
        smsdebug_cpu a, b;
        smsdebug_cpu_get(d, &a);
        smsdebug_scanline(d, 1);
        wait_stopped(d, 1, 3000);
        sleep_ms(20);
        smsdebug_cpu_get(d, &b);
        check(b.vpos == (a.vpos + 1) % 262, "scanline+1 advances the beam one line");
        smsdebug_frame(d, 1);
        wait_stopped(d, 1, 3000);
        sleep_ms(20);
        smsdebug_cpu_get(d, &a);
        check(a.frame == b.frame + 1, "frame+1 advances one frame");
    }

    /* Save */
    {
        static const struct { const char *kind; long size; } kinds[] = {
            { "ram", 8192 }, { "vram", 16384 }, { "cram", 32 }, { "sram", 0x8000 }, { "arena", 4096 },
        };
        for (i = 0; i < (int)(sizeof kinds / sizeof kinds[0]); i++) {
            char what[96];
            snprintf(path, sizeof path, "%s/save.%s", cfg, kinds[i].kind);
            snprintf(what, sizeof what, "save %s writes %ld bytes", kinds[i].kind, kinds[i].size);
            check(smsdebug_save(d, kinds[i].kind, path, msg, sizeof msg) == 0 &&
                  file_size(path) == kinds[i].size, what);
        }
        snprintf(path, sizeof path, "%s/save.regs", cfg);
        check(smsdebug_save(d, "regs", path, msg, sizeof msg) == 0 && file_size(path) > 100, "save regs");
        snprintf(path, sizeof path, "%s/save.dis", cfg);
        check(smsdebug_save(d, "dis", path, msg, sizeof msg) == 0 && file_size(path) > 1000, "save dis");
        check(smsdebug_save(d, "nonsense", path, msg, sizeof msg) == -1, "an unknown kind is refused");
    }

    /* the prompt */
    smsdebug_cpu_set(d, SMS_REG_A, 0x66);
    smsdebug_command(d, "print a", out, sizeof out);
    check(strstr(out, "66") != NULL, "print evaluates an expression");
    smsdebug_command(d, "mem $0006 4", out, sizeof out);
    check(strstr(out, "DB DC 32 12") != NULL, "mem dumps CPU memory");
    smsdebug_command(d, "label $000B setp", out, sizeof out);
    check(smsdebug_label_address(d, "setp") == 0x000B, "label sets one");
    smsdebug_command(d, "disasm $0006 2", out, sizeof out);
    check(strstr(out, "IN A,($DC)") != NULL, "disasm");
    smsdebug_command(d, "regs", out, sizeof out);
    check(strstr(out, "PC") != NULL && strstr(out, "SP") != NULL, "regs");
    smsdebug_command(d, "vdp", out, sizeof out);
    check(strstr(out, "R0") != NULL, "vdp");
    smsdebug_command(d, "psg", out, sizeof out);
    check(out[0] != '\0', "psg");
    smsdebug_command(d, "io", out, sizeof out);
    check(strstr(out, "3E") != NULL || strstr(out, "3e") != NULL, "io shows the memory control port");
    smsdebug_command(d, "cart", out, sizeof out);
    check(strstr(out, "FujiNet") != NULL || strstr(out, "GAME") != NULL || strstr(out, "game") != NULL, "cart");
    smsdebug_command(d, "break $000E", out, sizeof out);
    check(smsdebug_breakpoint_check(d, 0x000E), "break sets a breakpoint");
    smsdebug_command(d, "clear", out, sizeof out);
    check(!smsdebug_breakpoint_check(d, 0x000E), "clear removes them");
    smsdebug_command(d, "help", out, sizeof out);
    check(strstr(out, "runto") != NULL, "help lists the commands");
    smsdebug_completions(d, "ru", out, sizeof out);
    check(strstr(out, "run") != NULL && strstr(out, "runto") != NULL, "Tab completion");
    smsdebug_command(d, "frame", out, sizeof out);
    check(wait_stopped(d, 1, 3000), "frame stops again");

    /* the Video tab's controller against the live machine */
    {
        smsvid_tab *vt = smsvid_tab_new(d);
        const uint32_t *pic;
        int pw, ph, same = 1, bad = 0, checked = 0;
        smsvid_tab_set_zoom(vt, 1);
        smsvid_tab_set_overlays(vt, 0);
        /* let frames finish with the tab open: it asks the machine to capture them */
        smsdebug_resume(d);
        sleep_ms(300);
        smsdebug_stop(d);
        check(wait_stopped(d, 1, 3000), "stopped with the Video tab open");
        smsdebug_frame(d, 1);
        check(wait_stopped(d, 1, 3000), "stopped at a frame boundary");
        sleep_ms(20);
        smsvid_tab_set_view(vt, SMSVID_V_DISPLAY);
        check((smsvid_tab_refresh(vt, 1) & SMSVID_TAB_PICTURE) != 0, "Video tab refresh");
        pic = smsvid_tab_picture(vt, &pw, &ph);
        check(pic && pw == 268 && (ph == 224 || ph == 240), "Display is the whole frame");
        sms_host_lock();
        if (pic) {
            const sms_machine_t *m = sms_host_machine();
            for (int k = 0; k < pw * ph; k++)
                if ((pic[k] & 0xffffff) != (m->vdp.fb[k] & 0xffffff)) { same = 0; break; }
            if (m->vdp.vdp_mode == 4) {
                smsvid_tab_set_view(vt, SMSVID_V_DECODED);
                smsvid_tab_refresh(vt, 1);
                const uint32_t *dec = smsvid_tab_picture(vt, &pw, &ph);
                for (int y = 0; dec && y < ph; y += 3)
                    for (int x = 0; x < pw; x += 5) {
                        int row = -1;
                        for (int r = 0; r < m->vdp.fb_height; r++)
                            if (m->vdp.line_regs[r].valid && m->vdp.line_regs[r].line == y) { row = r; break; }
                        if (row < 0) continue;
                        checked++;
                        if ((dec[(size_t)y * pw + x] & 0xffffff) != (m->vdp.fb[(size_t)row * 268 + 2 + x] & 0xffffff))
                            bad++;
                    }
            }
        }
        sms_host_unlock();
        check(same, "Display equals the machine's framebuffer at a frame boundary");
        check(bad == 0, "Decoded agrees with the machine's own picture (mode 4)");
        printf("   (decoded samples compared: %d)\n", checked);
        {
            smsdebug_breakpoint bl[8];
            smsdebug_breakpoint_clear(d);
            smsvid_tab_set_view(vt, SMSVID_V_DISPLAY);
            smsvid_tab_refresh(vt, 1);
            smsvid_tab_hover(vt, 2, 20);
            if (smsvid_tab_pixel(vt) && smsvid_tab_pixel(vt)->active) {
                check(smsvid_tab_break_on_pixel(vt) >= 0, "Break on writes adds VRAM breakpoints");
                n = smsdebug_breakpoint_list(d, bl, 8);
                check(n >= 1 && (bl[0].type & SMSDEBUG_BP_VRAM), "...of the VRAM class");
            }
            check(smsvid_tab_break_on_swatch(vt, 3) >= 0, "Break on CRAM write adds a CRAM breakpoint");
            n = smsdebug_breakpoint_list(d, bl, 8);
            check(n >= 1 && (bl[n - 1].type & SMSDEBUG_BP_CRAM) && bl[n - 1].start == 3, "...on that entry");
            smsdebug_breakpoint_clear(d);
        }
        smsvid_tab_free(vt);
    }

    /* search needs a stopped machine */
    {
        static const uint8_t any[] = { 0xDB };
        uint16_t hit[2], found;
        smsdebug_resume(d);
        check(smsdebug_search(d, any, 1, 0xFF, 0, 0, 0xFFFF, hit, 2) == -1, "search refuses while running");
        check(smsdebug_search_next(d, any, 1, 0xFF, 0, 0, 0, &found) == -1, "and so does search_next");
        smsdebug_stop(d);
        check(wait_stopped(d, 1, 3000), "stopped again");
    }

    /* step over does not wedge the machine */
    smsdebug_step_over(d);
    check(wait_stopped(d, 1, 3000), "step over");

    /* detach: running again */
    smsdebug_detach(d);
    check(!smsdebug_is_attached(d), "detached");
    {
        uint8_t a = 0, b = 0;
        sleep_ms(100);
        smsdebug_attach(d);
        wait_stopped(d, 1, 3000);
        smsdebug_read(d, 0xC010, &a, 1);
        smsdebug_resume(d);
        sleep_ms(200);
        smsdebug_read(d, 0xC010, &b, 1);
        check(a != b, "the machine runs after resume");
        smsdebug_detach(d);
    }

    /* the debugger survives a power cycle */
    smsdebug_attach(d);
    smsdebug_breakpoint_toggle(d, 0x000E);
    check(smssession_reset_to_config(s) == 0, "reset to CONFIG with the debugger attached");
    check(smssession_load_cart(s, rom) == 0, "reopen the cartridge");
    /* the power cycle re-attaches: it stops on the new machine's first
     * instruction, and from there the breakpoint must still be set */
    check(wait_stopped(d, 1, 3000), "the new machine stops on attach");
    check(run_until_stop(d, 3000) && cpu_pc(d) == 0x000E, "the breakpoint survives the power cycle");
    smsdebug_breakpoint_clear(d);
    smsdebug_detach(d);

    /* attached before the session starts (a window opened first): the
     * machine stops as soon as it exists */
    smssession_stop(s);
    smsdebug_attach(d);
    o.cart_path = rom;
    check(smssession_start(s, &o) == 0, "restart with the debugger already attached");
    check(wait_stopped(d, 1, 3000), "the new machine stops in the debugger at once");
    smsdebug_detach(d);
    check(!smsdebug_is_stopped(d), "and runs when it detaches");

    smssession_stop(s);
    smssession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
