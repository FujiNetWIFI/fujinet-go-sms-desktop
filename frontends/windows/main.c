/*
 * FujiNet Go SMS -- the Windows (Win32 + GDI) frontend.
 *
 * No toolkit: a plain window, a menu bar, a status bar and StretchDIBits.
 * That is enough for a 268x240 framebuffer, and it keeps the artifact a
 * folder you copy rather than a runtime hunt.
 *
 * Two Windows-specific things are load bearing:
 *
 *   DwmFlush() on a present thread is this platform's frame clock. There is
 *   no GdkFrameClock here, and a plain timer would beat against the panel.
 *
 *   WM_ACTIVATE releases every held key. Alt-tabbing away mid-jump and coming
 *   back to a character walking into a wall is the classic symptom of not
 *   doing this, and Windows is where it happens most, because the WM eats the
 *   key-up.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smssession.h"
#include "controllers/controller_window.h"
#include "debugger/dbg_window.h"
#include "key_forward.h"
#include "resource.h"

#define WIN_CLASS "FujiNetGoSMS"
#define APP_TITLE "FujiNet Go SMS"

/* How long a gamepad connect/disconnect notice stays in the status bar. */
#define PAD_NOTICE_MS 4000

/* How long a menu click holds Pause or the Reset button down: the console
 * samples its buttons once a frame, so a press and release in the same
 * instant would fall between two frames and never be seen. */
#define TAP_MS 100

/* The status bar's first part: the FujiNet link dot. */
#define DOT_PART_W 22

static smssession *g_session;
static HWND g_hwnd;
static HWND g_statusbar;
static uint32_t *g_fb;
static int g_fb_height;
static uint64_t g_serial;
static BITMAPINFO g_bmi;
static CRITICAL_SECTION g_fb_lock;
static volatile LONG g_running = 1;
static HANDLE g_present_thread;
static int g_aspect = 1, g_smooth = 0, g_fullscreen = 0;   /* TV pixel aspect on */
static WINDOWPLACEMENT g_placement;
static int g_sysact_down[SMS_SYSACT_COUNT];
static unsigned g_pad_generation;
static char g_pad_notice[160];
static DWORD g_pad_notice_until;
static int g_tap_target = -1;    /* the console button a menu click is holding */
static HBRUSH g_accent_brush;

/* ---- the present thread: this platform's frame clock ---------------------- */

static DWORD WINAPI present_thread(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        LARGE_INTEGER t, f;
        int h = 0;
        /* Blocks until the compositor's next vblank; falls through at once
         * if the DWM is off, and the core's wall-clock pacing takes over --
         * which is the whole reason notify_vsync is advisory. */
        if (FAILED(DwmFlush())) Sleep(8);
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        smssession_notify_vsync(g_session,
                                (int64_t)(t.QuadPart * 1000000000LL / f.QuadPart));
        EnterCriticalSection(&g_fb_lock);
        if (smssession_copy_frame(g_session, g_fb, &h, &g_serial)) {
            g_fb_height = h;
            LeaveCriticalSection(&g_fb_lock);
            InvalidateRect(g_hwnd, NULL, FALSE);
        } else {
            LeaveCriticalSection(&g_fb_lock);
        }
    }
    return 0;
}

/* ---- painting ------------------------------------------------------------- */

static int statusbar_height(void)
{
    RECT r;
    if (!g_statusbar || !IsWindowVisible(g_statusbar)) return 0;
    GetWindowRect(g_statusbar, &r);
    return r.bottom - r.top;
}

/* The width of one console pixel over its height on a television: 8:7 on
 * NTSC; on PAL the VDP's 5.32 MHz dot clock against the 14.75 MHz
 * square-pixel rate, about 1.386. */
static double tv_pixel_aspect(void)
{
    return smssession_refresh_rate(g_session) == 50 ? 14.75 / 5.3203424 / 2.0 : 8.0 / 7.0;
}

static void paint(HDC dc)
{
    RECT rc;
    double want, w, h, sw, sh;
    int fbh;

    GetClientRect(g_hwnd, &rc);
    rc.bottom -= statusbar_height();
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (w <= 0 || h <= 0) return;

    EnterCriticalSection(&g_fb_lock);
    fbh = g_fb_height;
    if (fbh <= 0) { LeaveCriticalSection(&g_fb_lock); return; }

    /* A television drew each Master System pixel wider than it is tall;
     * "square" shows the framebuffer exactly. */
    want = (double)SMSSESSION_FB_WIDTH / (double)fbh;
    if (g_aspect) want *= tv_pixel_aspect();
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    /* The session's pixels are 0x00RRGGBB: exactly a 32-bit BI_RGB DIB. */
    g_bmi.bmiHeader.biHeight = -fbh;   /* top-down */
    SetStretchBltMode(dc, g_smooth ? HALFTONE : COLORONCOLOR);
    StretchDIBits(dc, (int)((w - sw) / 2), (int)((h - sh) / 2), (int)sw, (int)sh,
                  0, 0, SMSSESSION_FB_WIDTH, fbh, g_fb, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_fb_lock);
}

/* ---- helpers -------------------------------------------------------------- */

static void update_status(void);

/* Power-cycle with the current settings: the session decides whether that
 * is a power cycle of the console alone or a restart of FujiNet, audio and
 * the gamepads with it. */
static void restart_session(void)
{
    if (smssession_restart(g_session) != 0)
        MessageBoxA(g_hwnd, smssession_last_error(g_session), "Could not start",
                    MB_ICONWARNING | MB_OK);
    update_status();
}

