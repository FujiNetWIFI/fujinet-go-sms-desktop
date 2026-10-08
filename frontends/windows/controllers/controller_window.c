/*
 * The Win32 Controllers window: both Master System joypads side by side --
 * the D-pad and buttons 1 and 2 -- each with the gamepad driving it, then
 * the console's Pause and Reset buttons and the session's resets, then the
 * Map row, then the gamepads and which player each one drives.
 *
 * A button lights in the accent colour whenever the console sees it held,
 * from whatever source (keyboard, gamepad, or a click here), so the window
 * doubles as an input tester.
 *
 * Buttons are driven by WM_LBUTTONDOWN/WM_LBUTTONUP on the window rather
 * than by BN_CLICKED: a joypad button is HELD, and a game polls it, so a
 * value present only for the instant of a click falls between frames.
 * The mouse is captured on press and released on button-up wherever that
 * happens, so dragging off a button cannot strand the machine with a button
 * held forever.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controller_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <string.h>

#include "../key_forward.h"

#define PAD_CLASS "FujiNetGoSMSControllers"

#define BTN      40         /* a D-pad arm */
#define ROUND    52         /* buttons 1 and 2 */
#define GAP       6
#define MARGIN   12
#define PAD_W   (MARGIN + 3 * BTN + 2 * GAP + 40 + 2 * ROUND + 2 * GAP + MARGIN)
#define HEAD_H   22
#define PADS_H   20
#define WIDE_W  120
#define WIDE_H   40
#define ROW_H    26         /* one gamepad's row */
#define PAD_ROWS  4         /* gamepad rows the window has room for */
#define SEG_W    76         /* Automatic / Player 1 / Player 2 */

#define IDT_CAPTURE 1
#define IDT_HELD    2

typedef struct {
    RECT rc;
    int target;
    int round;               /* drawn as the joypad's round 1 / 2 buttons */
    char face[20];
} pad_button;

static HWND g_panel;
static smssession *g_session;
static pad_button g_btn[SMS_TARGET_COUNT];
static int g_nbtn;
static int g_held = -1;          /* button index under the captured mouse */
static int g_map_state = -2;     /* -2 idle, -1 armed, >=0 awaiting a key/button */
static RECT g_map_rc, g_defaults_rc, g_hint_rc;
static RECT g_head_rc[2], g_pads_rc[2], g_box_rc[2];
static RECT g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static unsigned g_shown_held[2];
static int g_shown_sw[SMS_SW_COUNT];
static int g_shown_console = -1;
static char g_hint[200];
static HBRUSH g_accent_brush, g_body_brush;
static HFONT g_bold;
static int g_total_w, g_total_h;

static void add_button(const char *face, int target, int x, int y, int w, int h, int round)
{
    pad_button *b;
    if (g_nbtn >= SMS_TARGET_COUNT) return;
    b = &g_btn[g_nbtn++];
    SetRect(&b->rc, x, y, x + w, y + h);
    b->target = target;
    b->round = round;
    snprintf(b->face, sizeof b->face, "%s", face);
}

/* One joypad, laid out like the real one: a rectangle with the D-pad left
 * and buttons 1 and 2 right. Returns the y below it. */
