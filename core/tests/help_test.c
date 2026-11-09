/*
 * help_test -- the Help covers what the app has: every Preferences setting
 * and choice, every machine, every ROM, every default key and gamepad button,
 * the app's shortcuts (and the Mac's), the two ports, the debugger's tabs
 * and prompt commands, and every context a Help button opens. Every miss is
 * listed before it fails.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smshelp.h"
#include "smssession.h"
#include "test_tmpdir.h"

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static const unsigned platforms[] = {
    SMSHELP_GNOME, SMSHELP_KDE, SMSHELP_WINDOWS, SMSHELP_MACOS
};
static const char *const platform_names[] = { "GNOME", "KDE", "Windows", "macOS" };

/* What the frontends' Help buttons and menus open. */
static const char *const contexts[] = {
    "main", "import-sd", "controllers", "fujilog", "prefs/Machine", "prefs/Input",
    "debugger/prompt", "debugger/cpu", "debugger/disassembly", "debugger/vdp",
    "debugger/io", "debugger/breakpoints", "debugger/cart", "debugger/fujinet",
};

/* The prompt's commands (core/include/smsdebug.h). */
static const char *const debug_commands[] = { "break", "bpr", "bpw", "bpin", "bpout", "print", "mem", "poke", "vram", "cram", "vdp", "sprites", "psg", "fm", "io", "cart", "mailbox", "label", "runto", "disasm", "trace", "save" };

/* The Preferences pages: what each one has, as the frontends label it. */
static const struct {
    const char *context;
    const char *labels[10];
} pages[] = {
    { "preferences", { "Console", "BIOS", "FM Sound Unit", "FM mutes the PSG", "Aspect", "Smooth scaling",
                       "Enable audio", "Volume", NULL } },
    { "prefs/Input", { "Enable gamepads", "Left stick drives the D-pad", "Gamepads", "Automatic",
                       "Player 1", "Player 2", "Reset Controls", "Open Controllers", NULL } },
    { "preferences", { "Enable FujiNet", "Use external FujiNet", "Address", NULL } },
};

/* The settings keys the pages write. */
static const char *const settings_keys[] = {
    "console", "fm_unit", "fm_unit_mutes_psg", "aspect", "smooth", "enable_audio", "volume", "enable_gamepad",
    "analog_joystick", "enable_fujinet", "fujinet_external", "fujinet_external_addr", "cart", "bindings",
    "pad_assign", "controllers_show_bindings",
};

/* The default gamepad buttons and what they do (the Gamepads topic). */
static const struct { const char *button, *does; } pad_rows[] = {
    { "D-pad", "Up, down, left, right" }, { "South (A)", "Button 1" }, { "East (B)", "Button 2" },
    { "Start", "Pause" }, { "Back", "Reset" },
};

/* The app's shortcuts, as a PC and as a Mac shows them. */
static const struct { const char *pc, *mac; } shortcuts[] = {
    { "Ctrl+O", "⌘O" }, { "Ctrl+R", "⌘R" }, { "Ctrl+,", "⌘," }, { "Ctrl+Shift+H", "⌘?" },
    { "F9", "F9" }, { "F11", "F11" }, { "F12", "F12" },
};

static int misses;

