/*
 * window.c -- the main window: the display, a header-bar menu, keyboard
 * capture, the console's buttons, gamepad hot-plug toasts and the FujiNet
 * status.
 *
 * Keyboard events are translated by hardware keycode (evdev, via the
 * session's HID table) rather than by GDK keyval, so a binding names the
 * physical key whatever Shift is doing and whatever layout is active -- the
 * same path the Qt, Win32 and AppKit frontends take, which is what lets one
 * tested table serve all four.
 *
 * The console's own buttons are machine keys, not menu accelerators: Return
 * is Pause and Backspace the Reset button through the bindings, so they can
 * be remapped and a joypad-only game still sees them. The menu items name
 * whatever key is bound to them now.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "display.h"
#include "fujilog.h"
#include "prefs.h"
#include "debugger/dbg_window.h"
#include "controllers/controllers_window.h"

#include <string.h>

/* How long a menu press of a console button is held: the machine latches
 * its inputs once a frame, so a press and release in the same instant would
 * fall between two frames and never be seen. Six frames is a firm press. */
#define TAP_MS 100

typedef struct {
    SMSWindow *self;
    int sw;                     /* sms_switch */
} TapCtx;

struct _SMSWindow {
    AdwApplicationWindow parent_instance;

    smssession *session;
    GtkWidget *display;
    GtkWidget *toast_overlay;
    GtkWidget *status;          /* the FujiNet link indicator */
    GtkWidget *status_dot;
    GMenu *console_menu;        /* the Console section, relabelled live */
    char console_labels[512];   /* what it last showed */
    guint status_id;
    guint sysact_id;
    guint tap_id[SMS_SW_COUNT];
    TapCtx tap[SMS_SW_COUNT];
    gboolean sysact_down[SMS_SYSACT_COUNT];
    unsigned pad_generation;
    gboolean fullscreen;
};

G_DEFINE_FINAL_TYPE(SMSWindow, sms_window, ADW_TYPE_APPLICATION_WINDOW)

void sms_window_toast(SMSWindow *self, const char *text)
{
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay),
                                adw_toast_new(text));
}

void sms_install_accent_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    char buf[512];
    if (done) return;
    done = TRUE;
    /* white on the Sega red: it reads better than black on this red */
    g_snprintf(buf, sizeof buf,
        ".sms-accent { background: #%06x; color: #ffffff; }\n"
        ".sms-accent:hover { background: #%06x; }\n"
        ".sms-accent-text { color: #%06x; font-weight: bold; }\n"
        ".sms-dot { border-radius: 6px; min-width: 12px; min-height: 12px; }\n"
        ".sms-dot-on { background: #%06x; }\n"
        ".sms-dot-off { background: #808080; }\n",
        SMSSESSION_ACCENT_RGB, SMSSESSION_ACCENT_RGB,
        SMSSESSION_ACCENT_RGB, SMSSESSION_ACCENT_RGB);
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, buf);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ---- input ---------------------------------------------------------------- */

static guint32 keysym_of(guint keyval, guint keycode)
{
    /* the hardware key first; a keyval only for keys evdev has no HID
     * usage for (rare: media keys) */
    guint32 k = keycode >= 8 ? smssession_keysym_from_evdev(keycode - 8) : 0;
    return k ? k : keyval;
}

void sms_window_run_sysaction(SMSWindow *self, int sa)
{
    switch (sa) {
    case SMS_SYSACT_RESET_CONFIG:
        smssession_sysaction(self->session, sa);
        sms_window_toast(self, "Back to the FujiNet CONFIG client");
        break;
    case SMS_SYSACT_SOFT_RESET:
        smssession_sysaction(self->session, sa);
        break;
    case SMS_SYSACT_DEBUG_STOP:
        /* the debugger window attaches, which stops the machine */
        sms_debugger_show(GTK_WINDOW(self), self->session);
        break;
    default:
        break;
    }
}

/* An open dialog (Preferences, About, an alert) or menu has the keyboard:
 * Escape closes it and the arrows and Return move through it, instead of
 * driving the machine. The key capture sits on the window, ahead of them. */
