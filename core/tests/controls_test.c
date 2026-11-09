/*
 * controls_test -- the on-screen controllers' layout, hit test and gesture
 * maths, pure: the console's top strip (PAUSE, and RESET or RAPID where the
 * model has one), Player 1's Control Pad on the left and Player 2's on the
 * right, each with a caption, a body, a D-pad disc with four arms and buttons
 * 1 and 2. The extents are the same for every console. No two pressable
 * controls overlap (but for the arms on the disc), every binding target is on
 * exactly one control, the hit test agrees with the arms and with
 * smssession_dpad_from_vector. No machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "smssession.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

#define EPS 1e-3f
#define BIT(act) (1u << (act))

static int pressable(const sms_control *ctl)
{
    return ctl->kind == SMS_CTL_SWITCH || ctl->kind == SMS_CTL_DIRECTION
        || ctl->kind == SMS_CTL_BUTTON || ctl->kind == SMS_CTL_DPAD;
}

static int overlap(const sms_control *one, const sms_control *two)
{
    return one->left < two->left + two->width - EPS && two->left < one->left + one->width - EPS
        && one->top < two->top + two->height - EPS && two->top < one->top + one->height - EPS;
}

static int inside(const sms_control *inner, const sms_control *outer)
{
    return inner->left >= outer->left - EPS && inner->top >= outer->top - EPS
        && inner->left + inner->width <= outer->left + outer->width + EPS
        && inner->top + inner->height <= outer->top + outer->height + EPS;
}

static int find_kind(const sms_control *lay, int count, int kind, int port, const char *label)
{
    for (int idx = 0; idx < count; idx++)
        if (lay[idx].kind == kind && lay[idx].port == port && (!label || !strcmp(lay[idx].label, label)))
            return idx;
    return -1;
}

static int count_kind(const sms_control *lay, int count, int kind, int port)
{
    int num = 0;
    for (int idx = 0; idx < count; idx++)
        if (lay[idx].kind == kind && lay[idx].port == port)
            num++;
    return num;
}

static void check_layout(int console, float base_w, float base_h)
{
    int count = 0, bad = 0;
    float wid = 0, hgt = 0;
    const sms_control *lay = smssession_controls_layout(console, &count, &wid, &hgt);
    const int has_second = sms_console_has_reset_button(console);
    const int pause = find_kind(lay, count, SMS_CTL_SWITCH, -1, "PAUSE");
    const int second = find_kind(lay, count, SMS_CTL_SWITCH, -1, console == SMS_CONSOLE_SMSJ ? "RAPID" : "RESET");
    const char *name = sms_console_name(console);
    char msg[200];

    snprintf(msg, sizeof msg, "%s: %d controls (%s)", name, 2 * 9 + 2 + has_second,
             has_second ? "strip, PAUSE, the second button, two pads of nine" : "strip, PAUSE, two pads of nine");
    check(lay != NULL && count == 2 * 9 + 2 + has_second, msg);

    snprintf(msg, sizeof msg, "%s: the same extent as every console", name);
    check(fabsf(wid - base_w) < EPS && fabsf(hgt - base_h) < EPS, msg);

    /* inside the extent, labelled, and no two pressable controls overlap
     * but an arm on its disc */
    for (int one = 0; one < count; one++) {
        if (lay[one].left < -EPS || lay[one].top < -EPS || lay[one].left + lay[one].width > wid + EPS
            || lay[one].top + lay[one].height > hgt + EPS) {
            printf("  (%s sits outside the extent)\n", lay[one].label);
            bad++;
        }
        if (!lay[one].label || !lay[one].label[0])
            bad++;
        for (int two = one + 1; two < count; two++) {
            const int arm_on_disc = (lay[one].kind == SMS_CTL_DPAD && lay[two].kind == SMS_CTL_DIRECTION
                                     && lay[one].port == lay[two].port);
            if (pressable(&lay[one]) && pressable(&lay[two]) && !arm_on_disc && overlap(&lay[one], &lay[two])) {
                printf("  (%s overlaps %s)\n", lay[one].label, lay[two].label);
                bad++;
            }
        }
    }
    snprintf(msg, sizeof msg, "%s: no two pressable controls overlap, and all sit inside the extent", name);
    check(bad == 0, msg);

    /* the console strip: a wordmark that names the model */
    {
        const int strip = find_kind(lay, count, SMS_CTL_CONSOLE, -1, NULL);
        char upper[80];
        size_t len = 0;

        for (const char *src = name; *src && len + 1 < sizeof upper; src++)
            upper[len++] = (char)((*src >= 'a' && *src <= 'z') ? *src - 32 : *src);
        upper[len] = '\0';
        snprintf(msg, sizeof msg, "%s: the strip is across the top and reads SEGA %s", name, upper);
        check(strip == 0 && lay[strip].legend && !strncmp(lay[strip].legend, "SEGA ", 5)
              && !strcmp(lay[strip].legend + 5, upper) && fabsf(lay[strip].width - wid) < EPS, msg);
    }

    /* PAUSE always; the second button only where the model has it, wired to
     * the switch, on the strip, left of PAUSE */
    snprintf(msg, sizeof msg, "%s: PAUSE is wired to the Pause switch and sits on the strip", name);
    check(pause >= 0 && lay[pause].target == SMS_TARGET_SWITCH(SMS_SW_PAUSE)
          && inside(&lay[pause], &lay[0]), msg);
    snprintf(msg, sizeof msg, "%s: %s", name, has_second ? "the second button is wired to the Reset switch, on the strip, left of PAUSE"
                                                      : "no RESET and no RAPID (this model has neither)");
    if (has_second)
        check(second >= 0 && lay[second].target == SMS_TARGET_SWITCH(SMS_SW_RESET)
              && inside(&lay[second], &lay[0]) && lay[second].left + lay[second].width <= lay[pause].left + EPS, msg);
    else
        check(second < 0 && find_kind(lay, count, SMS_CTL_SWITCH, -1, "RESET") < 0
              && find_kind(lay, count, SMS_CTL_SWITCH, -1, "RAPID") < 0
              && count_kind(lay, count, SMS_CTL_SWITCH, -1) == 1, msg);
    snprintf(msg, sizeof msg, "%s: smssession_switch_label agrees", name);
    check(!strcmp(smssession_switch_label(console, SMS_SW_PAUSE), "PAUSE")
          && (has_second ? (smssession_switch_label(console, SMS_SW_RESET) != NULL
                            && !strcmp(smssession_switch_label(console, SMS_SW_RESET), lay[second].label))
                         : smssession_switch_label(console, SMS_SW_RESET) == NULL), msg);

    /* the pads: Player 1 on the left, Player 2 on the right, each with a
     * caption, a body, a disc with four arms, and buttons 1 and 2 */
    {
        const int cap0 = find_kind(lay, count, SMS_CTL_CAPTION, 0, NULL);
        const int cap1 = find_kind(lay, count, SMS_CTL_CAPTION, 1, NULL);

        snprintf(msg, sizeof msg, "%s: Player 1 is on the left, Player 2 on the right", name);
        check(cap0 >= 0 && cap1 >= 0 && lay[cap0].left < lay[cap1].left
              && !strcmp(lay[cap0].label, "Player 1") && !strcmp(lay[cap1].label, "Player 2"), msg);
    }
    for (int port = 0; port < SMS_PLAYERS; port++) {
        const int body = find_kind(lay, count, SMS_CTL_PAD, port, NULL);
        const int disc = find_kind(lay, count, SMS_CTL_DPAD, port, NULL);
        int targets[SMS_ACT_PER_PORT] = {0}, good = 1;

        snprintf(msg, sizeof msg, "%s: pad %d has a caption, a body, a disc, 4 arms and 2 buttons", name, port + 1);
        check(body >= 0 && disc >= 0 && count_kind(lay, count, SMS_CTL_CAPTION, port) == 1
              && count_kind(lay, count, SMS_CTL_PAD, port) == 1 && count_kind(lay, count, SMS_CTL_DPAD, port) == 1
              && count_kind(lay, count, SMS_CTL_DIRECTION, port) == 4
              && count_kind(lay, count, SMS_CTL_BUTTON, port) == 2, msg);
        for (int idx = 0; idx < count; idx++) {
            const sms_control *ctl = &lay[idx];
            if (ctl->port != port)
                continue;
            if (ctl->kind == SMS_CTL_DIRECTION) {
                const unsigned mask = ctl->act_mask;
                if ((mask & (mask - 1)) || !mask)
                    good = 0;
                else {
                    const int act = (int)(mask == BIT(SMS_ACT_UP) ? SMS_ACT_UP : mask == BIT(SMS_ACT_DOWN) ? SMS_ACT_DOWN
                                          : mask == BIT(SMS_ACT_LEFT) ? SMS_ACT_LEFT : SMS_ACT_RIGHT);
                    if (ctl->target != SMS_TARGET_PORT(port, act))
                        good = 0;
                    targets[act]++;
                }
                if (idx < disc || !inside(ctl, &lay[disc]))
                    good = 0;     /* arms are after the disc, within its box */
            } else if (ctl->kind == SMS_CTL_BUTTON) {
                const int act = ctl->label[0] == '1' ? SMS_ACT_1 : SMS_ACT_2;
                if (ctl->target != SMS_TARGET_PORT(port, act) || ctl->act_mask != BIT(act))
                    good = 0;
                targets[act]++;
                if (!inside(ctl, &lay[body]))
                    good = 0;
            } else if (ctl->kind == SMS_CTL_DPAD) {
                if (ctl->target != -1 || ctl->act_mask != 0 || !inside(ctl, &lay[body]))
                    good = 0;
            }
        }
        for (int act = 0; act < SMS_ACT_PER_PORT; act++)
            if (targets[act] != 1)
                good = 0;
        snprintf(msg, sizeof msg, "%s: pad %d: every contact is on exactly one control, the arms within the disc's box",
                 name, port + 1);
        check(good, msg);
    }

    /* every pressable target appears exactly once: 12 contacts, PAUSE, [RESET] */
    {
        int seen[SMS_TARGET_COUNT] = {0}, good = 1, total = 0;

        for (int idx = 0; idx < count; idx++)
            if (lay[idx].target >= 0) {
                seen[lay[idx].target]++;
                total++;
            }
        for (int tgt = 0; tgt < SMS_TARGET_COUNT; tgt++) {
            const int want = tgt < SMS_TARGET_SWITCH(0) ? 1
                           : tgt == SMS_TARGET_SWITCH(SMS_SW_PAUSE) ? 1
                           : tgt == SMS_TARGET_SWITCH(SMS_SW_RESET) ? has_second : 0;
            if (seen[tgt] != want)
                good = 0;
        }
        snprintf(msg, sizeof msg, "%s: each of the %d targets on the window is on one control", name, 12 + 1 + has_second);
        check(good && total == 12 + 1 + has_second, msg);
    }

    /* the hit test: the middle of every pressable control hits it; the disc
     * is hit away from its arms (the centre cell, the diagonal corners) */
    {
        int good = 1;

        for (int idx = 0; idx < count; idx++) {
            const sms_control *ctl = &lay[idx];
            float xpos = ctl->left + ctl->width / 2, ypos = ctl->top + ctl->height / 2;
            const int hit_idx = smssession_controls_hit(console, xpos, ypos);
            int expect = pressable(ctl) ? idx : -1;

            if (ctl->kind == SMS_CTL_DPAD) {
                /* the centre cell is the disc's (the arms stop short of it) */
                expect = idx;
            }
            if (hit_idx != expect) {
                printf("  (%s: hit %d, want %d)\n", ctl->label, hit_idx, expect);
                good = 0;
            }
        }
        snprintf(msg, sizeof msg, "%s: the middle of every pressable control hits it, decor hits nothing pressable", name);
        check(good, msg);
    }
    for (int port = 0; port < SMS_PLAYERS; port++) {
        const int disc = find_kind(lay, count, SMS_CTL_DPAD, port, NULL);
        const float cenx = lay[disc].left + lay[disc].width / 2, ceny = lay[disc].top + lay[disc].height / 2;
        const float rad = lay[disc].width / 2;
        const float corner = rad * 0.7f;       /* 45 degrees, inside the dish, outside the arms */

        snprintf(msg, sizeof msg, "%s: pad %d: the disc is hit at its diagonal corners and misses outside its dish", name, port + 1);
        check(smssession_controls_hit(console, cenx + corner, ceny - corner) == disc
              && smssession_controls_hit(console, cenx - corner, ceny + corner) == disc
              && smssession_controls_hit(console, cenx + rad * 0.99f, ceny + rad * 0.99f) == -1
              && smssession_controls_hit(console, cenx, ceny + rad + 0.3f) != disc, msg);
        snprintf(msg, sizeof msg, "%s: pad %d: an arm beats the disc under it", name, port + 1);
        check(lay[smssession_controls_hit(console, cenx, ceny - 1.0f)].kind == SMS_CTL_DIRECTION
              && lay[smssession_controls_hit(console, cenx, ceny - 1.0f)].act_mask == BIT(SMS_ACT_UP)
              && lay[smssession_controls_hit(console, cenx + 1.0f, ceny)].act_mask == BIT(SMS_ACT_RIGHT)
              && lay[smssession_controls_hit(console, cenx, ceny + 1.0f)].act_mask == BIT(SMS_ACT_DOWN)
              && lay[smssession_controls_hit(console, cenx - 1.0f, ceny)].act_mask == BIT(SMS_ACT_LEFT), msg);
    }
    check(smssession_controls_hit(console, -1.0f, -1.0f) == -1
          && smssession_controls_hit(console, wid + 1, hgt + 1) == -1,
          "a point outside the layout hits nothing");
    check(smssession_controls_hit(console, 0.1f, 0.1f) == -1, "the strip itself is not pressable");
}