static int build_controller(int port, int x0, int y0)
{
    int y = y0, cy, cx;

    SetRect(&g_head_rc[port], x0, y, x0 + PAD_W, y + HEAD_H);
    y += HEAD_H;
    SetRect(&g_pads_rc[port], x0, y, x0 + PAD_W, y + PADS_H);
    y += PADS_H + GAP;

    SetRect(&g_box_rc[port], x0, y, x0 + PAD_W, y + 3 * BTN + 2 * GAP + 2 * MARGIN);
    y += MARGIN;
    cx = x0 + MARGIN;
    cy = y;
    add_button("Up", SMS_TARGET_PORT(port, SMS_ACT_UP), cx + BTN + GAP, cy, BTN, BTN, 0);
    add_button("Left", SMS_TARGET_PORT(port, SMS_ACT_LEFT), cx, cy + BTN + GAP, BTN, BTN, 0);
    add_button("Right", SMS_TARGET_PORT(port, SMS_ACT_RIGHT), cx + 2 * (BTN + GAP), cy + BTN + GAP, BTN, BTN, 0);
    add_button("Down", SMS_TARGET_PORT(port, SMS_ACT_DOWN), cx + BTN + GAP, cy + 2 * (BTN + GAP), BTN, BTN, 0);

    /* 1 and 2 on one row, level with the middle of the D-pad. */
    cx += 3 * BTN + 2 * GAP + 40;
    cy += BTN + GAP + (BTN - ROUND) / 2;
    add_button("1", SMS_TARGET_PORT(port, SMS_ACT_1), cx, cy, ROUND, ROUND, 1);
    add_button("2", SMS_TARGET_PORT(port, SMS_ACT_2), cx + ROUND + 2 * GAP, cy, ROUND, ROUND, 1);

    return g_box_rc[port].bottom;
}

static void layout(void)
{
    const int x1 = MARGIN + PAD_W + 2 * MARGIN;
    int y, cx, i;

    g_nbtn = 0;
    y = build_controller(0, MARGIN, MARGIN);
    build_controller(1, x1, MARGIN);
    g_total_w = x1 + PAD_W + MARGIN;

    /* The console's buttons and the session's resets, centred. Debugger
     * Stop is here so it can be mapped like everything else. */
    y += MARGIN + 4;
    cx = (g_total_w - (5 * WIDE_W + 4 * GAP)) / 2;
    add_button("Pause", SMS_TARGET_SWITCH(SMS_SW_PAUSE), cx, y, WIDE_W, WIDE_H, 0);
    add_button("Reset", SMS_TARGET_SWITCH(SMS_SW_RESET), cx + (WIDE_W + GAP), y, WIDE_W, WIDE_H, 0);
    add_button("Soft Reset", SMS_TARGET_SYSACT(SMS_SYSACT_SOFT_RESET), cx + 2 * (WIDE_W + GAP), y,
               WIDE_W, WIDE_H, 0);
    add_button("Reset to CONFIG", SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG), cx + 3 * (WIDE_W + GAP), y,
               WIDE_W, WIDE_H, 0);
    add_button("Debugger Stop", SMS_TARGET_SYSACT(SMS_SYSACT_DEBUG_STOP), cx + 4 * (WIDE_W + GAP), y,
               WIDE_W, WIDE_H, 0);
    y += WIDE_H + MARGIN;

    SetRect(&g_map_rc, MARGIN, y, MARGIN + 70, y + 30);
    SetRect(&g_defaults_rc, MARGIN + 76, y, MARGIN + 76 + 84, y + 30);
    SetRect(&g_hint_rc, MARGIN + 76 + 84 + 10, y, g_total_w - MARGIN, y + 30);
    y += 30 + MARGIN + 4;

    /* The gamepads: a fixed number of rows, so the window never resizes
     * when one is plugged in. */
    SetRect(&g_gp_head_rc, MARGIN, y, g_total_w - MARGIN, y + HEAD_H);
    y += HEAD_H + 2;
    for (i = 0; i < PAD_ROWS; i++) {
        SetRect(&g_gp_row_rc[i], MARGIN, y, g_total_w - MARGIN, y + ROW_H);
        y += ROW_H + 2;
    }
    g_total_h = y + MARGIN;
}

/* The three assignment segments at the right of a gamepad row. */
static void segment_rect(int row, int seg, RECT *rc)
{
    const RECT *r = &g_gp_row_rc[row];
    const int x = r->right - (3 - seg) * (SEG_W + 4) + 4;
    SetRect(rc, x, r->top + 1, x + SEG_W, r->bottom - 1);
}

static int current_console(void)
{
    const int c = smssession_console(g_session);
    return (c >= 0 && c < SMS_CONSOLE_COUNT) ? c : SMS_CONSOLE_SMS1;
}

/* The Reset button exists on the first Master System (Reset) and the
 * Japanese one (Rapid) only. */