static gboolean ui_has_keyboard(SMSWindow *self)
{
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self));
    if (adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self)))
        return TRUE;
    return focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER) != NULL;
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    SMSWindow *self = user_data;
    guint32 keysym;
    int sa;
    (void)ctrl;

    if (ui_has_keyboard(self))
        return FALSE;

    /* The window's own keys, deliberately not bindable: they are how you
     * reach the panels that do the binding. */
    if (keyval == GDK_KEY_F9) {
        sms_controllers_window_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    if (keyval == GDK_KEY_F12) {
        sms_debugger_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    if (keyval == GDK_KEY_F11) {
        gtk_widget_activate_action(GTK_WIDGET(self), "win.fullscreen", NULL);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) || (state & GDK_ALT_MASK))
        return FALSE;   /* menu accelerators */

    keysym = keysym_of(keyval, keycode);
    /* A system action is checked BEFORE the machine keys, so Escape always
     * gets back to CONFIG whatever else the key table says. Leading edge
     * only: GTK4 has no repeat flag. */
    sa = smssession_key_sysaction(self->session, keysym);
    if (sa >= 0) {
        if (!self->sysact_down[sa]) {
            self->sysact_down[sa] = TRUE;
            sms_window_run_sysaction(self, sa);
        }
        return TRUE;
    }
    return smssession_key(self->session, keysym, 1) ? TRUE : FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                                guint keycode, GdkModifierType state,
                                gpointer user_data)
{
    SMSWindow *self = user_data;
    guint32 keysym;
    int sa;
    (void)ctrl; (void)state;

    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12)
        return TRUE;
    keysym = keysym_of(keyval, keycode);
    if (ui_has_keyboard(self)) {
        /* still let go of a machine key held into the dialog */
        smssession_key(self->session, keysym, 0);
        return FALSE;
    }
    sa = smssession_key_sysaction(self->session, keysym);
    if (sa >= 0) {
        self->sysact_down[sa] = FALSE;
        return TRUE;
    }
    return smssession_key(self->session, keysym, 0) ? TRUE : FALSE;
}

/* Losing focus with keys held would leave the machine believing they are
 * still down. */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer user_data)
{
    SMSWindow *self = user_data;
    (void)ctrl;
    smssession_release_all(self->session);
    memset(self->sysact_down, 0, sizeof self->sysact_down);
}

/* ---- the Console menu: labels that follow the bindings and the console ---- */

/* The console running now, not the setting: Preferences can be choosing
 * another one, which only powers up when it closes. */
static int current_console(SMSWindow *self)
{
    return smssession_console(self->session);
}

/* "Pause (Return)": the item's name and whatever key drives its target now
 * (a remap in the Controllers window shows here within a quarter second).
 * The mnemonics are unique across the whole menu. */
static void item_label(SMSWindow *self, char *dst, int dstsz, const char *name,
                       int target)
{
    sms_binding b = smssession_binding_get(self->session, target);
    char key[32];
    if (b.keysym && smssession_keysym_name(b.keysym, key, sizeof key) > 0)
        g_snprintf(dst, dstsz, "%s (%s)", name, key);
    else
        g_snprintf(dst, dstsz, "%s", name);
}

static void sync_console_menu(SMSWindow *self)
{
    char pause[96], reset[96], soft[96], config[96], all[512];
    int console = current_console(self);
    gboolean has_reset = sms_console_has_reset_button(console) != 0;
    GAction *a;

    item_label(self, pause, sizeof pause, "Pa_use", SMS_TARGET_SWITCH(SMS_SW_PAUSE));
    /* the Japanese console's button in that place is Rapid, not Reset */
    item_label(self, reset, sizeof reset,
               console == SMS_CONSOLE_SMSJ ? "_Rapid Button" : "_Reset Button",
               SMS_TARGET_SWITCH(SMS_SW_RESET));
    item_label(self, soft, sizeof soft, "_Soft Reset",
               SMS_TARGET_SYSACT(SMS_SYSACT_SOFT_RESET));
    item_label(self, config, sizeof config, "Reset to CO_NFIG",
               SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG));
    g_snprintf(all, sizeof all, "%s|%s|%s|%s|%d", pause, reset, soft, config, has_reset);

    a = g_action_map_lookup_action(G_ACTION_MAP(self), "reset-button");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(a), has_reset);

    if (strcmp(all, self->console_labels) == 0)
        return;
    g_strlcpy(self->console_labels, all, sizeof self->console_labels);
    g_menu_remove_all(self->console_menu);
    g_menu_append(self->console_menu, pause, "win.pause");
    g_menu_append(self->console_menu, reset, "win.reset-button");
    g_menu_append(self->console_menu, soft, "win.soft-reset");
    g_menu_append(self->console_menu, config, "win.reset-config");
}