static void run_sysaction(int sa)
{
    switch (sa) {
    case SMS_SYSACT_RESET_CONFIG:
    case SMS_SYSACT_SOFT_RESET:
        smssession_sysaction(g_session, sa);
        break;
    case SMS_SYSACT_DEBUG_STOP:
        /* Showing the debugger attaches it, and attaching stops the machine. */
        sms_debugger_show(g_hwnd, g_session);
        break;
    default: break;
    }
}

/* Hold a console button for a few frames, as a finger would (Pause and the
 * Reset button in the Machine menu). */
static void tap_button(int target)
{
    if (g_tap_target >= 0) smssession_press(g_session, g_tap_target, 0);
    g_tap_target = target;
    smssession_press(g_session, target, 1);
    SetTimer(g_hwnd, IDT_TAP, TAP_MS, NULL);
}

static void tap_release(void)
{
    KillTimer(g_hwnd, IDT_TAP);
    if (g_tap_target >= 0) smssession_press(g_session, g_tap_target, 0);
    g_tap_target = -1;
}

/* The console running now (the setting runs ahead of it while Settings is
 * open). */
static int current_console(void)
{
    const int c = smssession_console(g_session);
    return (c >= 0 && c < SMS_CONSOLE_COUNT) ? c : SMS_CONSOLE_SMS1;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '\\');
    const char *t = strrchr(p, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

static void set_text_utf8(HWND h, const char *text, int statusbar)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512);
    if (statusbar) SendMessageW(h, SB_SETTEXTW, 1, (LPARAM)w);   /* part 1: the text */
    else SetWindowTextW(h, w);
}

static void update_status(void)
{
    char title[512], st[160], line[400];
    const char *cart = smssession_cart_path(g_session);

    if (!smssession_is_running(g_session)) {
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 stopped");
        snprintf(st, sizeof st, "stopped");
    } else {
        smssession_cart_status(g_session, st, sizeof st);
        if (cart && cart[0])
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 %s", base_name(cart));
        else if (smssession_cart_booted_game(g_session))
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 booted from FujiNet");
        else
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 CONFIG");
    }
    set_text_utf8(g_hwnd, title, 0);

    if (!g_statusbar) return;
    /* Part 0 is the link dot, drawn in WM_DRAWITEM from this item data. */
    SendMessageA(g_statusbar, SB_SETTEXTA, SBT_OWNERDRAW | 0,
                 (LPARAM)(smssession_cart_link_up(g_session) == 1));
    if (g_pad_notice[0] && GetTickCount() < g_pad_notice_until) {
        snprintf(line, sizeof line, "%s", g_pad_notice);
    } else {
        g_pad_notice[0] = '\0';
        snprintf(line, sizeof line, "FujiNet: %s%s", st,
                 smssession_fujinet_running(g_session) ? "" : " (runtime not running)");
    }
    set_text_utf8(g_statusbar, line, 1);
}

/* The link dot: the accent while the cartridge's link to FujiNet is up,
 * grey otherwise. */
static void draw_status_dot(const DRAWITEMSTRUCT *di)
{
    const int up = di->itemData != 0;
    const int size = 10;
    RECT r = di->rcItem;
    const int x = r.left + ((r.right - r.left) - size) / 2;
    const int y = r.top + ((r.bottom - r.top) - size) / 2;
    HGDIOBJ oldb = SelectObject(di->hDC, up ? (HGDIOBJ)g_accent_brush : GetStockObject(GRAY_BRUSH));
    HGDIOBJ oldp = SelectObject(di->hDC, GetStockObject(NULL_PEN));
    Ellipse(di->hDC, x, y, x + size + 1, y + size + 1);
    SelectObject(di->hDC, oldp);
    SelectObject(di->hDC, oldb);
}

/* Gamepad hot-plug: the session bumps a generation on every add/remove and
 * keeps the last event as text; show it for a few seconds. */
static void poll_gamepads(void)
{
    const unsigned gen = smssession_gamepad_generation(g_session);
    if (gen == g_pad_generation) return;
    g_pad_generation = gen;
    if (smssession_gamepad_last_event(g_session, g_pad_notice, sizeof g_pad_notice) > 0) {
        g_pad_notice_until = GetTickCount() + PAD_NOTICE_MS;
        update_status();
    }
    sms_controller_window_gamepads_changed();
}

/* ---- menu ----------------------------------------------------------------- */

/* A Machine menu item's label with the key it is bound to after the tab --
 * read from the bindings each time the menu opens, so a remap in the
 * Controllers window shows here. The key is only a label: there is no
 * accelerator table, so Return and Backspace reach the console through the
 * bindings rather than being eaten by the menu. */
static void machine_item(HMENU m, UINT id, const char *text, int target, int enabled)
{
    char key[32], label[96];
    const sms_binding b = smssession_binding_get(g_session, target);
    smssession_keysym_name(b.keysym, key, sizeof key);
    if (key[0]) snprintf(label, sizeof label, "%s\t%s", text, key);
    else snprintf(label, sizeof label, "%s", text);
    ModifyMenuA(m, id, MF_BYCOMMAND | MF_STRING, id, label);
    EnableMenuItem(m, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
}

/* The console's own buttons follow the console: only the first Master
 * System has a Reset button, the Japanese one a Rapid button in its place,
 * and the Master System II and the Mark III neither. */
static void sync_machine_menu(void)
{
    HMENU m = GetMenu(g_hwnd);
    const int console = current_console();
    if (!m) return;
    machine_item(m, IDM_PAUSE, "&Pause", SMS_TARGET_SWITCH(SMS_SW_PAUSE), 1);
    machine_item(m, IDM_RESET_BUTTON, console == SMS_CONSOLE_SMSJ ? "R&apid Button" : "&Reset Button",
                 SMS_TARGET_SWITCH(SMS_SW_RESET), sms_console_has_reset_button(console));
    machine_item(m, IDM_SOFT_RESET, "&Soft Reset", SMS_TARGET_SYSACT(SMS_SYSACT_SOFT_RESET), 1);
    machine_item(m, IDM_RESET_CONFIG, "Reset to &CONFIG", SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG), 1);
}

