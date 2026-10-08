/*
 * The Preferences dialog: a programmatic AdwPreferencesDialog, no .ui file.
 *
 * The analog switch, the volume and the aspect apply LIVE. Everything else
 * is read when the machine powers up: the console, its BIOS and the FM
 * options are the console you would switch off to change, and the host
 * options (FujiNet, audio, gamepads) are what runs beside it. Those mark
 * the dialog dirty, and closing it power-cycles the session once
 * (smssession_restart, which restarts FujiNet and the devices only if one
 * of their switches changed) -- an opened cartridge boots again on the new
 * console.
 *
 * No BIOS is ever needed: the BIOS row offers "None" and whatever the user
 * has imported that fits the chosen console, and Import BIOS sits beside
 * it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "fujilog.h"
#include "window.h"

#include <string.h>

#define MAX_BIOS_CHOICES 16

typedef struct {
    SMSWindow *window;
    smssession *session;
    void (*restart)(SMSWindow *window);
    gboolean dirty;
    AdwDialog *dialog;
    GtkWidget *pad_group;
    GtkWidget *pad_rows[8];
    int pad_row_count;
    unsigned pad_generation;
    guint pad_timer;

    /* the rows that follow the console */
    GtkWidget *bios_row;
    const char *bios_names[MAX_BIOS_CHOICES];   /* by position; "" = none */
    int bios_count;
    gboolean bios_syncing;
    GtkWidget *fm_unit_row, *fm_mutes_row, *fm_builtin_row, *patch_row;
} PrefsState;

/* What a row does when it changes. */
enum {
    ROW_RESTART = -1,   /* read at power-up: power-cycle on close */
    ROW_ANALOG = -2,    /* live */
    ROW_CONSOLE = -3,   /* power-cycle on close, and the BIOS/FM rows follow */
    ROW_ASPECT = -4,    /* live */
};

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
    int kind;
} RowBinding;

static void binding_free(gpointer p, GClosure *closure)
{
    (void)closure;
    g_free(p);
}

static RowBinding *binding_new(PrefsState *state, const char *key, int def, int kind)
{
    RowBinding *b = g_new0(RowBinding, 1);
    b->state = state;
    b->key = key;
    b->def = def;
    b->kind = kind;
    return b;
}

static const char *aspect_name(int i)
{
    static const char *const names[] = { "Television", "Square pixels", NULL };
    return (i >= 0 && i < 2) ? names[i] : NULL;
}

static int prefs_console(PrefsState *state)
{
    int c = smssession_get_int(state->session, "console", SMS_CONSOLE_SMS1);
    return (c >= 0 && c < SMS_CONSOLE_COUNT) ? c : SMS_CONSOLE_SMS1;
}

/* ---- the console's BIOS and FM rows ---------------------------------------- */

static void refresh_machine_rows(PrefsState *state)
{
    const int console = prefs_console(state);
    const char *chosen = smssession_console_bios(state->session, console);
    const int n = smssession_bios_count();
    const int patch = smssession_bios_find("ym2413");
    GtkStringList *model = gtk_string_list_new(NULL);
    guint sel = 0;
    gboolean found = FALSE;
    int i;

    state->bios_count = 0;
    if (!sms_console_has_bios_socket(console)) {
        state->bios_names[state->bios_count++] = "";
        gtk_string_list_append(model, "None (no BIOS socket: the cartridge boots directly)");
    } else {
        state->bios_names[state->bios_count++] = "";
        gtk_string_list_append(model, "None (boot the cartridge)");
        /* every imported image that fits, and the custom one (index n) */
        for (i = 0; i <= n && state->bios_count < MAX_BIOS_CHOICES; i++) {
            const sms_bios_info *b = smssession_bios_info(i);
            if (!b || !((b->consoles >> console) & 1) ||
                !smssession_bios_available(state->session, i))
                continue;
            if (chosen && strcmp(chosen, b->name) == 0) {
                sel = (guint)state->bios_count;
                found = TRUE;
            }
            state->bios_names[state->bios_count++] = b->name;
            gtk_string_list_append(model, b->desc);
        }
        /* chosen once, since deleted from the ROM folder: say so rather than
         * pretend the choice is "None" (the console boots without it) */
        if (chosen && *chosen && !found && state->bios_count < MAX_BIOS_CHOICES) {
            const sms_bios_info *b = smssession_bios_info(smssession_bios_find(chosen));
            char text[160];
            if (b) {
                g_snprintf(text, sizeof text, "%s (not imported)", b->desc);
                sel = (guint)state->bios_count;
                state->bios_names[state->bios_count++] = b->name;
                gtk_string_list_append(model, text);
            }
        }
    }
    state->bios_syncing = TRUE;
    adw_combo_row_set_model(ADW_COMBO_ROW(state->bios_row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(state->bios_row), sel);
    state->bios_syncing = FALSE;
    g_object_unref(model);

    gtk_widget_set_visible(state->fm_unit_row, console == SMS_CONSOLE_MARK3);
    gtk_widget_set_visible(state->fm_mutes_row, console == SMS_CONSOLE_MARK3);
    gtk_widget_set_visible(state->fm_builtin_row, console == SMS_CONSOLE_SMSJ);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(state->patch_row),
        patch >= 0 && smssession_bios_available(state->session, patch)
            ? "Imported: the YM2413 plays the real chip's instruments"
            : "Not imported: the YM2413 uses ymfm's own instrument table "
              "(close, not identical). Import BIOS takes it too.");
}

