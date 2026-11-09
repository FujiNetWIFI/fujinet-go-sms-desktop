/*
 * controls -- the on-screen controllers: one layout for every toolkit, its hit
 * test, the D-pad's drag maths and the label every console button has.
 *
 * The Master System has no keyboard. Its console has a PAUSE button (the
 * NMI) and, by model, a RESET button (Master System) or a RAPID control (the
 * Japanese one); each of its two control ports takes a Control Pad: a D-pad
 * and two buttons, 1 and 2 (smssession.h). The window draws the console's top
 * strip across the top and the two pads under it, Player 1 on the left (control
 * port 1 faces the player's left) and Player 2 on the right.
 *
 * Units: one D-pad arm's width is 1.0. The extents are the controls' bounding
 * box and are the same for every console, so the window never resizes when the
 * console changes: only the entry list does (the model's console buttons). A
 * toolkit adds its own margin.
 *
 * The D-pad is a disc (the dish the cross sits in) with four arms on it. A
 * press on an arm holds that direction alone; a press on the rest of the disc
 * (the centre, the diagonal corners) starts a drag whose vector, through
 * smssession_dpad_from_vector, picks one or two directions.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>

#include "smssession.h"

#define PI_F 3.14159265358979f

/* ---- geometry (layout units) ------------------------------------------------ */

#define TOTAL_W     15.2f
#define TOTAL_H     6.5f
#define STRIP_H     1.4f
#define CAP_W       1.8f
#define CAP_H       0.8f
#define CAP_Y       0.3f
#define PAUSE_X     12.9f
#define RESET_X     10.8f
#define PAD_W       7.2f
#define PAD_H       3.8f
#define PAD_GAP     0.8f
#define CAPTION_Y   1.9f
#define BODY_Y      (CAPTION_Y + 0.8f)
#define DISC_R      1.55f
#define ARM         1.0f
#define BTN         1.2f

#define N_COMMON    (1 + 1 + 2 * 9)     /* the strip, PAUSE, then two pads */
#define N_MAX       (N_COMMON + 1)      /* and RESET / RAPID */

typedef struct {
    sms_control entry[N_MAX];
    int count;
} variant;

static char g_legend[SMS_CONSOLE_COUNT][64];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* The eight sectors in order round the disc from RIGHT, counter-clockwise as
 * drawn (so UP, at the top, is 2). */
static const unsigned k_sector_acts[8] = {
    1u << SMS_ACT_RIGHT,
    (1u << SMS_ACT_UP) | (1u << SMS_ACT_RIGHT),
    1u << SMS_ACT_UP,
    (1u << SMS_ACT_UP) | (1u << SMS_ACT_LEFT),
    1u << SMS_ACT_LEFT,
    (1u << SMS_ACT_DOWN) | (1u << SMS_ACT_LEFT),
    1u << SMS_ACT_DOWN,
    (1u << SMS_ACT_DOWN) | (1u << SMS_ACT_RIGHT),
};

static void add(variant *var, int kind, const char *label, const char *legend, int port,
                int target, unsigned acts, float xpos, float ypos, float wid, float hgt)
{
    sms_control *ctl = &var->entry[var->count++];
    ctl->kind = kind;
    ctl->label = label;
    ctl->legend = legend;
    ctl->port = port;
    ctl->target = target;
    ctl->act_mask = acts;
    ctl->left = xpos; ctl->top = ypos; ctl->width = wid; ctl->height = hgt;
}

static void pad(variant *var, int port, float xzero)
{
    static const char *const k_caption[SMS_PLAYERS] = { "Player 1", "Player 2" };
    static const char *const k_arm[4] = { "Up", "Down", "Left", "Right" };
    const float body = BODY_Y;
    const float cenx = xzero + 2.0f, ceny = body + 1.9f;
    const float armx[4] = { cenx - 0.5f, cenx - 0.5f, cenx - 1.5f, cenx + 0.5f };
    const float army[4] = { ceny - 1.5f, ceny + 0.5f, ceny - 0.5f, ceny - 0.5f };

    add(var, SMS_CTL_CAPTION, k_caption[port], NULL, port, -1, 0,
        xzero, CAPTION_Y, PAD_W, 0.7f);
    add(var, SMS_CTL_PAD, "Control Pad", NULL, port, -1, 0, xzero, body, PAD_W, PAD_H);
    add(var, SMS_CTL_DPAD, "D-pad", NULL, port, -1, 0,
        cenx - DISC_R, ceny - DISC_R, 2 * DISC_R, 2 * DISC_R);
    /* the arms come after the disc: the hit test goes backwards */
    for (int arm = 0; arm < 4; arm++)
        add(var, SMS_CTL_DIRECTION, k_arm[arm], NULL, port,
            SMS_TARGET_PORT(port, SMS_ACT_UP + arm), 1u << (SMS_ACT_UP + arm),
            armx[arm], army[arm], ARM, ARM);
    add(var, SMS_CTL_BUTTON, "1", NULL, port, SMS_TARGET_PORT(port, SMS_ACT_1),
        1u << SMS_ACT_1, xzero + 4.2f, body + 1.3f, BTN, BTN);
    add(var, SMS_CTL_BUTTON, "2", NULL, port, SMS_TARGET_PORT(port, SMS_ACT_2),
        1u << SMS_ACT_2, xzero + 5.7f, body + 1.3f, BTN, BTN);
}