static void build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU fuji = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuA(machine, MF_STRING, IDM_OPEN, "&Open Cartridge...\tCtrl+O");
    AppendMenuA(machine, MF_STRING, IDM_EJECT, "&Eject Cartridge");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_SD, "&Import Cartridge to SD...");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_BIOS, "Import &BIOS...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_PAUSE, "&Pause");
    AppendMenuA(machine, MF_STRING, IDM_RESET_BUTTON, "&Reset Button");
    AppendMenuA(machine, MF_STRING, IDM_SOFT_RESET, "&Soft Reset");
    AppendMenuA(machine, MF_STRING, IDM_RESET_CONFIG, "Reset to &CONFIG");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_SETTINGS, "Se&ttings...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");

    AppendMenuA(view, MF_STRING, IDM_CONTROLLERS, "&Controllers\tF9");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);
    AppendMenuA(view, MF_STRING | (g_aspect ? MF_CHECKED : 0), IDM_TV_ASPECT, "&TV Aspect");
    AppendMenuA(view, MF_STRING | (g_smooth ? MF_CHECKED : 0), IDM_SMOOTH, "&Smooth Scaling");
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "&Fullscreen\tF11");

    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_CONFIG, "&Web UI");
    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_LOG, "Console &Log");

    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About " APP_TITLE);

    AppendMenuA(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fuji, "&FujiNet");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    SetMenu(hwnd, bar);
    sync_machine_menu();
}

static void open_cart(const char *path)
{
    if (smssession_load_cart(g_session, path) != 0)
        MessageBoxA(g_hwnd, smssession_last_error(g_session), "Could not open",
                    MB_ICONWARNING | MB_OK);
    update_status();
}

static void settings_bios_imported(void);

/* Import BIOS: the session's message always says what happened -- which
 * image it was, whether it fits the console, the custom-CRC warning. */
static void import_bios(HWND owner, const char *path)
{
    char msg[512];
    const int idx = smssession_import_bios(g_session, path, msg, sizeof msg);
    MessageBoxA(owner, msg[0] ? msg : smssession_last_error(g_session), "Import BIOS",
                (idx < 0 || idx >= smssession_bios_count() ? MB_ICONWARNING : MB_ICONINFORMATION) | MB_OK);
    settings_bios_imported();
}

/* A dropped file, or one named on the command line: a BIOS or patch ROM is
 * imported, a cartridge opened, anything else copied to FujiNet's SD
 * folder for CONFIG to mount. */
static void load_media(const char *path)
{
    char dest[1024];
    if (smssession_media_is_bios(path)) { import_bios(g_hwnd, path); return; }
    if (smssession_media_is_cartridge(path)) { open_cart(path); return; }
    if (smssession_import_media(g_session, path, dest, sizeof dest) != 0) {
        MessageBoxA(g_hwnd, smssession_last_error(g_session), "Import failed",
                    MB_ICONWARNING | MB_OK);
        return;
    }
    MessageBoxA(g_hwnd, "Copied to FujiNet's SD folder. Mount it from the CONFIG client.",
                "Imported", MB_ICONINFORMATION | MB_OK);
}