static void bios_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    guint sel = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    const int console = prefs_console(state);
    const char *name;
    (void)pspec;
    if (state->bios_syncing || sel >= (guint)state->bios_count)
        return;
    name = state->bios_names[sel];
    if (strcmp(smssession_console_bios(state->session, console), name) == 0)
        return;
    smssession_set_console_bios(state->session, console, name);
    state->dirty = TRUE;
}

static void on_bios_file(GObject *src, GAsyncResult *res, gpointer user_data)
{
    AdwDialog *dialog = user_data;
    PrefsState *state = g_object_get_data(G_OBJECT(dialog), "prefs-state");
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;

    /* the dialog may have closed while the file chooser was up */
    if (state && file && (path = g_file_get_path(file)) != NULL) {
        const int console = prefs_console(state);
        g_autofree char *before = g_strdup(smssession_console_bios(state->session, console));
        if (sms_show_bios_import(GTK_WIDGET(dialog), state->session, path) >= 0) {
            if (strcmp(before, smssession_console_bios(state->session, console)) != 0)
                state->dirty = TRUE;
            refresh_machine_rows(state);
        }
    }
    g_object_unref(dialog);
}

static void on_import_bios(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *images = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    (void)b;

    gtk_file_filter_set_name(images, "BIOS and YM2413 patch ROM images (*.sms, *.bin, *.rom, *.ic2)");
    gtk_file_filter_add_suffix(images, "sms");
    gtk_file_filter_add_suffix(images, "bin");
    gtk_file_filter_add_suffix(images, "rom");
    gtk_file_filter_add_suffix(images, "ic2");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, images);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Import BIOS");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, GTK_WINDOW(state->window), NULL, on_bios_file,
                         g_object_ref(state->dialog));
    g_object_unref(images);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

/* ---- generic rows ------------------------------------------------------------ */

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (b->kind == ROW_ASPECT) {
        smssession_set_int(b->state->session, b->key, sel);
        sms_window_apply_aspect(b->state->window, sel);
        return;
    }
    if (smssession_get_int(b->state->session, b->key, b->def) == sel)
        return;
    smssession_set_int(b->state->session, b->key, sel);
    b->state->dirty = TRUE;
    if (b->kind == ROW_CONSOLE)
        refresh_machine_rows(b->state);
}

static void analog_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)row; (void)pspec;
    smssession_set_analog(state->session,
        smssession_get_int(state->session, "analog_joystick", 1));
}

static void switch_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;
    if (smssession_get_int(b->state->session, b->key, b->def) == on)
        return;
    smssession_set_int(b->state->session, b->key, on);
    if (b->kind == ROW_ANALOG)          /* an analog switch: live */
        analog_changed(row, NULL, b->state);
    else
        b->state->dirty = TRUE;
}

static void volume_changed(GtkRange *range, gpointer user_data)
{
    PrefsState *state = user_data;
    smssession_set_volume(state->session, (int)gtk_range_get_value(range));
}