/* The gamepad thread cannot call into GTK; it posts system actions and this
 * timer takes them. It also watches for gamepads coming and going, and says
 * so: a pad that drops off Bluetooth mid-game should not be a mystery. */
static gboolean sysact_drain_tick(gpointer user_data)
{
    SMSWindow *self = user_data;
    unsigned gen;
    int sa;
    while (smssession_sysaction_take(self->session, &sa))
        sms_window_run_sysaction(self, sa);

    gen = smssession_gamepad_generation(self->session);
    if (gen != self->pad_generation) {
        char text[160];
        self->pad_generation = gen;
        if (smssession_gamepad_last_event(self->session, text, sizeof text) > 0)
            sms_window_toast(self, text);
    }
    sync_console_menu(self);
    return G_SOURCE_CONTINUE;
}

/* ---- status --------------------------------------------------------------- */

static gboolean update_status(gpointer user_data)
{
    SMSWindow *self = user_data;
    char text[1200], st[160];
    gboolean on = FALSE;

    if (!smssession_is_running(self->session)) {
        g_snprintf(text, sizeof text, "Stopped");
    } else {
        const char *cart = smssession_cart_path(self->session);
        const char *slash = cart ? strrchr(cart, '/') : NULL;
        int link = smssession_cart_link_up(self->session);
        smssession_cart_status(self->session, st, sizeof st);
        on = link == 1;
        if (cart && *cart)
            g_snprintf(text, sizeof text, "%s \xe2\x80\x94 FujiNet %s",
                       slash ? slash + 1 : cart, st);
        else
            g_snprintf(text, sizeof text, "FujiNet %s", st);
    }
    gtk_label_set_text(GTK_LABEL(self->status), text);
    if (on) {
        gtk_widget_add_css_class(self->status_dot, "sms-dot-on");
        gtk_widget_remove_css_class(self->status_dot, "sms-dot-off");
    } else {
        gtk_widget_add_css_class(self->status_dot, "sms-dot-off");
        gtk_widget_remove_css_class(self->status_dot, "sms-dot-on");
    }
    return G_SOURCE_CONTINUE;
}

/* ---- cartridges and BIOSes ------------------------------------------------- */

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* A cartridge the FujiNet cartridge cannot run (a mapper its engine does
 * not have, an image too big for its SRAM) says why in a dialog: a toast
 * would vanish before the reason was read, and the reason is the whole
 * point. */
static void show_message(GtkWidget *over, const char *heading, const char *body)
{
    AdwDialog *dlg = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "ok", "OK");
    adw_dialog_present(dlg, over);
}

static void open_cart_path(SMSWindow *self, const char *path)
{
    char msg[1200];
    if (smssession_load_cart(self->session, path) != 0) {
        show_message(GTK_WIDGET(self), "Cannot Open Cartridge",
                     smssession_last_error(self->session));
        return;
    }
    g_snprintf(msg, sizeof msg, "Running %s", base_name(path));
    sms_window_toast(self, msg);
}

/* Import BIOS says what it did in every case -- which console the image
 * fits, when it boots, or that its CRC is not one MAME knows -- and that is
 * worth more than a toast's few seconds. */
int sms_show_bios_import(GtkWidget *over, smssession *session, const char *path)
{
    char msg[1024];
    int idx = smssession_import_bios(session, path, msg, sizeof msg);
    show_message(over, idx < 0 ? "Cannot Import BIOS" : "BIOS Imported",
                 msg[0] ? msg : smssession_last_error(session));
    return idx;
}

/* A dropped file: a BIOS is imported, a cartridge runs, anything else goes
 * where it belongs. The BIOS test comes first: BIOS dumps are .bin and .rom
 * too, and they are recognised by size and CRC, not by name. */
