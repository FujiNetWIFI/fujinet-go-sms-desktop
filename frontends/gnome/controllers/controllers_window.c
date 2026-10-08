/*
 * SMSControllersWindow -- both Master System joypads side by side, each laid
 * out like the real one (the direction pad on the left, buttons 1 and 2 on
 * the right), the console's Pause and Reset buttons and Reset to CONFIG,
 * Map mode, and the gamepads' port assignments.
 *
 * Every control is pressed with a raw GtkGestureClick (press AND release),
 * not GtkButton's "clicked": the machine latches the joypads once per frame,
 * so a value present only for the instant of a click falls between frames
 * and a polling game never sees it. A button on screen is HELD for as long
 * as the mouse button is down, exactly like the plastic one.
 *
 * The gesture's "cancel" is wired to the same release path as "released":
 * dragging off a button must not leave the machine believing a button is
 * still down forever.
 *
 * What the console sees held -- keyboard, gamepads and this window together
 * (smssession_buttons_held, smssession_switch_held) -- is painted in the
 * accent colour, so the window doubles as an input tester for a new
 * gamepad.
 *
 * The Reset button is only there on a console that has one: the Master
 * System has Reset, the Japanese Master System has Rapid in its place, and
 * the Master System II and the Mark III have neither.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click any control to
 * pick the target, then press a keyboard key OR a gamepad button. While
 * armed, the same gestures pick the target instead of injecting input, the
 * key handler intercepts the next keystroke, and a timer polls the session's
 * gamepad capture. Rebinding steals the key from whatever held it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"

#include <string.h>

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the main window: system actions go through it */
static smssession *g_session;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button;
static GtkWidget *g_map_hint;
static guint g_capture_timer;
static guint g_held_timer;
static GtkWidget *g_pad_box;
static GtkWidget *g_reset_button;   /* the console's Reset (or Rapid) */
static int g_reset_console = -1;    /* the console it was last labelled for */
static unsigned g_pad_generation = (unsigned)-1;
static gboolean g_sysact_down[SMS_SYSACT_COUNT];

typedef struct {
    GtkWidget *button;
    int target;
} control;

static control g_controls[SMS_TARGET_COUNT];
static int g_ncontrols;

static void refresh_labels(void);

/* ---- pressing ------------------------------------------------------------- */

static void run_sysaction(int sa)
{
    if (g_parent)
        sms_window_run_sysaction(SMS_WINDOW(g_parent), sa);
    else
        smssession_sysaction(g_session, sa);
}

static void press_target(int target, int down)
{
    if (target >= SMS_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off should not power-cycle the cartridge. */
        if (!down)
            run_sysaction(target - SMS_TARGET_SYSACT(0));
        return;
    }
    smssession_press(g_session, target, down);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    smssession_gamepad_capture_cancel(g_session);
}

static gboolean capture_tick(gpointer d)
{
    int button;
    (void)d;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (smssession_gamepad_capture_poll(g_session, &button)) {
        char stolen[128], msg[200];
        smssession_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", sms_pad_button_name(button), stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", sms_pad_button_name(button));
        g_capture_timer = 0;
        g_map_state = -1;     /* stay armed: remapping several in a row is normal */
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        refresh_labels();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "sms-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "sms-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a control to remap");
    } else {
        char msg[128];
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s",
                   sms_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        smssession_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_labels();
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;

    if (g_map_state == -1) { set_map_state(target); return; }
    if (g_map_state >= 0) return;
    press_target(target, 1);
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;
    if (g_map_state != -2) return;
    press_target(target, 0);
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)seq;
    if (g_map_state != -2) return;
    press_target(target, 0);
}

static GtkWidget *control_button(const char *label, int target, int w, int h)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    GtkGesture *g = gtk_gesture_click_new();

    gtk_widget_set_size_request(b, w, h);
    g_signal_connect(g, "pressed", G_CALLBACK(on_pressed), GINT_TO_POINTER(target));
    g_signal_connect(g, "released", G_CALLBACK(on_released), GINT_TO_POINTER(target));
    g_signal_connect(g, "cancel", G_CALLBACK(on_cancelled), GINT_TO_POINTER(target));
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
    gtk_widget_set_focusable(b, FALSE);
    g_object_set_data_full(G_OBJECT(b), "face", g_strdup(label), g_free);

    if (g_ncontrols < SMS_TARGET_COUNT) {
        g_controls[g_ncontrols].button = b;
        g_controls[g_ncontrols].target = target;
        g_ncontrols++;
    }
    return b;
}

/* In Map mode each control shows what is bound to it, so choosing what to
 * change does not require remembering the whole table. */