static int button_present(int target)
{
    if (target != SMS_TARGET_SWITCH(SMS_SW_RESET)) return 1;
    return sms_console_has_reset_button(current_console());
}

/* The session's actions, from a click here or a key: Debugger Stop has to
 * open the debugger window, which is the main window's business, so it goes
 * through the session's sysaction queue that the main window polls. */
static void run_sysaction(int sa)
{
    if (sa == SMS_SYSACT_DEBUG_STOP) smssession_sysaction_post(g_session, sa);
    else smssession_sysaction(g_session, sa);
}

static void press_target(int target, int down)
{
    if (target >= SMS_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not power-cycle the machine. */
        if (!down) run_sysaction(target - SMS_TARGET_SYSACT(0));
        return;
    }
    smssession_press(g_session, target, down);
}

static int hit(int x, int y)
{
    POINT p = { x, y };
    int i;
    for (i = 0; i < g_nbtn; i++)
        if (PtInRect(&g_btn[i].rc, p)) return i;
    return -1;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        KillTimer(g_panel, IDT_CAPTURE);
        smssession_gamepad_capture_cancel(g_session);
        g_hint[0] = '\0';
    } else if (state == -1) {
        KillTimer(g_panel, IDT_CAPTURE);
        smssession_gamepad_capture_cancel(g_session);
        snprintf(g_hint, sizeof g_hint, "Click a button to remap it");
    } else {
        snprintf(g_hint, sizeof g_hint, "Press a key or gamepad button for %s", sms_target_name(state));
        smssession_gamepad_capture_begin(g_session);
        SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    InvalidateRect(g_panel, NULL, TRUE);
}

/* After a rebind: name what it now does, and what it was taken from. */
static void bound_hint(int target, const char *what, const char *stolen)
{
    char name[64];
    snprintf(name, sizeof name, "%s", sms_target_name(target));   /* a static buffer */
    if (stolen[0])
        snprintf(g_hint, sizeof g_hint, "%s: %s (taken from %s)", name, what, stolen);
    else
        snprintf(g_hint, sizeof g_hint, "%s: %s", name, what);
}

static void button_label(const pad_button *b, char *out, int outsz)
{
    if (g_map_state != -2) {
        const sms_binding bind = smssession_binding_get(g_session, b->target);
        char key[32];
        smssession_keysym_name(bind.keysym, key, sizeof key);
        if (bind.button != SMS_PAD_BTN_NONE)
            snprintf(out, outsz, "%s\n%s", key[0] ? key : "-", sms_pad_button_name(bind.button));
        else
            snprintf(out, outsz, "%s", key[0] ? key : "-");
    } else if (b->target == SMS_TARGET_SWITCH(SMS_SW_RESET) && current_console() == SMS_CONSOLE_SMSJ) {
        snprintf(out, outsz, "Rapid");   /* the Japanese console's name for it */
    } else {
        snprintf(out, outsz, "%s", b->face);
    }
}