void sms_window_load_media(SMSWindow *self, const char *path)
{
    char dest[1024];

    if (smssession_media_is_bios(path)) {
        sms_show_bios_import(GTK_WIDGET(self), self->session, path);
        return;
    }
    if (smssession_media_is_cartridge(path)) {
        open_cart_path(self, path);
        return;
    }
    if (smssession_import_media(self->session, path, dest, sizeof dest) != 0) {
        sms_window_toast(self, smssession_last_error(self->session));
        return;
    }
    sms_window_toast(self, "Copied to FujiNet's SD folder \xe2\x80\x94 mount it "
                           "from the CONFIG client");
}

static GtkFileDialog *file_dialog(const char *title, const char *filter_name,
                                  const char *const *suffixes, const char *folder)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *images = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    int i;

    /* suffixes match case-insensitively: GAME.SMS is as good as game.sms */
    gtk_file_filter_set_name(images, filter_name);
    for (i = 0; suffixes[i]; i++)
        gtk_file_filter_add_suffix(images, suffixes[i]);
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, images);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    if (folder && *folder) {
        g_autoptr(GFile) dir = g_file_new_for_path(folder);
        gtk_file_dialog_set_initial_folder(dlg, dir);
    }
    g_object_unref(images);
    g_object_unref(all);
    g_object_unref(filters);
    return dlg;
}

static GtkFileDialog *cart_dialog(SMSWindow *self, const char *title)
{
    static const char *const exts[] = { "sms", "sg", "bin", "rom", NULL };
    const char *cart = smssession_cart_path(self->session);
    g_autofree char *dir = cart && *cart ? g_path_get_dirname(cart) : NULL;
    /* where the last cartridge came from: the next one is usually beside it */
    return file_dialog(title, "Master System cartridges (*.sms, *.sg, *.bin, *.rom)",
                       exts, dir);
}

static void on_cart_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    SMSWindow *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file) return;
    path = g_file_get_path(file);
    if (path) open_cart_path(self, path);
}

static void action_open(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    GtkFileDialog *dlg = cart_dialog(self, "Open Cartridge");
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_cart_chosen, self);
    g_object_unref(dlg);
}

static void on_sd_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    SMSWindow *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    char dest[1024], msg[1200];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    if (smssession_import_cart_to_sd(self->session, path, dest, sizeof dest) != 0) {
        sms_window_toast(self, smssession_last_error(self->session));
        return;
    }
    g_snprintf(msg, sizeof msg, "Copied to %s \xe2\x80\x94 boot it from the "
               "CONFIG client", dest);
    sms_window_toast(self, msg);
}

static void action_import_sd(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    GtkFileDialog *dlg = cart_dialog(self, "Import Cartridge to SD");
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_sd_chosen, self);
    g_object_unref(dlg);
}

static void on_bios_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    SMSWindow *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file) return;
    path = g_file_get_path(file);
    if (path) sms_show_bios_import(GTK_WIDGET(self), self->session, path);
}

static void action_import_bios(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    static const char *const exts[] = { "sms", "bin", "rom", "ic2", NULL };
    GtkFileDialog *dlg = file_dialog("Import BIOS",
        "BIOS and YM2413 patch ROM images (*.sms, *.bin, *.rom, *.ic2)", exts, NULL);
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_bios_chosen, self);
    g_object_unref(dlg);
}

static void action_eject(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    (void)a; (void)p;
    smssession_eject(self->session);
    sms_window_toast(self, "Cartridge ejected \xe2\x80\x94 back to CONFIG");
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x,
                        double y, gpointer user_data)
{
    SMSWindow *self = user_data;
    g_autofree char *path = NULL;
    (void)t; (void)x; (void)y;
    if (!G_VALUE_HOLDS(value, G_TYPE_FILE)) return FALSE;
    path = g_file_get_path(G_FILE(g_value_get_object(value)));
    if (!path) return FALSE;
    sms_window_load_media(self, path);
    return TRUE;
}

/* ---- the console and actions ---------------------------------------------- */