static int pick_cart(const char *title, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "Master System and SG-1000 cartridges (*.sms;*.sg;*.bin;*.rom)\0*.sms;*.sg;*.bin;*.rom\0"
                      "All files\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

/* A BIOS image or the YM2413's patch ROM, the user's own dump. */
static int pick_bios(HWND owner, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "BIOS and patch ROM images (*.sms;*.bin;*.rom;*.ic2)\0*.sms;*.bin;*.rom;*.ic2\0"
                      "All files\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = "Import BIOS";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

/* ---- settings window -------------------------------------------------------
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. The stick, the picture,
 * the volume and the gamepad assignments apply live; the machine (console,
 * BIOS, FM) and the host options are applied when the window closes, by
 * one power cycle or restart for the lot.
 */

#define BIOS_ROWS_MAX 32

static HWND g_settings_window;
static int g_settings_dirty;
static int g_settings_console;             /* the console the Machine section shows */
static HWND g_pad_list, g_pad_port, g_bios_combo, g_fm_unit, g_fm_mutes, g_fm_info, g_patch_info;
static const char *g_bios_row_name[BIOS_ROWS_MAX];   /* BIOS name per combo row; "" none */
static int g_bios_rows;

static const char *aspect_name(int i)
{
    static const char *const names[] = { "TV (the console's pixel aspect)", "Square pixels", NULL };
    return (i >= 0 && i < 2) ? names[i] : NULL;
}

static void settings_apply_checkbox(HWND hwnd, int id, const char *key, int def)
{
    int on = SendMessageA(GetDlgItem(hwnd, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (smssession_get_int(g_session, key, def) != on) {
        smssession_set_int(g_session, key, on);
        g_settings_dirty = 1;
    }
}

/* The BIOS choices for the console the window shows: none (boot the
 * cartridge directly, always there), then every imported image that fits
 * it, the custom one included. A choice whose image has gone stays listed
 * so the combo never lies about what is set. */
static void fill_bios_combo(void)
{
    const int c = g_settings_console;
    const char *cur = smssession_console_bios(g_session, c);
    int i, sel = 0;

    if (!g_bios_combo) return;
    SendMessageA(g_bios_combo, CB_RESETCONTENT, 0, 0);
    g_bios_rows = 0;
    if (!sms_console_has_bios_socket(c)) {
        SendMessageA(g_bios_combo, CB_ADDSTRING, 0, (LPARAM)"None (no BIOS socket)");
        g_bios_row_name[g_bios_rows++] = "";
        SendMessageA(g_bios_combo, CB_SETCURSEL, 0, 0);
        EnableWindow(g_bios_combo, FALSE);
        return;
    }
    EnableWindow(g_bios_combo, TRUE);
    SendMessageA(g_bios_combo, CB_ADDSTRING, 0, (LPARAM)"None (boot the cartridge)");
    g_bios_row_name[g_bios_rows++] = "";
    /* <= count: the custom image sits one past the table */
    for (i = 0; i <= smssession_bios_count() && g_bios_rows < BIOS_ROWS_MAX; i++) {
        const sms_bios_info *b = smssession_bios_info(i);
        char line[160];
        int current, have;
        if (!b || !((b->consoles >> c) & 1)) continue;   /* the YM2413 patch ROM fits no console */
        current = cur && cur[0] && strcmp(cur, b->name) == 0;
        have = smssession_bios_available(g_session, i);
        if (!have && !current) continue;
        snprintf(line, sizeof line, "%s%s", b->desc, have ? "" : " (not imported)");
        SendMessageA(g_bios_combo, CB_ADDSTRING, 0, (LPARAM)line);
        if (current) sel = g_bios_rows;
        g_bios_row_name[g_bios_rows++] = b->name;
    }
    SendMessageA(g_bios_combo, CB_SETCURSEL, (WPARAM)sel, 0);
}

/* The FM row: the Mark III takes the FM Sound Unit (a switch, and whether
 * FM mutes the PSG); the Japanese Master System has the YM2413 built in;
 * the export consoles have no FM at all. */
static void sync_fm_controls(void)
{
    const int c = g_settings_console;
    const int mark3 = c == SMS_CONSOLE_MARK3;
    const int patch = smssession_bios_find("ym2413");
    char line[160];

    if (!g_fm_unit) return;
    ShowWindow(g_fm_unit, mark3 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_fm_mutes, mark3 ? SW_SHOW : SW_HIDE);
    EnableWindow(g_fm_mutes, smssession_get_int(g_session, "fm_unit", 1) != 0);
    ShowWindow(g_fm_info, mark3 ? SW_HIDE : SW_SHOW);
    SetWindowTextA(g_fm_info, sms_console_has_fm(c) ? "YM2413 FM sound built in"
                                                    : "No FM sound on this console");
    snprintf(line, sizeof line, "YM2413 patch ROM: %s",
             patch >= 0 && smssession_bios_available(g_session, patch)
                 ? "imported" : "not imported (ymfm's own patch set)");
    SetWindowTextA(g_patch_info, line);
}

/* An import from the menu or the window's own button: the list may have
 * grown, the patch ROM may be in, and the console may have a new BIOS. */
static void settings_bios_imported(void)
{
    if (!g_settings_window) return;
    fill_bios_combo();
    sync_fm_controls();
}

static void refresh_pad_list(void)
{
    int i, n, sel;
    if (!g_pad_list) return;
    sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
    SendMessageA(g_pad_list, LB_RESETCONTENT, 0, 0);
    n = smssession_gamepad_count(g_session);
    if (n == 0) {
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)"(no gamepads connected)");
    }
    for (i = 0; i < n; i++) {
        char name[128], line[200];
        int eff = smssession_gamepad_effective_port(g_session, i);
        smssession_gamepad_name(g_session, i, name, sizeof name);
        if (eff >= 0)
            snprintf(line, sizeof line, "%s  [player %d%s]", name, eff + 1,
                     smssession_gamepad_assignment(g_session, i) < 0 ? ", automatic" : "");
        else
            snprintf(line, sizeof line, "%s  [unused]", name);
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel >= 0 && sel < n) SendMessageA(g_pad_list, LB_SETCURSEL, (WPARAM)sel, 0);
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SET_CONSOLE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int c = (int)SendMessageA(GetDlgItem(hwnd, IDC_SET_CONSOLE), CB_GETCURSEL, 0, 0);
                if (c < 0 || c >= SMS_CONSOLE_COUNT) return 0;
                if (c != smssession_get_int(g_session, "console", SMS_CONSOLE_SMS1)) {
                    smssession_set_int(g_session, "console", c);
                    g_settings_dirty = 1;
                }
                g_settings_console = c;
                fill_bios_combo();
                sync_fm_controls();
            }
            return 0;
        case IDC_SET_BIOS:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int row = (int)SendMessageA(g_bios_combo, CB_GETCURSEL, 0, 0);
                if (row >= 0 && row < g_bios_rows &&
                    strcmp(smssession_console_bios(g_session, g_settings_console), g_bios_row_name[row]) != 0) {
                    smssession_set_console_bios(g_session, g_settings_console, g_bios_row_name[row]);
                    g_settings_dirty = 1;
                }
            }
            return 0;
        case IDC_SET_IMPORT_BIOS: {
            char path[MAX_PATH], before[64];
            if (!pick_bios(hwnd, path, sizeof path)) return 0;
            snprintf(before, sizeof before, "%s", smssession_console_bios(g_session, g_settings_console));
            import_bios(hwnd, path);
            /* An image that fits becomes the console's BIOS: that is a
             * machine change like any other here. */
            if (strcmp(before, smssession_console_bios(g_session, g_settings_console)) != 0)
                g_settings_dirty = 1;
            return 0;
        }
        case IDC_SET_FM_UNIT:
            settings_apply_checkbox(hwnd, IDC_SET_FM_UNIT, "fm_unit", 1);
            sync_fm_controls();
            return 0;
        case IDC_SET_FM_MUTES: settings_apply_checkbox(hwnd, IDC_SET_FM_MUTES, "fm_unit_mutes_psg", 0); return 0;
        case IDC_SET_ASPECT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                HMENU m = GetMenu(g_hwnd);
                /* row 0 is the TV, row 1 square pixels */
                g_aspect = SendMessageA(GetDlgItem(hwnd, IDC_SET_ASPECT), CB_GETCURSEL, 0, 0) == 0;
                smssession_set_int(g_session, "aspect", g_aspect ? 0 : 1);
                if (m) CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect ? MF_CHECKED : MF_UNCHECKED));
                InvalidateRect(g_hwnd, NULL, TRUE);
            }
            return 0;
        case IDC_SET_AN_JOY:
            smssession_set_analog(g_session,
                SendMessageA(GetDlgItem(hwnd, IDC_SET_AN_JOY), BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_SET_PAD_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
                SendMessageA(g_pad_port, CB_SETCURSEL,
                             (WPARAM)(smssession_gamepad_assignment(g_session, sel) + 1), 0);
            }
            return 0;
        case IDC_SET_PAD_PORT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
                int choice = (int)SendMessageA(g_pad_port, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < smssession_gamepad_count(g_session))
                    smssession_gamepad_assign(g_session, sel, choice - 1);
                refresh_pad_list();
                sms_controller_window_gamepads_changed();
            }
            return 0;
        case IDC_SET_FUJINET: settings_apply_checkbox(hwnd, IDC_SET_FUJINET, "enable_fujinet", 1); return 0;
        case IDC_SET_AUDIO: settings_apply_checkbox(hwnd, IDC_SET_AUDIO, "enable_audio", 1); return 0;
        case IDC_SET_GAMEPAD: settings_apply_checkbox(hwnd, IDC_SET_GAMEPAD, "enable_gamepad", 1); return 0;
        default: break;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(hwnd, IDC_SET_VOLUME))
            smssession_set_volume(g_session, (int)SendMessageA((HWND)lp, TBM_GETPOS, 0, 0));
        return 0;
    case WM_TIMER:
        if (wp == IDT_SETTINGS_PADS) {
            static unsigned seen;
            unsigned gen = smssession_gamepad_generation(g_session);
            if (gen != seen) { seen = gen; refresh_pad_list(); }
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_SETTINGS_PADS);
        g_settings_window = NULL;
        g_pad_list = g_pad_port = g_bios_combo = NULL;
        g_fm_unit = g_fm_mutes = g_fm_info = g_patch_info = NULL;
        if (g_settings_dirty) {
            g_settings_dirty = 0;
            restart_session();
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND settings_checkbox(HWND parent, HINSTANCE inst, const char *text, int id, int x, int y, int w, int checked)
{
    HWND h = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             x, y, w, 22, parent, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return h;
}

static HWND settings_label(HWND parent, HINSTANCE inst, const char *text, int x, int y, int w, int h)
{
    HWND l = CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent,
                             NULL, inst, NULL);
    SendMessageA(l, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return l;
}

static HWND settings_combo(HWND parent, HINSTANCE inst, int id, int x, int y, int w,
                           const char *(*names)(int), int sel)
{
    HWND c = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                             x, y, w, 200, parent, (HMENU)(INT_PTR)id, inst, NULL);
    int i;
    if (names)
        for (i = 0; names(i); i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)names(i));
    SendMessageA(c, CB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void show_settings(HINSTANCE inst)
{
    const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND h;
    RECT want;
    char line[200];
    int y = 12;

    if (g_settings_window) { SetForegroundWindow(g_settings_window); return; }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof wc);
            wc.lpfnWndProc = settings_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "SMSSettingsWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_settings_window = CreateWindowA("SMSSettingsWindow", "Settings", style,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 700, NULL, NULL, inst, NULL);
    g_settings_console = current_console();

    settings_label(g_settings_window, inst, "Machine (applied by a power cycle when this window closes)",
                   16, y, 440, 18); y += 22;
    settings_label(g_settings_window, inst, "Console:", 16, y + 3, 90, 18);
    settings_combo(g_settings_window, inst, IDC_SET_CONSOLE, 110, y, 220, sms_console_name, g_settings_console);
    y += 30;
    settings_label(g_settings_window, inst, "BIOS:", 16, y + 3, 90, 18);
    g_bios_combo = settings_combo(g_settings_window, inst, IDC_SET_BIOS, 110, y, 236, NULL, 0);
    SendMessageA(g_bios_combo, CB_SETDROPPEDWIDTH, 380, 0);   /* the descriptions are long */
    h = CreateWindowExA(0, "BUTTON", "Import BIOS...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        352, y - 1, 100, 26, g_settings_window, (HMENU)(INT_PTR)IDC_SET_IMPORT_BIOS, inst, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    fill_bios_combo();
    y += 30;
    settings_label(g_settings_window, inst, "FM:", 16, y + 3, 90, 18);
    g_fm_unit = settings_checkbox(g_settings_window, inst, "FM Sound Unit fitted", IDC_SET_FM_UNIT, 110, y, 150,
                                  smssession_get_int(g_session, "fm_unit", 1));
    g_fm_mutes = settings_checkbox(g_settings_window, inst, "FM mutes the PSG", IDC_SET_FM_MUTES, 270, y, 180,
                                   smssession_get_int(g_session, "fm_unit_mutes_psg", 0));
    g_fm_info = settings_label(g_settings_window, inst, "", 110, y + 3, 340, 18);
    y += 26;
    g_patch_info = settings_label(g_settings_window, inst, "", 110, y + 3, 340, 18);
    sync_fm_controls();
    y += 30;
    settings_label(g_settings_window, inst, "Picture:", 16, y + 3, 90, 18);
    settings_combo(g_settings_window, inst, IDC_SET_ASPECT, 110, y, 220, aspect_name, g_aspect ? 0 : 1);
    y += 40;

    settings_label(g_settings_window, inst, "Audio", 16, y, 440, 18); y += 22;
    settings_checkbox(g_settings_window, inst, "Enable audio (applied when this window closes)", IDC_SET_AUDIO,
                      16, y, 400, smssession_get_int(g_session, "enable_audio", 1));
    y += 28;
    settings_label(g_settings_window, inst, "Volume:", 16, y + 3, 90, 18);
    h = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                        110, y, 300, 28, g_settings_window, (HMENU)(INT_PTR)IDC_SET_VOLUME, inst, NULL);
    SendMessageA(h, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageA(h, TBM_SETTICFREQ, 10, 0);
    SendMessageA(h, TBM_SETPOS, TRUE, (LPARAM)smssession_get_int(g_session, "volume", 100));
    y += 40;

    settings_label(g_settings_window, inst, "Input", 16, y, 440, 18); y += 22;
    settings_checkbox(g_settings_window, inst, "Enable gamepads (applied when this window closes)", IDC_SET_GAMEPAD,
                      16, y, 400, smssession_get_int(g_session, "enable_gamepad", 1));
    y += 26;
    settings_checkbox(g_settings_window, inst, "The left stick drives the D-pad too", IDC_SET_AN_JOY, 16, y, 300,
                      smssession_get_int(g_session, "analog_joystick", 1));
    y += 30;
    settings_label(g_settings_window, inst, "Gamepads (select one, then choose its player):", 16, y, 440, 18); y += 22;
    g_pad_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 16, y, 316, 70,
        g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_LIST, inst, NULL);
    SendMessageA(g_pad_list, WM_SETFONT, (WPARAM)font, TRUE);
    g_pad_port = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        342, y, 110, 200, g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_PORT, inst, NULL);
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Automatic");
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Player 1");
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Player 2");
    SendMessageA(g_pad_port, CB_SETCURSEL, 0, 0);
    SendMessageA(g_pad_port, WM_SETFONT, (WPARAM)font, TRUE);
    refresh_pad_list();
    y += 80;

    settings_label(g_settings_window, inst, "FujiNet", 16, y, 440, 18); y += 22;
    settings_checkbox(g_settings_window, inst, "Enable FujiNet (applied when this window closes)", IDC_SET_FUJINET,
                      16, y, 400, smssession_get_int(g_session, "enable_fujinet", 1));
    y += 26;
    snprintf(line, sizeof line, "Web UI: %s", smssession_fujinet_webui_url(g_session));
    settings_label(g_settings_window, inst, line, 16, y + 3, 440, 18);
    y += 32;

    /* Exactly tall enough for what is in it. */
    SetRect(&want, 0, 0, 468, y);
    AdjustWindowRectEx(&want, style, FALSE, 0);
    SetWindowPos(g_settings_window, NULL, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER);

    SetTimer(g_settings_window, IDT_SETTINGS_PADS, 500, NULL);
    ShowWindow(g_settings_window, SW_SHOW);
}