static void build_variant(variant *var, int console, int second_button)
{
    var->count = 0;
    add(var, SMS_CTL_CONSOLE, "Console", g_legend[console], -1, -1, 0, 0.0f, 0.0f, TOTAL_W, STRIP_H);
    if (second_button)
        add(var, SMS_CTL_SWITCH, console == SMS_CONSOLE_SMSJ ? "RAPID" : "RESET", NULL, -1,
            SMS_TARGET_SWITCH(SMS_SW_RESET), 0, RESET_X, CAP_Y, CAP_W, CAP_H);
    add(var, SMS_CTL_SWITCH, "PAUSE", NULL, -1, SMS_TARGET_SWITCH(SMS_SW_PAUSE), 0,
        PAUSE_X, CAP_Y, CAP_W, CAP_H);
    pad(var, 0, 0.0f);
    pad(var, 1, PAD_W + PAD_GAP);
}

/* One table per console: they differ in the wordmark and in the second
 * button (RESET, RAPID or none). */
static variant g_console[SMS_CONSOLE_COUNT];

static void build(void)
{
    for (int console = 0; console < SMS_CONSOLE_COUNT; console++) {
        const char *name = sms_console_name(console);
        size_t len = 0;

        len += (size_t)snprintf(g_legend[console], sizeof g_legend[console], "SEGA ");
        for (; name && *name && len + 1 < sizeof g_legend[console]; name++)
            g_legend[console][len++] = (char)toupper((unsigned char)*name);
        g_legend[console][len] = '\0';
        build_variant(&g_console[console], console, sms_console_has_reset_button(console));
    }
}

static void ensure_built(void)
{
    pthread_once(&g_once, build);
}

static variant *variant_for(int console)
{
    ensure_built();
    return &g_console[console >= 0 && console < SMS_CONSOLE_COUNT ? console : SMS_CONSOLE_SMS1];
}

const sms_control *smssession_controls_layout(int console, int *count, float *width, float *height)
{
    variant *var = variant_for(console);

    if (count) *count = var->count;
    if (width) *width = TOTAL_W;
    if (height) *height = TOTAL_H;
    return var->entry;
}

/* The sector (0 = RIGHT, counter-clockwise on screen) a vector lies in. */
static int sector_of(float delx, float dely)
{
    float deg = atan2f(-dely, delx) * 180.0f / PI_F;      /* +y is toward the player */
    int sector;

    if (deg < 0) deg += 360.0f;
    sector = (int)floorf((deg + 22.5f) / 45.0f);
    return sector & 7;
}

int smssession_controls_hit(int console, float xpos, float ypos)
{
    const variant *var = variant_for(console);

    for (int idx = var->count - 1; idx >= 0; idx--) {
        const sms_control *ctl = &var->entry[idx];

        switch (ctl->kind) {
        case SMS_CTL_DPAD: {
            const float delx = xpos - (ctl->left + ctl->width / 2);
            const float dely = ypos - (ctl->top + ctl->height / 2);

            if (delx * delx + dely * dely <= DISC_R * DISC_R)
                return idx;
            break;
        }
        case SMS_CTL_SWITCH:
        case SMS_CTL_DIRECTION:
        case SMS_CTL_BUTTON:
            if (xpos >= ctl->left && xpos < ctl->left + ctl->width
                && ypos >= ctl->top && ypos < ctl->top + ctl->height)
                return idx;
            break;
        default:                         /* decor */
            break;
        }
    }
    return -1;
}

unsigned smssession_dpad_from_vector(float delx, float dely, float radius)
{
    if (radius <= 0.0f)
        return 0;
    if (sqrtf(delx * delx + dely * dely) < radius * 0.25f)
        return 0;
    return k_sector_acts[sector_of(delx, dely)];
}

const char *smssession_switch_label(int console, int swnum)
{
    if (swnum == SMS_SW_PAUSE)
        return "PAUSE";
    if (swnum == SMS_SW_RESET && sms_console_has_reset_button(console))
        return console == SMS_CONSOLE_SMSJ ? "RAPID" : "RESET";
    return NULL;
}