static gboolean tap_release(gpointer user_data)
{
    TapCtx *t = user_data;
    smssession_press(t->self->session, SMS_TARGET_SWITCH(t->sw), 0);
    t->self->tap_id[t->sw] = 0;
    return G_SOURCE_REMOVE;
}

/* Press a console button from the menu: held for TAP_MS, then let go. */
static void tap_switch(SMSWindow *self, int sw)
{
    if (self->tap_id[sw])
        g_source_remove(self->tap_id[sw]);
    self->tap[sw].self = self;
    self->tap[sw].sw = sw;
    smssession_press(self->session, SMS_TARGET_SWITCH(sw), 1);
    self->tap_id[sw] = g_timeout_add(TAP_MS, tap_release, &self->tap[sw]);
}

static void action_pause(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    tap_switch(user_data, SMS_SW_PAUSE);
}

static void action_reset_button(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    (void)a; (void)p;
    if (sms_console_has_reset_button(current_console(self)))
        tap_switch(self, SMS_SW_RESET);
}

static void action_soft_reset(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    sms_window_run_sysaction(user_data, SMS_SYSACT_SOFT_RESET);
}

static void action_reset_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    sms_window_run_sysaction(user_data, SMS_SYSACT_RESET_CONFIG);
}

static void action_controllers(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    (void)a; (void)p;
    sms_controllers_window_toggle(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    (void)a; (void)p;
    sms_debugger_toggle(GTK_WINDOW(self), self->session);
}

static void action_fullscreen(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    (void)a; (void)p;
    self->fullscreen = !self->fullscreen;
    if (self->fullscreen) gtk_window_fullscreen(GTK_WINDOW(self));
    else gtk_window_unfullscreen(GTK_WINDOW(self));
}

static void action_fujinet_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    (void)a; (void)p;
    if (!smssession_fujinet_running(self->session)) {
        sms_window_toast(self, "FujiNet is not running");
        return;
    }
    sms_fujiconfig_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    (void)a; (void)p;
    sms_fujilog_show(GTK_WINDOW(self), self->session);
}

/* Power-cycle onto the settings store's options (a full restart if FujiNet,
 * audio or the gamepads were switched). Preferences hands this to
 * sms_prefs_show() as its close callback. */
static void restart_session(SMSWindow *self)
{
    if (smssession_restart(self->session) != 0) {
        sms_window_toast(self, smssession_last_error(self->session));
        return;
    }
    self->console_labels[0] = '\0';     /* the console may have changed */
    sync_console_menu(self);
    sms_window_toast(self, "Machine options applied (power cycled)");
}

static void action_prefs(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    (void)a; (void)p;
    sms_prefs_show(self, self->session, restart_session);
}

static void action_quit(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    gtk_window_close(GTK_WINDOW(user_data));
}

static void action_about(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    AdwDialog *about;
    (void)a; (void)p;
    about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "FujiNet Go SMS");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), sms_icon_name());
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), SMS_VERSION_STRING);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "Thomas Cherryhomes");
    adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), "https://fujinet.online/");
    adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about),
        "https://github.com/FujiNetWIFI/fujinet-go-sms-desktop/issues");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
        "A Sega Master System with a built-in FujiNet. The emulator is MAME's "
        "Master System drivers (BSD-3-Clause) transposed to C, with floooh's "
        "z80.h (zlib), ymfm (BSD-3-Clause) for the YM2413, and the FujiNet "
        "firmware (GPL-3.0-or-later) for the FujiNet and its cartridge.");
    adw_dialog_present(about, GTK_WIDGET(self));
}

static void action_aspect(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    gboolean tv = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(tv));
    sms_display_set_tv_aspect(SMS_DISPLAY(self->display), tv);
    /* "aspect": 0 = the television's pixel aspect, 1 = square pixels */
    smssession_set_int(self->session, "aspect", tv ? 0 : 1);
}

static void action_smooth(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    SMSWindow *self = user_data;
    gboolean sm = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(sm));
    sms_display_set_smooth(SMS_DISPLAY(self->display), sm);
    smssession_set_int(self->session, "smooth", sm ? 1 : 0);
}