static void refresh_labels(void)
{
    int i;
    for (i = 0; i < g_ncontrols; i++) {
        int t = g_controls[i].target;
        GtkWidget *b = g_controls[i].button;
        if (g_map_state != -2) {
            sms_binding bind = smssession_binding_get(g_session, t);
            char key[32], text[80];
            smssession_keysym_name(bind.keysym, key, sizeof key);
            if (bind.button != SMS_PAD_BTN_NONE)
                g_snprintf(text, sizeof text, "%s\n%s", *key ? key : "\xe2\x80\x94",
                           sms_pad_button_name(bind.button));
            else
                g_snprintf(text, sizeof text, "%s", *key ? key : "\xe2\x80\x94");
            gtk_button_set_label(GTK_BUTTON(b), text);
            if (t == g_map_state) gtk_widget_add_css_class(b, "sms-accent");
            else gtk_widget_remove_css_class(b, "sms-accent");
        } else {
            gtk_widget_remove_css_class(b, "sms-accent");
            gtk_button_set_label(GTK_BUTTON(b), g_object_get_data(G_OBJECT(b), "face"));
        }
    }
}

/* What the console sees held, lit in the accent colour. */
static gboolean held_tick(gpointer d)
{
    unsigned held[2];
    int i;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    if (g_map_state != -2)
        return G_SOURCE_CONTINUE;
    held[0] = smssession_buttons_held(g_session, 0);
    held[1] = smssession_buttons_held(g_session, 1);
    for (i = 0; i < g_ncontrols; i++) {
        int t = g_controls[i].target;
        gboolean on = FALSE;
        if (t < 2 * SMS_ACT_PER_PORT)
            on = (held[t / SMS_ACT_PER_PORT] >> (t % SMS_ACT_PER_PORT)) & 1;
        else if (t < SMS_TARGET_SYSACT(0))
            on = smssession_switch_held(g_session, t - SMS_TARGET_SWITCH(0)) != 0;
        if (on) gtk_widget_add_css_class(g_controls[i].button, "sms-accent");
        else gtk_widget_remove_css_class(g_controls[i].button, "sms-accent");
    }
    return G_SOURCE_CONTINUE;
}

/* ---- one joypad ----------------------------------------------------------- */

static GtkWidget *build_controller(int port)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 48);
    GtkWidget *cross = gtk_grid_new();
    GtkWidget *face = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *title, *one, *two;
    char label[32];

    g_snprintf(label, sizeof label, "Player %d", port + 1);
    title = gtk_label_new(label);
    gtk_widget_add_css_class(title, "heading");

    gtk_grid_set_row_spacing(GTK_GRID(cross), 2);
    gtk_grid_set_column_spacing(GTK_GRID(cross), 2);
    gtk_grid_attach(GTK_GRID(cross), control_button("\xe2\x96\xb2", SMS_TARGET_PORT(port, SMS_ACT_UP), 44, 40), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cross), control_button("\xe2\x97\x80", SMS_TARGET_PORT(port, SMS_ACT_LEFT), 44, 40), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(cross), control_button("\xe2\x96\xb6", SMS_TARGET_PORT(port, SMS_ACT_RIGHT), 44, 40), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(cross), control_button("\xe2\x96\xbc", SMS_TARGET_PORT(port, SMS_ACT_DOWN), 44, 40), 1, 2, 1, 1);
    gtk_widget_set_valign(cross, GTK_ALIGN_CENTER);

    /* The joypad's two round buttons, 1 then 2, side by side on the right. */
    one = control_button("1", SMS_TARGET_PORT(port, SMS_ACT_1), 56, 56);
    two = control_button("2", SMS_TARGET_PORT(port, SMS_ACT_2), 56, 56);
    gtk_widget_add_css_class(one, "circular");
    gtk_widget_add_css_class(two, "circular");
    gtk_widget_set_valign(one, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(two, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(face), one);
    gtk_box_append(GTK_BOX(face), two);
    gtk_widget_set_valign(face, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(body), cross);
    gtk_box_append(GTK_BOX(body), face);
    gtk_widget_set_halign(body, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), body);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 14);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

/* ---- the console's buttons ------------------------------------------------ */

/* Shown only on a console that has the button, and named for it there. */
static void sync_reset_button(void)
{
    int console = smssession_console(g_session);
    if (console == g_reset_console)
        return;
    g_reset_console = console;
    g_object_set_data_full(G_OBJECT(g_reset_button), "face",
                           g_strdup(console == SMS_CONSOLE_SMSJ ? "Rapid" : "Reset"),
                           g_free);
    gtk_widget_set_visible(g_reset_button, sms_console_has_reset_button(console) != 0);
    refresh_labels();
}

/* ---- gamepads ------------------------------------------------------------- */

static void on_assign_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    int idx = GPOINTER_TO_INT(user_data);
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    /* 0 = automatic (connection order), 1 = player 1, 2 = player 2 */
    smssession_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name and which player it drives. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Player 1", "Player 2", NULL };
    GtkWidget *child;
    int n, i;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);

    n = smssession_gamepad_count(g_session);
    if (n == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dd, *eff;
        char text[128], port[32];
        int assign = smssession_gamepad_assignment(g_session, i);
        int effective = smssession_gamepad_effective_port(g_session, i);

        smssession_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        dd = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), assign < 0 ? 0 : (guint)assign + 1);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(i));
        if (effective >= 0) g_snprintf(port, sizeof port, "drives player %d", effective + 1);
        else g_snprintf(port, sizeof port, "drives nothing");
        eff = gtk_label_new(port);
        gtk_widget_add_css_class(eff, "dim-label");
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), dd);
        gtk_box_append(GTK_BOX(g_pad_box), row);
    }
}