/* ---- FujiNet console log --------------------------------------------------- */

static HWND g_log_window;
static HWND g_log_edit;

static void log_refresh(void)
{
    static char buf[128 * 1024];
    int n;
    DWORD first, last, lines;
    if (!g_log_edit) return;

    first = (DWORD)SendMessageA(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    lines = (DWORD)SendMessageA(g_log_edit, EM_GETLINECOUNT, 0, 0);
    {
        RECT rc;
        HDC dc = GetDC(g_log_edit);
        TEXTMETRICA tm;
        int visible = 1;
        GetClientRect(g_log_edit, &rc);
        if (dc) {
            HFONT of = (HFONT)SelectObject(dc, (HGDIOBJ)SendMessageA(g_log_edit, WM_GETFONT, 0, 0));
            if (GetTextMetricsA(dc, &tm) && tm.tmHeight > 0)
                visible = (rc.bottom - rc.top) / tm.tmHeight;
            SelectObject(dc, of);
            ReleaseDC(g_log_edit, dc);
        }
        last = first + (DWORD)(visible > 0 ? visible : 1);
    }
    n = smssession_fujinet_copy_log(g_session, buf, sizeof buf);
    SetWindowTextA(g_log_edit, n > 0 ? buf : "(no FujiNet output yet)");
    if (last >= lines) {
        int len = GetWindowTextLengthA(g_log_edit);
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
    }
}

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_log_edit) MoveWindow(g_log_edit, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_LOG_REFRESH) log_refresh();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_LOG_REFRESH);
        g_log_window = NULL;
        g_log_edit = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(HINSTANCE inst)
{
    RECT rc;
    if (g_log_window) { SetForegroundWindow(g_log_window); return; }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof wc);
            wc.lpfnWndProc = log_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "SMSFujiNetLogWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_log_window = CreateWindowA("SMSFujiNetLogWindow", "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 860, 600, NULL, NULL, inst, NULL);
    GetClientRect(g_log_window, &rc);
    g_log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, rc.right - rc.left, rc.bottom - rc.top, g_log_window, (HMENU)(INT_PTR)IDC_LOG_EDIT, inst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
    SetTimer(g_log_window, IDT_LOG_REFRESH, 1000, NULL);
    log_refresh();
    ShowWindow(g_log_window, SW_SHOW);
}