/* Lower case, every run of white space one space. */
static void
fold(const char *in, char *out, size_t outsz)
{
    size_t n = 0;
    int space = 0;
    for (; *in && n + 1 < outsz; in++) {
        unsigned char c = (unsigned char) *in;
        if (c == ' ' || c == '\n' || c == '\t') {
            space = 1;
            continue;
        }
        if (space && n)
            out[n++] = ' ';
        space = 0;
        out[n++] = (char) ((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[n] = 0;
}

static char section[1 << 18];
static char raw[1 << 18];

/* The folded text of a context's (or slug's) section on platform p; raw
 * keeps the lisms. */
static const char *
section_text(int p, const char *context)
{
    int t, b;
    section[0] = 0;
    raw[0] = 0;
    if (smshelp_find(context, &t, &b) == 0) {
        smshelp_plain_text(platforms[p], t, b, raw, sizeof raw);
        fold(raw, section, sizeof section);
    }
    return section;
}

static void
require(int p, const char *context, const char *what, const char *text)
{
    char want[512];
    fold(text, want, sizeof want);
    if (!strstr(section_text(p, context), want)) {
        fprintf(stderr, "MISSING (%s) %s: %s \"%s\"\n", platform_names[p], context, what, text);
        misses++;
    }
}

/* A line of the section (a table row: cells joined by " | ") with every one
 * of these, folded, in that order. */
static void
require_row(int p, const char *context, const char *a, const char *b, const char *c)
{
    char line[1024], fa[256], fb[256], fc[256];
    const char *s, *e;
    int ok = 0;

    section_text(p, context);
    fold(a, fa, sizeof fa);
    fold(b ? b : "", fb, sizeof fb);
    fold(c ? c : "", fc, sizeof fc);
    for (s = raw; *s && !ok; s = *e ? e + 1 : e) {
        char *x, *y;
        size_t n;
        e = strchr(s, '\n');
        if (!e)
            e = s + strlen(s);
        n = (size_t) (e - s) < sizeof line - 1 ? (size_t) (e - s) : sizeof line - 1;
        memcpy(line, s, n);
        line[n] = 0;
        fold(line, line, sizeof line);
        x = strstr(line, fa);
        if (!x)
            continue;
        y = b && *fb ? strstr(x + strlen(fa), fb) : x + strlen(fa);
        if (!y)
            continue;
        if (c && *fc && !strstr(y + (b && *fb ? strlen(fb) : 0), fc))
            continue;
        ok = 1;
    }
    if (!ok) {
        fprintf(stderr, "MISSING (%s) %s: a row \"%s\" | \"%s\" | \"%s\"\n", platform_names[p], context,
                a, b ? b : "", c ? c : "");
        misses++;
    }
}

static void
check_prefs(smssession *s, int p)
{
    char buf[128];

    for (size_t k = 0; k < sizeof pages / sizeof *pages; k++)
        for (int i = 0; pages[k].labels[i]; i++)
            require(p, pages[k].context, "label", pages[k].labels[i]);

    for (int c = 0; c < SMS_CONSOLE_COUNT; c++)
        require(p, "machines", "console", sms_console_name(c));
    for (size_t k = 0; k < sizeof settings_keys / sizeof *settings_keys; k++)
        require(p, "preferences", "setting", settings_keys[k]);
    (void) s;
}

/* Every default key and gamepad button, the console's buttons and the
 * shortcuts. */
static void
check_controls(smssession *s, int p)
{
    char name[64];

    CHECK(SMS_TARGET_COUNT == 17, "the Help says 17 controls");
    require(p, "changing-the-controls", "count", "17 in all");
    for (size_t row = 0; row < sizeof pad_rows / sizeof *pad_rows; row++)
        require_row(p, "gamepads", pad_rows[row].button, pad_rows[row].does, NULL);
    for (int port = 0; port < 2; port++)
        for (int a = 0; a < SMS_ACT_PER_PORT; a++) {
            sms_binding b = smssession_binding_get(s, SMS_TARGET_PORT(port, a));
            if (b.keysym) {
                smssession_keysym_name(b.keysym, name, sizeof name);
                require(p, "default-keys", "key", name);
            }
        }
    require_row(p, "default-keys", "Pause", "Return", NULL);
    require_row(p, "default-keys", "Reset", "Backspace", NULL);
    require_row(p, "default-keys", "Soft Reset", "F3", NULL);
    require_row(p, "default-keys", "Reset to CONFIG", "Escape", NULL);
    for (size_t k = 0; k < sizeof shortcuts / sizeof *shortcuts; k++)
        require(p, "default-keys", "shortcut", platforms[p] == SMSHELP_MACOS ? shortcuts[k].mac : shortcuts[k].pc);
}

/* The two ports, in the FujiNet topic. */
static void
check_ports(int p)
{
    char port[16];

    snprintf(port, sizeof port, "%d", SMSSESSION_BOIP_PORT);
    require(p, "fujinet", "BoIP port", port);
    snprintf(port, sizeof port, "%d", SMSSESSION_WEBUI_PORT);
    require(p, "fujinet", "web UI port", port);
    require(p, "an-external-fujinet", "default address", "127.0.0.1:11508");
}

/* Each command starts a `code` span in the prompt's section. */
static void
check_debugger(int p)
{
    int t, b;
    CHECK(smshelp_find("debugger/prompt", &t, &b) == 0, "no debugger/prompt");
    const smshelp_topic *tp = smshelp_topic_at(t);
    int end = smshelp_section_end(t, b);
    for (size_t k = 0; k < sizeof debug_commands / sizeof *debug_commands; k++) {
        const char *cmd = debug_commands[k];
        size_t n = strlen(cmd);
        int found = 0;
        for (int bl = b; bl < end && !found; bl++) {
            const smshelp_block *blk = &tp->blocks[bl];
            if (!(blk->platforms & platforms[p]))
                continue;
            for (int i = 0; i < blk->nspans && !found; i++) {
                const char *c = blk->spans[i].text;
                if (blk->spans[i].kind != SMSHELP_CODE)
                    continue;
                found = !strncmp(c, cmd, n) && (c[n] == 0 || c[n] == ' ' || c[n] == '/');
            }
        }
        if (!found) {
            fprintf(stderr, "MISSING (%s) debugger/prompt: command `%s`\n", platform_names[p], cmd);
            misses++;
        }
    }
    /* the eight tabs */
    require(p, "debugger", "tabs", "eight tabs");
}

int
main(void)
{
    char cfg[512], data[512];
    smssession_paths paths;
    smshelp_hit hits[16];
    smssession *s;
    int t, b;

    test_tmpdir(cfg, sizeof cfg, "hcfg");
    test_tmpdir(data, sizeof data, "hdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    s = smssession_new(&paths);
    CHECK(s, "new");
    CHECK(smshelp_topic_count() > 0, "no topics");

    for (size_t k = 0; k < sizeof contexts / sizeof *contexts; k++)
        if (smshelp_find(contexts[k], &t, &b) != 0) {
            fprintf(stderr, "MISSING context %s\n", contexts[k]);
            misses++;
        }
    for (int p = 0; p < 4; p++) {
        check_prefs(s, p);
        check_controls(s, p);
        check_ports(p);
        check_debugger(p);
    }

    /* lookup and search */
    CHECK(smshelp_find("#preferences", &t, &b) == 0 && b == -1, "#preferences");
    CHECK(smshelp_find("#the-settings-file", &t, &b) == 0 && b >= 0, "#the-settings-file");
    CHECK(smshelp_search(SMSHELP_GNOME, "cartridge", hits, 16) > 0, "search cartridge");
    CHECK(smshelp_search(SMSHELP_GNOME, "zzqxj", hits, 16) == 0, "search nonsense");
    int n = smshelp_search(SMSHELP_KDE, "breakpoints", hits, 16);
    CHECK(n > 0 && smshelp_find("debugger/breakpoints", &t, &b) == 0
          && hits[0].topic == t && hits[0].block == b, "breakpoints is the first hit");
    for (int i = 1; i < n; i++)
        CHECK(hits[i - 1].score >= hits[i].score, "hits best first");

    /* the Mac sees its own keys */
    {
        char text[4096];
        CHECK(smshelp_find("default-keys", &t, &b) == 0, "default-keys");
        smshelp_plain_text(SMSHELP_MACOS, t, b, text, sizeof text);
        CHECK(strstr(text, "⌘O") != NULL, "the Mac's Open Cartridge key");
        smshelp_plain_text(SMSHELP_GNOME, t, b, text, sizeof text);
        CHECK(strstr(text, "Ctrl+O") != NULL && !strstr(text, "⌘O"), "the PC's");
    }

    smssession_free(s);
    CHECK(misses == 0, "%d missing from data/help/help.md", misses);
    printf("help ok: %d topics\n", smshelp_topic_count());
    return 0;
}