void sms_window_apply_aspect(SMSWindow *self, int aspect)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), "tv-aspect");
    sms_display_set_tv_aspect(SMS_DISPLAY(self->display), aspect == 0);
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(aspect == 0));
}

static const GActionEntry win_actions[] = {
    { "open", action_open, NULL, NULL, NULL, { 0 } },
    { "eject", action_eject, NULL, NULL, NULL, { 0 } },
    { "import-sd", action_import_sd, NULL, NULL, NULL, { 0 } },
    { "import-bios", action_import_bios, NULL, NULL, NULL, { 0 } },
    { "pause", action_pause, NULL, NULL, NULL, { 0 } },
    { "reset-button", action_reset_button, NULL, NULL, NULL, { 0 } },
    { "soft-reset", action_soft_reset, NULL, NULL, NULL, { 0 } },
    { "reset-config", action_reset_config, NULL, NULL, NULL, { 0 } },
    { "controllers", action_controllers, NULL, NULL, NULL, { 0 } },
    { "debugger", action_debugger, NULL, NULL, NULL, { 0 } },
    { "fullscreen", action_fullscreen, NULL, NULL, NULL, { 0 } },
    { "tv-aspect", action_aspect, NULL, "true", NULL, { 0 } },
    { "smooth", action_smooth, NULL, "false", NULL, { 0 } },
    { "fujinet-config", action_fujinet_config, NULL, NULL, NULL, { 0 } },
    { "fujinet-log", action_fujinet_log, NULL, NULL, NULL, { 0 } },
    { "prefs", action_prefs, NULL, NULL, NULL, { 0 } },
    { "about", action_about, NULL, NULL, NULL, { 0 } },
    { "quit", action_quit, NULL, NULL, NULL, { 0 } },
};

/* ---- construction --------------------------------------------------------- */

static GMenu *build_menu(SMSWindow *self)
{
    GMenu *menu = g_menu_new();
    GMenu *cart = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *fuji = g_menu_new();
    GMenu *app = g_menu_new();

    g_menu_append(cart, "_Open Cartridge...", "win.open");
    g_menu_append(cart, "_Eject Cartridge", "win.eject");
    g_menu_append(cart, "_Import Cartridge to SD...", "win.import-sd");
    g_menu_append(cart, "Import _BIOS...", "win.import-bios");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(cart));

    /* filled (and kept current) by sync_console_menu */
    self->console_menu = g_menu_new();
    g_menu_append_section(menu, "Console", G_MENU_MODEL(self->console_menu));

    g_menu_append(view, "_Controllers (F9)", "win.controllers");
    g_menu_append(view, "_Debugger (F12)", "win.debugger");
    g_menu_append(view, "_TV Aspect", "win.tv-aspect");
    g_menu_append(view, "S_mooth Scaling", "win.smooth");
    g_menu_append(view, "_Fullscreen (F11)", "win.fullscreen");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view));

    g_menu_append(fuji, "FujiNet _Web UI", "win.fujinet-config");
    g_menu_append(fuji, "Console _Log", "win.fujinet-log");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fuji));

    g_menu_append(app, "_Preferences", "win.prefs");
    g_menu_append(app, "_About FujiNet Go SMS", "win.about");
    g_menu_append(app, "_Quit", "win.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app));

    g_object_unref(cart);
    g_object_unref(view);
    g_object_unref(fuji);
    g_object_unref(app);
    return menu;
}

static void sms_window_dispose(GObject *object)
{
    SMSWindow *self = SMS_WINDOW(object);
    int i;
    if (self->status_id) {
        g_source_remove(self->status_id);
        self->status_id = 0;
    }
    if (self->sysact_id) {
        g_source_remove(self->sysact_id);
        self->sysact_id = 0;
    }
    for (i = 0; i < SMS_SW_COUNT; i++) {
        if (self->tap_id[i]) {
            g_source_remove(self->tap_id[i]);
            self->tap_id[i] = 0;
        }
    }
    g_clear_object(&self->console_menu);
    G_OBJECT_CLASS(sms_window_parent_class)->dispose(object);
}

static void sms_window_class_init(SMSWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = sms_window_dispose;
}

static void sms_window_init(SMSWindow *self)
{
    (void)self;
}