static void draw_label(HDC dc, const RECT *r, const char *label, int pushed)
{
    RECT text = *r;
    int h;
    /* Vertically centred, word-broken on the newline a Map-mode label
     * carries between its key and its pad button. */
    InflateRect(&text, -2, 0);
    h = DrawTextA(dc, label, -1, &text, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    text.left = r->left + 2;
    text.right = r->right - 2;
    text.top = r->top + ((r->bottom - r->top) - h) / 2 + (pushed ? 1 : 0);
    text.bottom = r->bottom;
    DrawTextA(dc, label, -1, &text, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
}

static void draw_button(HDC dc, const RECT *rc, const char *label, int pushed, int accent, int inactive)
{
    RECT r = *rc;

    if (accent) {
        FillRect(dc, &r, g_accent_brush);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(dc, RGB(255, 255, 255));
    } else {
        DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0)
                                             | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(dc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
    draw_label(dc, &r, label, pushed);
}

/* Buttons 1 and 2: round, light grey on the black pad, the accent when
 * held. */
static void draw_round_button(HDC dc, const RECT *rc, const char *label, int pushed, int accent)
{
    HGDIOBJ oldb = SelectObject(dc, accent ? (HGDIOBJ)g_accent_brush
                                           : GetStockObject(pushed ? GRAY_BRUSH : LTGRAY_BRUSH));
    HGDIOBJ oldp = SelectObject(dc, GetStockObject(BLACK_PEN));
    Ellipse(dc, rc->left, rc->top, rc->right, rc->bottom);
    SelectObject(dc, oldp);
    SelectObject(dc, oldb);
    SetTextColor(dc, accent ? RGB(255, 255, 255) : RGB(0, 0, 0));
    draw_label(dc, rc, label, pushed);
}

/* The gamepads driving a port, as one line. */
static void pads_line(int port, char *out, int outsz)
{
    int i, n = smssession_gamepad_count(g_session), len = 0;
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        char name[96];
        if (smssession_gamepad_effective_port(g_session, i) != port) continue;
        smssession_gamepad_name(g_session, i, name, sizeof name);
        len += snprintf(out + len, (size_t)(outsz - len), "%s%s", len ? ", " : "Gamepad: ", name);
        if (len >= outsz) break;
    }
    if (!out[0]) snprintf(out, (size_t)outsz, "No gamepad (keyboard only)");
}

/* One row per gamepad: its name, which player it drives now, and the
 * Automatic / Player 1 / Player 2 choice, the current one pushed in. */
static void paint_gamepads(HDC dc, HFONT font)
{
    static const char *const segs[3] = { "Automatic", "Player 1", "Player 2" };
    const int n = smssession_gamepad_count(g_session);
    int i, s;

    SelectObject(dc, g_bold);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    DrawTextA(dc, "Gamepads", -1, &g_gp_head_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, font);
    if (n == 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, "No gamepads connected", -1, &g_gp_row_rc[0],
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    for (i = 0; i < n && i < PAD_ROWS; i++) {
        char name[128], line[64];
        RECT name_rc = g_gp_row_rc[i], eff_rc;
        const int assign = smssession_gamepad_assignment(g_session, i);
        const int eff = smssession_gamepad_effective_port(g_session, i);
        RECT seg0;

        segment_rect(i, 0, &seg0);
        smssession_gamepad_name(g_session, i, name, sizeof name);
        name_rc.right = seg0.left - 150;
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextA(dc, name, -1, &name_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetRect(&eff_rc, name_rc.right + 8, name_rc.top, seg0.left - 8, name_rc.bottom);
        if (eff >= 0) snprintf(line, sizeof line, "drives player %d", eff + 1);
        else snprintf(line, sizeof line, "drives nothing");
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, line, -1, &eff_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        for (s = 0; s < 3; s++) {
            RECT r;
            segment_rect(i, s, &r);
            draw_button(dc, &r, segs[s], assign + 1 == s, 0, 0);
        }
    }
}

static void paint_panel(HDC dc)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ old = SelectObject(dc, font);
    RECT client;
    int i, port;

    GetClientRect(g_panel, &client);
    FillRect(dc, &client, (HBRUSH)(COLOR_BTNFACE + 1));
    SetBkMode(dc, TRANSPARENT);

    for (port = 0; port < 2; port++) {
        char line[200];
        RECT box = g_box_rc[port];
        SelectObject(dc, g_bold);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextA(dc, port ? "Player 2" : "Player 1", -1, &g_head_rc[port],
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, font);
        pads_line(port, line, sizeof line);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, line, -1, &g_pads_rc[port], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        /* the joypad's body: black, as Sega made it */
        FillRect(dc, &box, g_body_brush);
        FrameRect(dc, &box, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
    }

    for (i = 0; i < g_nbtn; i++) {
        char label[64];
        const int t = g_btn[i].target;
        int lit = 0, accent;
        const int clicked = (i == g_held) && g_map_state == -2;
        if (t < 2 * SMS_ACT_PER_PORT)
            lit = (g_shown_held[t / SMS_ACT_PER_PORT] >> (t % SMS_ACT_PER_PORT)) & 1;
        else if (t < SMS_TARGET_SYSACT(0))
            lit = g_shown_sw[t - SMS_TARGET_SWITCH(0)];
        button_label(&g_btn[i], label, sizeof label);
        accent = (g_map_state == -2 && (lit || clicked)) || (g_map_state >= 0 && t == g_map_state);
        if (g_btn[i].round) draw_round_button(dc, &g_btn[i].rc, label, clicked, accent);
        else draw_button(dc, &g_btn[i].rc, label, clicked, accent,
                         g_map_state == -2 && !button_present(t));
    }

    draw_button(dc, &g_map_rc, g_map_state == -2 ? "Map" : "Done", 0, g_map_state != -2, 0);
    draw_button(dc, &g_defaults_rc, "Defaults", 0, 0, 0);

    if (g_hint[0]) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, g_hint, -1, &g_hint_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    paint_gamepads(dc, font);
    SelectObject(dc, old);
}

static void refresh_held(void)
{
    int port, sw, changed = 0;
    const int console = current_console();
    for (port = 0; port < 2; port++) {
        unsigned now = smssession_buttons_held(g_session, port);
        if (now != g_shown_held[port]) { g_shown_held[port] = now; changed = 1; }
    }
    for (sw = 0; sw < SMS_SW_COUNT; sw++) {
        const int now = smssession_switch_held(g_session, sw);
        if (now != g_shown_sw[sw]) { g_shown_sw[sw] = now; changed = 1; }
    }
    /* Settings may have swapped the console: the Reset button comes and
     * goes with it. */
    if (console != g_shown_console) { g_shown_console = console; changed = 1; }
    if (changed) InvalidateRect(g_panel, NULL, FALSE);
}

static LRESULT CALLBACK pad_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        /* Off-screen first: the held highlights repaint at 20 Hz. */
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        oldbmp = SelectObject(mem, bmp);
        paint_panel(mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT p = { x, y };
        int i = hit(x, y), row, seg;

        if (PtInRect(&g_map_rc, p)) { set_map_state(g_map_state == -2 ? -1 : -2); return 0; }
        if (PtInRect(&g_defaults_rc, p)) {
            smssession_bindings_reset(g_session);
            snprintf(g_hint, sizeof g_hint, "Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        /* A gamepad row's player choice. */
        for (row = 0; row < PAD_ROWS && row < smssession_gamepad_count(g_session); row++) {
            for (seg = 0; seg < 3; seg++) {
                RECT r;
                segment_rect(row, seg, &r);
                if (PtInRect(&r, p)) {
                    /* 0 = automatic (connection order), 1 = player 1, 2 = player 2 */
                    smssession_gamepad_assign(g_session, row, seg - 1);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
        }
        if (i < 0) return 0;

        if (g_map_state == -1) { set_map_state(g_btn[i].target); return 0; }
        if (g_map_state >= 0) return 0;
        if (!button_present(g_btn[i].target)) return 0;

        g_held = i;
        SetCapture(hwnd);
        press_target(g_btn[i].target, 1);
        InvalidateRect(hwnd, &g_btn[i].rc, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        /* Release wherever the mouse ended up: capture means this arrives
         * even if the pointer left the button. */
        if (g_held >= 0) {
            RECT r = g_btn[g_held].rc;
            int t = g_btn[g_held].target;
            g_held = -1;
            ReleaseCapture();
            press_target(t, 0);
            InvalidateRect(hwnd, &r, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        /* Lost the mouse some other way (Alt+Tab mid-press): let go, but
         * without firing a system action nobody finished clicking. */
        if (g_held >= 0 && (HWND)lp != hwnd) {
            RECT r = g_btn[g_held].rc;
            const int t = g_btn[g_held].target;
            g_held = -1;
            if (t < SMS_TARGET_SYSACT(0)) smssession_press(g_session, t, 0);
            InvalidateRect(hwnd, &r, FALSE);
        }
        break;

    case WM_TIMER:
        if (wp == IDT_CAPTURE && g_map_state >= 0) {
            int button;
            if (smssession_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_map_state;
                stolen[0] = '\0';
                smssession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(target, sms_pad_button_name(button), stolen);
                InvalidateRect(hwnd, NULL, TRUE);
            }
        } else if (wp == IDT_HELD) {
            refresh_held();
        }
        return 0;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        const uint32_t ks = sms_keysym_from_msg(wp, lp);
        int sa;
        if (lp & (1 << 30)) return 0;   /* auto-repeat */
        if (g_map_state >= 0) {
            if (wp == VK_ESCAPE && !(GetKeyState(VK_SHIFT) & 0x8000)) { set_map_state(-1); return 0; }
            if (ks) {
                char stolen[128], name[32];
                const int target = g_map_state;
                stolen[0] = '\0';
                smssession_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
                smssession_keysym_name(ks, name, sizeof name);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(target, name, stolen);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        if (g_map_state == -1) {
            if (wp == VK_ESCAPE) set_map_state(-2);
            return 0;
        }
        if (wp == VK_F9) { ShowWindow(hwnd, SW_HIDE); return 0; }
        if (!ks) break;
        sa = smssession_key_sysaction(g_session, ks);
        if (sa >= 0) { run_sysaction(sa); return 0; }
        if (smssession_key(g_session, ks, 1)) return 0;
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        const uint32_t ks = sms_keysym_from_msg(wp, lp);
        if (g_map_state != -2) return 0;
        if (ks && smssession_key(g_session, ks, 0)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) smssession_release_all(g_session);
        return 0;
    case WM_SHOWWINDOW:
        if (wp) { refresh_held(); SetTimer(hwnd, IDT_HELD, 50, NULL); }
        else KillTimer(hwnd, IDT_HELD);
        break;
    case WM_CLOSE:
        /* Hide, do not destroy: the window's position survives closing it. */
        set_map_state(-2);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void sms_controller_window_toggle(HWND parent, smssession *session)
{
    g_session = session;

    if (!g_panel) {
        WNDCLASSEXA wc;
        RECT want;
        LOGFONTA lf;

        layout();
        g_accent_brush = CreateSolidBrush(RGB((SMSSESSION_ACCENT_RGB >> 16) & 0xff,
                                              (SMSSESSION_ACCENT_RGB >> 8) & 0xff,
                                              SMSSESSION_ACCENT_RGB & 0xff));
        g_body_brush = CreateSolidBrush(RGB(0x28, 0x28, 0x28));
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
        lf.lfWeight = FW_BOLD;
        g_bold = CreateFontIndirectA(&lf);

        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = pad_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = PAD_CLASS;
        RegisterClassExA(&wc);

        SetRect(&want, 0, 0, g_total_w, g_total_h);
        /* A tool window with a caption and no thick frame or maximize box:
         * it is exactly the size of its controls. WS_EX_TOOLWINDOW also
         * keeps it off the taskbar. */
        AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, FALSE, WS_EX_TOOLWINDOW);
        g_panel = CreateWindowExA(WS_EX_TOOLWINDOW, PAD_CLASS, "Controllers",
                                  WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  want.right - want.left, want.bottom - want.top,
                                  parent, NULL, wc.hInstance, NULL);
        if (!g_panel) return;
        g_shown_held[0] = g_shown_held[1] = 0;
        memset(g_shown_sw, 0, sizeof g_shown_sw);
        g_shown_console = -1;
    }

    if (IsWindowVisible(g_panel)) {
        set_map_state(-2);
        ShowWindow(g_panel, SW_HIDE);
    } else {
        ShowWindow(g_panel, SW_SHOW);
        SetForegroundWindow(g_panel);
    }
}

void sms_controller_window_gamepads_changed(void)
{
    if (g_panel && IsWindowVisible(g_panel)) InvalidateRect(g_panel, NULL, FALSE);
}

int sms_controller_pretranslate(MSG *msg)
{
    /* The window has no child controls, so its own window proc sees every
     * key; nothing to steal here. Kept for symmetry with the debugger. */
    (void)msg;
    return 0;
}