static GtkWidget *combo_row(PrefsState *state, const char *title,
                            const char *subtitle, const char *key, int def,
                            const char *(*name_fn)(int), int kind)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(NULL);
    int i;

    for (i = 0; name_fn(i); i++)
        gtk_string_list_append(model, name_fn(i));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row),
        (guint)smssession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          binding_new(state, key, def, kind), binding_free, 0);
    g_object_unref(model);
    return row;
}

static GtkWidget *switch_row(PrefsState *state, const char *title,
                             const char *subtitle, const char *key, int def,
                             int live)
{
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              smssession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_changed),
                          binding_new(state, key, def, live ? ROW_ANALOG : ROW_RESTART),
                          binding_free, 0);
    return row;
}

static GtkWidget *info_row(const char *title, const char *subtitle)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    return row;
}

/* ---- gamepads: one row per pad, with its port assignment ------------------- */

static void pad_assign_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(row, "pad-index"));
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    smssession_gamepad_assign(state->session, idx, sel - 1);
}

static void rebuild_pad_rows(PrefsState *state)
{
    static const char *const choices[] = { "Automatic", "Player 1", "Player 2", NULL };
    int i, n = smssession_gamepad_count(state->session);
    char name[128], sub[64];

    for (i = 0; i < state->pad_row_count; i++)
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(state->pad_group), state->pad_rows[i]);
    state->pad_row_count = 0;

    if (n == 0) {
        GtkWidget *row = info_row("No gamepads connected",
                                  "Plug one in: it is picked up as it appears");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        return;
    }
    for (i = 0; i < n && i < 8; i++) {
        GtkWidget *row = adw_combo_row_new();
        GtkStringList *model = gtk_string_list_new(choices);
        int eff = smssession_gamepad_effective_port(state->session, i);
        smssession_gamepad_name(state->session, i, name, sizeof name);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        if (eff >= 0)
            g_snprintf(sub, sizeof sub, "Driving player %d", eff + 1);
        else
            g_snprintf(sub, sizeof sub, "Driving nothing");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
        adw_combo_row_set_selected(ADW_COMBO_ROW(row),
            (guint)(smssession_gamepad_assignment(state->session, i) + 1));
        g_object_set_data(G_OBJECT(row), "pad-index", GINT_TO_POINTER(i));
        g_signal_connect(row, "notify::selected", G_CALLBACK(pad_assign_changed), state);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        g_object_unref(model);
    }
}

static gboolean pad_tick(gpointer user_data)
{
    PrefsState *state = user_data;
    unsigned gen = smssession_gamepad_generation(state->session);
    if (gen != state->pad_generation) {
        state->pad_generation = gen;
        rebuild_pad_rows(state);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- FujiNet ------------------------------------------------------------------ */

static void on_open_webui(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)b;
    if (!smssession_fujinet_running(state->session)) {
        adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(state->dialog),
                                         adw_toast_new("FujiNet is not running"));
        return;
    }
    sms_fujiconfig_show(GTK_WINDOW(state->window), state->session);
}

/* ---- the dialog -------------------------------------------------------------- */

static void prefs_closed(AdwDialog *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    g_object_set_data(G_OBJECT(dialog), "prefs-state", NULL);
    if (state->pad_timer) g_source_remove(state->pad_timer);
    if (state->dirty && state->restart)
        state->restart(state->window);
    g_free(state);
}

