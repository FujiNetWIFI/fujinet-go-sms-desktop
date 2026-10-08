/*
 * debug_test -- the debugger contract against a running machine: attach
 * stops it, registers, step / run-to / scanline / frame, disassembly with
 * the PC line and port names, breakpoints of every class (execute, memory
 * read and write, I/O in and out) with conditions, memory, VRAM and CRAM,
 * the VDP views, sound and I/O, the cartridge tab, built-in and loaded
 * symbols in every format, the trace ring, Save, the prompt and its
 * completion, and detach lets it run again. The breakpoints survive a
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
        smsdebug_trace_enable(d, 1);
        for (i = 0; i < 10; i++) {
            smsdebug_step(d);
            wait_stopped(d, 1, 2000);
        }
        n = smsdebug_trace_read(d, tr, 16);
        check(n >= 10, "the trace holds the stepped instructions");
        check(n > 0 && tr[0].pc >= 0x0006 && tr[0].pc <= 0x000F, "newest first, in the loop");
        smsdebug_trace_enable(d, 0);
        smsdebug_trace_clear(d);
    }

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