static gboolean pad_tick(gpointer d)
{
    unsigned gen;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    gen = smssession_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    }
    /* a power cycle out of Preferences can change the console under an
     * open window */
    sync_reset_button();
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    guint32 keysym = code >= 8 ? smssession_keysym_from_evdev(code - 8) : 0;
    int sa;
    (void)c; (void)st; (void)d;
    if (!keysym) keysym = keyval;

    if (g_map_state >= 0) {
        char stolen[128], msg[200], name[32];
        smssession_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
        smssession_keysym_name(keysym, name, sizeof name);
        set_map_state(-1);   /* stay armed: remapping several in a row is normal */
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", name, stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", name);
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        return TRUE;
    }
    if (g_map_state == -1) return TRUE;
    /* an open drop-down (a gamepad's player) has the arrows and Return */
    {
        GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
        if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
            return FALSE;
    }
    if (keyval == GDK_KEY_F9) {
        sms_controllers_window_toggle(NULL, g_session);
        return TRUE;
    }
    /* Otherwise behave exactly like the main window, so playing works
     * whichever window has focus: a system action first, on its leading
     * edge only (GTK4 has no repeat flag). */
    sa = smssession_key_sysaction(g_session, keysym);
    if (sa >= 0) {
        if (!g_sysact_down[sa]) {
            g_sysact_down[sa] = TRUE;
            run_sysaction(sa);
        }
        return TRUE;
    }
    return smssession_key(g_session, keysym, 1) ? TRUE : FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *c, guint keyval,
                                guint code, GdkModifierType st, gpointer d)
{
    guint32 keysym = code >= 8 ? smssession_keysym_from_evdev(code - 8) : 0;
    int sa;
    (void)c; (void)st; (void)d;
    if (!keysym) keysym = keyval;
    if (g_map_state != -2) return TRUE;
    sa = smssession_key_sysaction(g_session, keysym);
    if (sa >= 0) {
        g_sysact_down[sa] = FALSE;
        return TRUE;
    }
    return smssession_key(g_session, keysym, 0) ? TRUE : FALSE;
}

/* As in the main window: keys held into a focus change must not stay down. */
static void on_focus_leave(GtkEventControllerFocus *c, gpointer d)
{
    (void)c; (void)d;
    smssession_release_all(g_session);
    memset(g_sysact_down, 0, sizeof g_sysact_down);
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    smssession_bindings_reset(g_session);
    refresh_labels();
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)d;
    set_map_state(-2);
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    return TRUE;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *ports, *system, *maprow, *toolbar, *header, *defaults, *systitle, *padtitle;
    GtkEventController *keys, *focus;

    g_parent = parent;
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
    /* Same app-id as the main window, so the shell groups and icons it. */
    gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);

    g_ncontrols = 0;

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    ports = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(ports), build_controller(0));
    gtk_box_append(GTK_BOX(ports), build_controller(1));
    gtk_box_append(GTK_BOX(root), ports);

    systitle = gtk_label_new("Console");
    gtk_widget_add_css_class(systitle, "heading");
    gtk_box_append(GTK_BOX(root), systitle);
    system = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(system, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(system), control_button("Pause",
                   SMS_TARGET_SWITCH(SMS_SW_PAUSE), 120, 36));
    g_reset_button = control_button("Reset", SMS_TARGET_SWITCH(SMS_SW_RESET), 120, 36);
    gtk_box_append(GTK_BOX(system), g_reset_button);
    gtk_box_append(GTK_BOX(system), control_button("Reset to CONFIG",
                   SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG), 140, 36));
    gtk_box_append(GTK_BOX(root), system);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(root), g_pad_box);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_map_hint);
    gtk_box_append(GTK_BOX(root), maprow);

    header = adw_header_bar_new();
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    g_held_timer = g_timeout_add(33, held_tick, NULL);
    g_timeout_add(250, pad_tick, NULL);
    g_reset_console = -1;
    set_map_state(-2);
    sync_reset_button();
}

static void show_window(void)
{
    g_pad_generation = (unsigned)-1;
    pad_tick(NULL);
    gtk_window_present(GTK_WINDOW(g_window));
    pad_tick(NULL);
}

void sms_controllers_window_toggle(GtkWindow *parent, smssession *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);

    if (gtk_widget_get_visible(g_window)) {
        set_map_state(-2);
        gtk_widget_set_visible(g_window, FALSE);
    } else {
        show_window();
    }
}

gboolean sms_controllers_window_is_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