void sms_prefs_show(SMSWindow *parent, smssession *session,
                    void (*restart)(SMSWindow *parent))
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *machine = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *display = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *audio = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *input = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *pads = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *fujinet = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *import;

    state->window = parent;
    state->session = session;
    state->restart = restart;
    state->dialog = ADW_DIALOG(dialog);
    state->pad_group = GTK_WIDGET(pads);
    g_object_set_data(G_OBJECT(dialog), "prefs-state", state);

    adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 600);
    adw_preferences_page_set_title(page, "Machine");
    adw_preferences_page_set_icon_name(page, "applications-games-symbolic");

    /* ---- the machine: applied by a power cycle when the dialog closes */
    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_description(machine,
        "Applied when Preferences closes, with a power cycle: an opened "
        "cartridge boots again on the new console");
    adw_preferences_group_add(machine,
        combo_row(state, "Console", NULL, "console", SMS_CONSOLE_SMS1,
                  sms_console_name, ROW_CONSOLE));

    /* the choice shows as the subtitle: BIOS descriptions are long */
    state->bios_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(state->bios_row), "BIOS");
    adw_combo_row_set_use_subtitle(ADW_COMBO_ROW(state->bios_row), TRUE);
    import = gtk_button_new_with_label("Import BIOS\xe2\x80\xa6");
    gtk_widget_set_valign(import, GTK_ALIGN_CENTER);
    g_signal_connect(import, "clicked", G_CALLBACK(on_import_bios), state);
    adw_action_row_add_suffix(ADW_ACTION_ROW(state->bios_row), import);
    g_signal_connect(state->bios_row, "notify::selected", G_CALLBACK(bios_changed), state);
    adw_preferences_group_add(machine, state->bios_row);

    state->fm_unit_row = switch_row(state, "FM Sound Unit",
        "The Mark III's optional YM2413 add-on is fitted", "fm_unit", 1, 0);
    state->fm_mutes_row = switch_row(state, "FM mutes the PSG",
        "Only the YM2413 is heard while a game has the FM switched on",
        "fm_unit_mutes_psg", 0, 0);
    state->fm_builtin_row = info_row("FM sound",
        "YM2413 built in: the Japanese Master System always has it");
    state->patch_row = info_row("YM2413 instrument ROM", NULL);
    adw_preferences_group_add(machine, state->fm_unit_row);
    adw_preferences_group_add(machine, state->fm_mutes_row);
    adw_preferences_group_add(machine, state->fm_builtin_row);
    adw_preferences_group_add(machine, state->patch_row);
    refresh_machine_rows(state);

    /* ---- the picture: live */
    adw_preferences_group_set_title(display, "Display");
    adw_preferences_group_add(display,
        combo_row(state, "Aspect", "How the 268 x 224 picture (240 lines on PAL) "
                  "is shaped: a television's wide pixels (8:7 on NTSC), or square "
                  "ones", "aspect", 0, aspect_name, ROW_ASPECT));

    /* ---- audio */
    adw_preferences_group_set_title(audio, "Audio");
    adw_preferences_group_add(audio,
        switch_row(state, "Enable audio", "Open the system audio device "
                   "(applied when Preferences closes)", "enable_audio", 1, 0));
    {
        GtkWidget *row = adw_action_row_new();
        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Volume");
        gtk_range_set_value(GTK_RANGE(scale), smssession_get_int(session, "volume", 100));
        gtk_widget_set_size_request(scale, 200, -1);
        gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
        g_signal_connect(scale, "value-changed", G_CALLBACK(volume_changed), state);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), scale);
        adw_preferences_group_add(audio, row);
    }

    /* ---- input */
    adw_preferences_group_set_title(input, "Input");
    adw_preferences_group_add(input,
        switch_row(state, "Enable gamepads", "Poll USB/Bluetooth gamepads "
                   "(applied when Preferences closes)", "enable_gamepad", 1, 0));
    adw_preferences_group_add(input,
        switch_row(state, "Left stick drives the D-pad",
                   "Off: only a gamepad's D-pad moves the joypad's direction "
                   "pad", "analog_joystick", 1, 1));

    adw_preferences_group_set_title(pads, "Gamepads");
    adw_preferences_group_set_description(pads,
        "Assigned to ports in connection order unless chosen here");
    rebuild_pad_rows(state);
    state->pad_generation = smssession_gamepad_generation(session);

    /* ---- FujiNet */
    adw_preferences_group_set_title(fujinet, "FujiNet");
    adw_preferences_group_add(fujinet,
        switch_row(state, "Enable FujiNet",
                   "Run the in-process FujiNet the cartridge dials into. Off "
                   "means no network and a link-down CONFIG (applied when "
                   "Preferences closes).",
                   "enable_fujinet", 1, 0));
    {
        GtkWidget *row = info_row("Web UI", smssession_fujinet_webui_url(session));
        GtkWidget *open = gtk_button_new_with_label("Open");
        gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
        g_signal_connect(open, "clicked", G_CALLBACK(on_open_webui), state);
        adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);
        adw_preferences_group_add(fujinet, row);
    }

    adw_preferences_page_add(page, machine);
    adw_preferences_page_add(page, display);
    adw_preferences_page_add(page, audio);
    adw_preferences_page_add(page, input);
    adw_preferences_page_add(page, pads);
    adw_preferences_page_add(page, fujinet);
    adw_preferences_dialog_add(dialog, page);
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    state->pad_timer = g_timeout_add(1000, pad_tick, state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