GtkWidget *sms_window_new(AdwApplication *app, smssession *session)
{
    SMSWindow *self = g_object_new(SMS_TYPE_WINDOW, "application", app, NULL);
    GtkWidget *box, *header, *menu_button, *toolbar, *status_box;
    GtkEventController *keys, *focus;
    g_autoptr(GMenu) menu = NULL;

    self->session = session;
    sms_install_accent_css();

    gtk_window_set_title(GTK_WINDOW(self), "FujiNet Go SMS");
    gtk_window_set_icon_name(GTK_WINDOW(self), sms_icon_name());
    /* 268x224 at 3x with NTSC's 8:7 pixels: the VDP's line at the
     * television's width, three times over. */
    gtk_window_set_default_size(GTK_WINDOW(self), 919, 672 + 46);

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);
    {
        static const char *const prefs_accels[] = { "<Control>comma", NULL };
        static const char *const open_accels[] = { "<Control>o", NULL };
        static const char *const reboot_accels[] = { "<Control>r", NULL };
        static const char *const quit_accels[] = { "<Control>q", NULL };
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.prefs", prefs_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.open", open_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.reset-config", reboot_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.quit", quit_accels);
    }

    header = adw_header_bar_new();
    menu = build_menu(self);
    menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_button), TRUE);
    gtk_widget_set_tooltip_text(menu_button, "Main Menu");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    self->status_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->status_dot, "sms-dot");
    gtk_widget_add_css_class(self->status_dot, "sms-dot-off");
    gtk_widget_set_valign(self->status_dot, GTK_ALIGN_CENTER);
    self->status = gtk_label_new("Starting...");
    gtk_widget_add_css_class(self->status, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(self->status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(status_box), self->status_dot);
    gtk_box_append(GTK_BOX(status_box), self->status);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), status_box);

    self->display = sms_display_new(session);
    sms_display_set_tv_aspect(SMS_DISPLAY(self->display),
        smssession_get_int(session, "aspect", 0) == 0);
    sms_display_set_smooth(SMS_DISPLAY(self->display),
        smssession_get_int(session, "smooth", 0) != 0);
    {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), "tv-aspect");
        g_simple_action_set_state(G_SIMPLE_ACTION(a),
            g_variant_new_boolean(smssession_get_int(session, "aspect", 0) == 0));
        a = g_action_map_lookup_action(G_ACTION_MAP(self), "smooth");
        g_simple_action_set_state(G_SIMPLE_ACTION(a),
            g_variant_new_boolean(smssession_get_int(session, "smooth", 0) != 0));
    }

    {
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
        g_signal_connect(drop, "drop", G_CALLBACK(on_drop), self);
        gtk_widget_add_controller(self->display, GTK_EVENT_CONTROLLER(drop));
    }

    self->toast_overlay = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), self->display);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->toast_overlay);
    gtk_box_append(GTK_BOX(box), toolbar);
    gtk_widget_set_vexpand(toolbar, TRUE);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), box);

    /* Capture on the WINDOW so input works no matter what has focus. */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), focus);

    self->status_id = g_timeout_add_seconds(1, update_status, self);
    /* The current pad set is the baseline: no toast for pads that were
     * already plugged in at launch. */
    self->pad_generation = smssession_gamepad_generation(session);
    self->sysact_id = g_timeout_add(250, sysact_drain_tick, self);
    sync_console_menu(self);
    update_status(self);

    /* SMS_OPEN_CONTROLLERS=1 / SMS_OPEN_DEBUGGER=1 / SMS_OPEN_SETTINGS=1
     * open those windows at launch, following the family's convention: the
     * way in when the app misbehaves before the menu is reachable. */
    {
        const char *env = g_getenv("SMS_OPEN_CONTROLLERS");
        if (env && *env && *env != '0')
            sms_controllers_window_toggle(GTK_WINDOW(self), session);
        env = g_getenv("SMS_OPEN_DEBUGGER");
        if (env && *env && *env != '0')
            sms_debugger_show(GTK_WINDOW(self), session);
        env = g_getenv("SMS_OPEN_SETTINGS");
        if (env && *env && *env != '0')
            sms_prefs_show(self, session, restart_session);
    }
    return GTK_WIDGET(self);
}