int main(void)
{
    int count = 0;
    float wid = 0, hgt = 0;
    const sms_control *lay = smssession_controls_layout(SMS_CONSOLE_SMS1, &count, &wid, &hgt);

    check(lay && count > 0 && wid > hgt, "the window is wider than it is tall");
    for (int console = 0; console < SMS_CONSOLE_COUNT; console++)
        check_layout(console, wid, hgt);

    /* which consoles have which second button */
    check(smssession_switch_label(SMS_CONSOLE_SMS1, SMS_SW_RESET) && !strcmp(smssession_switch_label(SMS_CONSOLE_SMS1, SMS_SW_RESET), "RESET")
          && smssession_switch_label(SMS_CONSOLE_SMS1_PAL, SMS_SW_RESET) && smssession_switch_label(SMS_CONSOLE_SMS2, SMS_SW_RESET) == NULL
          && smssession_switch_label(SMS_CONSOLE_SMS2_PAL, SMS_SW_RESET) == NULL
          && smssession_switch_label(SMS_CONSOLE_MARK3, SMS_SW_RESET) == NULL,
          "RESET on the Master System (both TVs); none on the Master System II or the Mark III");
    check(smssession_switch_label(SMS_CONSOLE_SMSJ, SMS_SW_RESET) && !strcmp(smssession_switch_label(SMS_CONSOLE_SMSJ, SMS_SW_RESET), "RAPID"),
          "RAPID, not RESET, on the Japanese console");
    check(smssession_switch_label(SMS_CONSOLE_SMS1, SMS_SW_COUNT) == NULL && smssession_switch_label(SMS_CONSOLE_SMS1, -1) == NULL,
          "no label out of range");

    /* an unknown console gets the Master System's layout */
    {
        int unk_count = 0;
        const sms_control *unk = smssession_controls_layout(99, &unk_count, NULL, NULL);
        check(unk == lay && unk_count == count && smssession_controls_layout(-1, NULL, NULL, NULL) == lay,
              "an unknown console gets the Master System's layout");
    }
    check(smssession_controls_layout(SMS_CONSOLE_SMS2, NULL, NULL, NULL) != lay,
          "and the Master System II its own list");

    /* the D-pad's gesture maths: nothing in the middle quarter, else 8 sectors of 45 degrees */
    check(smssession_dpad_from_vector(0, 0, 10) == 0 && smssession_dpad_from_vector(2, 0, 10) == 0
          && smssession_dpad_from_vector(1.5f, 1.5f, 10) == 0, "inside a quarter of the reach: nothing");
    check(smssession_dpad_from_vector(5, 0, 10) == BIT(SMS_ACT_RIGHT)
          && smssession_dpad_from_vector(-5, 0, 10) == BIT(SMS_ACT_LEFT)
          && smssession_dpad_from_vector(0, -5, 10) == BIT(SMS_ACT_UP)
          && smssession_dpad_from_vector(0, 5, 10) == BIT(SMS_ACT_DOWN),
          "right, left, up (up on the screen) and down (toward the player)");
    check(smssession_dpad_from_vector(5, -5, 10) == (BIT(SMS_ACT_RIGHT) | BIT(SMS_ACT_UP))
          && smssession_dpad_from_vector(-5, -5, 10) == (BIT(SMS_ACT_LEFT) | BIT(SMS_ACT_UP))
          && smssession_dpad_from_vector(5, 5, 10) == (BIT(SMS_ACT_RIGHT) | BIT(SMS_ACT_DOWN))
          && smssession_dpad_from_vector(-5, 5, 10) == (BIT(SMS_ACT_LEFT) | BIT(SMS_ACT_DOWN)),
          "the diagonals are two contacts");
    /* 22.5 degrees is the boundary between right and the diagonal */
    check(smssession_dpad_from_vector(9.3f, -3.0f, 10) == BIT(SMS_ACT_RIGHT)       /* 17.9 degrees */
          && smssession_dpad_from_vector(9.0f, -4.3f, 10) == (BIT(SMS_ACT_RIGHT) | BIT(SMS_ACT_UP)), /* 25.5 */
          "the sectors are 45 degrees wide, centred on the directions");
    check(smssession_dpad_from_vector(100, 0, 10) == BIT(SMS_ACT_RIGHT) && smssession_dpad_from_vector(5, 0, 0) == 0
          && smssession_dpad_from_vector(5, 0, -1) == 0,
          "past the reach still counts; no reach is nothing");
    /* the disc's arms agree with the gesture maths: toward an arm is that arm's direction */
    {
        int good = 1;
        for (int port = 0; port < SMS_PLAYERS; port++)
            for (int idx = 0; idx < count; idx++)
                if (lay[idx].kind == SMS_CTL_DIRECTION && lay[idx].port == port) {
                    const sms_control *disc = &lay[0];
                    for (int cand = 0; cand < count; cand++)
                        if (lay[cand].kind == SMS_CTL_DPAD && lay[cand].port == port)
                            disc = &lay[cand];
                    const float vecx = lay[idx].left + lay[idx].width / 2 - (disc->left + disc->width / 2);
                    const float vecy = lay[idx].top + lay[idx].height / 2 - (disc->top + disc->height / 2);
                    if (smssession_dpad_from_vector(vecx, vecy, 1.55f) != lay[idx].act_mask)
                        good = 0;
                }
        check(good, "dragging toward an arm selects exactly that arm's direction");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