/* ---- window --------------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        g_placement.length = sizeof g_placement;
        GetWindowPlacement(hwnd, &g_placement);
        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            ShowWindow(g_statusbar, SW_HIDE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        build_menu(hwnd);
        ShowWindow(g_statusbar, SW_SHOW);
        SetWindowPlacement(hwnd, &g_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_statusbar) SendMessageA(g_statusbar, WM_SIZE, 0, 0);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DRAWITEM:
        if (wp == IDC_STATUSBAR) { draw_status_dot((const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_INITMENUPOPUP:
        sync_machine_menu();
        break;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        uint32_t ks;
        int sa;
        const int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wp == VK_F9) { sms_controller_window_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F12) { sms_debugger_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F11) { toggle_fullscreen(hwnd); return 0; }
        if (msg == WM_SYSKEYDOWN) break;
        if (ctrl) {
            if (wp == 'O') { PostMessage(hwnd, WM_COMMAND, IDM_OPEN, 0); return 0; }
            break;
        }
        if (lp & (1 << 30)) return 0;  /* auto-repeat: the key is already held */
        ks = sms_keysym_from_msg(wp, lp);
        if (!ks) break;
        /* The session's own actions (Escape, F3) first; then the console:
         * Return is Pause and Backspace the Reset button, through the
         * bindings like every other key. */
        sa = smssession_key_sysaction(g_session, ks);
        if (sa >= 0) {
            if (!g_sysact_down[sa]) {
                g_sysact_down[sa] = 1;
                run_sysaction(sa);
                update_status();
            }
            return 0;
        }
        if (smssession_key(g_session, ks, 1)) return 0;
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        uint32_t ks = sms_keysym_from_msg(wp, lp);
        int sa;
        if (!ks) break;
        sa = smssession_key_sysaction(g_session, ks);
        if (sa >= 0) { g_sysact_down[sa] = 0; return 0; }
        if (smssession_key(g_session, ks, 0)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            smssession_release_all(g_session);
            memset(g_sysact_down, 0, sizeof g_sysact_down);
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_STATUS) update_status();
        else if (wp == IDT_SYSACT) {
            int sa;
            while (smssession_sysaction_take(g_session, &sa)) {
                run_sysaction(sa);
                update_status();
            }
            poll_gamepads();
        } else if (wp == IDT_TAP) {
            tap_release();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN: {
            char path[MAX_PATH];
            if (pick_cart("Open Cartridge", path, sizeof path)) open_cart(path);
            return 0;
        }
        case IDM_EJECT: smssession_eject(g_session); update_status(); return 0;
        case IDM_IMPORT_SD: {
            char path[MAX_PATH], dest[1024], note[1200];
            if (!pick_cart("Import Cartridge to SD", path, sizeof path)) return 0;
            if (smssession_import_cart_to_sd(g_session, path, dest, sizeof dest) != 0) {
                MessageBoxA(hwnd, smssession_last_error(g_session), "Import failed", MB_ICONWARNING | MB_OK);
                return 0;
            }
            snprintf(note, sizeof note, "%s is on the SD host. Boot it from the CONFIG client.", base_name(dest));
            MessageBoxA(hwnd, note, "Imported", MB_ICONINFORMATION | MB_OK);
            return 0;
        }
        case IDM_IMPORT_BIOS: {
            char path[MAX_PATH];
            if (pick_bios(hwnd, path, sizeof path)) import_bios(hwnd, path);
            return 0;
        }
        case IDM_PAUSE: tap_button(SMS_TARGET_SWITCH(SMS_SW_PAUSE)); return 0;
        case IDM_RESET_BUTTON:
            if (sms_console_has_reset_button(current_console()))
                tap_button(SMS_TARGET_SWITCH(SMS_SW_RESET));
            return 0;
        case IDM_SOFT_RESET: run_sysaction(SMS_SYSACT_SOFT_RESET); update_status(); return 0;
        case IDM_RESET_CONFIG: run_sysaction(SMS_SYSACT_RESET_CONFIG); update_status(); return 0;
        case IDM_CONTROLLERS: sms_controller_window_toggle(hwnd, g_session); return 0;
        case IDM_DEBUGGER: sms_debugger_toggle(hwnd, g_session); return 0;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); return 0;
        case IDM_TV_ASPECT: {
            HMENU m = GetMenu(hwnd);
            g_aspect = !g_aspect;
            if (m) CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect ? MF_CHECKED : MF_UNCHECKED));
            smssession_set_int(g_session, "aspect", g_aspect ? 0 : 1);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_SMOOTH: {
            HMENU m = GetMenu(hwnd);
            g_smooth = !g_smooth;
            if (m) CheckMenuItem(m, IDM_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
            smssession_set_int(g_session, "smooth", g_smooth);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_SETTINGS: show_settings((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE)); return 0;
        case IDM_FUJINET_LOG: show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE)); return 0;
        case IDM_FUJINET_CONFIG:
            if (!smssession_fujinet_running(g_session)) {
                MessageBoxA(hwnd, "FujiNet is not running.", "FujiNet", MB_ICONINFORMATION | MB_OK);
                return 0;
            }
            ShellExecuteA(hwnd, "open", smssession_fujinet_webui_url(g_session), NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case IDM_ABOUT:
            MessageBoxA(hwnd,
                APP_TITLE " " SMS_VERSION_STRING "\n\n"
                "A Sega Master System with a built-in FujiNet.\n"
                "The emulator is MAME's Master System drivers (BSD-3-Clause)\n"
                "carried over to C around floooh's z80.h (zlib), with ymfm's\n"
                "YM2413 (BSD-3-Clause), and the FujiNet firmware for the cartridge.\n\n"
                "Copyright (C) 2026 Thomas Cherryhomes -- GPL-3.0-or-later\n"
                "https://fujinet.online/",
                "About " APP_TITLE, MB_ICONINFORMATION | MB_OK);
            return 0;
        case IDM_EXIT: PostMessage(hwnd, WM_CLOSE, 0, 0); return 0;
        default: break;
        }
        break;

    case WM_DROPFILES: {
        char path[MAX_PATH];
        HDROP drop = (HDROP)wp;
        if (DragQueryFileA(drop, 0, path, sizeof path)) load_media(path);
        DragFinish(drop);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, IDT_STATUS);
        KillTimer(hwnd, IDT_SYSACT);
        tap_release();
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static int env_on(const char *name)
{
    const char *env = getenv(name);
    return env && *env && *env != '0';
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEX wc;
    MSG msg;
    smssession_start_opts opts;
    RECT want;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    int parts[2] = { DOT_PART_W, -1 };
    char media[MAX_PATH];
    const char *later = NULL;   /* a non-cartridge path from the command line */
    (void)prev;

    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&g_fb_lock);

    g_session = smssession_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session (unusable config or data directories?)",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    g_fb = calloc((size_t)SMSSESSION_FB_WIDTH * SMSSESSION_FB_MAX_HEIGHT, sizeof *g_fb);
    if (!g_fb) return 1;
    g_accent_brush = CreateSolidBrush(RGB((SMSSESSION_ACCENT_RGB >> 16) & 0xff,
                                          (SMSSESSION_ACCENT_RGB >> 8) & 0xff,
                                          SMSSESSION_ACCENT_RGB & 0xff));

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof g_bmi.bmiHeader;
    g_bmi.bmiHeader.biWidth = SMSSESSION_FB_WIDTH;
    g_bmi.bmiHeader.biHeight = -SMSSESSION_FB_MAX_HEIGHT;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    /* "aspect": 0 (the default) the television's pixel aspect, 1 square --
     * the family's meaning, shared with the other frontends */
    g_aspect = smssession_get_int(g_session, "aspect", 0) == 0;
    g_smooth = smssession_get_int(g_session, "smooth", 0);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WIN_CLASS;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassEx(&wc);

    /* Three times the console's picture, at the TV's aspect, plus chrome. */
    {
        const int pal = sms_console_is_pal(current_console());
        const double par = pal ? 14.75 / 5.3203424 / 2.0 : 8.0 / 7.0;
        SetRect(&want, 0, 0, (int)(SMSSESSION_FB_WIDTH * 3 * par), (pal ? 240 : 224) * 3);
    }
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_ACCEPTFILES);
    g_hwnd = CreateWindowEx(WS_EX_ACCEPTFILES, WIN_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                            want.bottom - want.top + 24, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    build_menu(g_hwnd);
    g_statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, inst, NULL);
    SendMessageA(g_statusbar, SB_SETPARTS, 2, (LPARAM)parts);
    ShowWindow(g_hwnd, show);

    smssession_default_opts(g_session, &opts);
    if (cmdline && *cmdline) {
        /* a path on the command line, quotes and all: a cartridge boots in
         * place of the remembered one; a BIOS or anything else is routed
         * like a dropped file once the machine is up */
        const char *p = cmdline;
        size_t n;
        if (*p == '"') p++;
        snprintf(media, sizeof media, "%s", p);
        n = strlen(media);
        while (n && (media[n - 1] == '"' || media[n - 1] == ' ')) media[--n] = '\0';
        if (media[0]) {
            if (!smssession_media_is_bios(media) && smssession_media_is_cartridge(media))
                opts.cart_path = media;
            else
                later = media;
        }
    }
    if (smssession_start(g_session, &opts) != 0)
        MessageBoxA(g_hwnd, smssession_last_error(g_session), APP_TITLE, MB_ICONWARNING | MB_OK);
    if (later) load_media(later);

    g_pad_generation = smssession_gamepad_generation(g_session);
    SetTimer(g_hwnd, IDT_STATUS, 1000, NULL);
    SetTimer(g_hwnd, IDT_SYSACT, 100, NULL);
    update_status();

    if (env_on("SMS_OPEN_CONTROLLERS")) sms_controller_window_toggle(g_hwnd, g_session);
    if (env_on("SMS_OPEN_DEBUGGER")) sms_debugger_show(g_hwnd, g_session);
    if (env_on("SMS_OPEN_SETTINGS")) show_settings(inst);

    g_present_thread = CreateThread(NULL, 0, present_thread, NULL, 0, NULL);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the sub-windows' keys reach them
         * regardless of which child control has the focus. */
        if (sms_debugger_pretranslate(&msg)) continue;
        if (sms_controller_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    InterlockedExchange(&g_running, 0);
    if (g_present_thread) {
        WaitForSingleObject(g_present_thread, 2000);
        CloseHandle(g_present_thread);
    }
    smssession_stop(g_session);
    smssession_free(g_session);
    free(g_fb);
    DeleteObject(g_accent_brush);
    DeleteCriticalSection(&g_fb_lock);
    return (int)msg.wParam;
}
