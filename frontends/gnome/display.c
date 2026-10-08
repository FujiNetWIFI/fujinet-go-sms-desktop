/*
 * display.c -- see display.h.
 *
 * Two things here are load bearing and neither is obvious:
 *
 *  - The frame clock is the emulator's clock. GTK4 presents on the
 *    compositor's vsync, so its tick callback is the most accurate ~60 Hz
 *    signal available, and feeding it to the session lets the emulator run
 *    one frame per tick instead of racing its own timer against the panel's.
 *    (A PAL console runs at 49.70 Hz: on a 60 Hz panel the ticks are not
 *    near its rate, and the session paces it on the wall clock instead.)
 *
 *  - Frames are pulled by serial, not pushed. copy_frame does nothing when
 *    the emulator has not produced a new frame, so a tick that arrives
 *    between frames costs a mutex and no memcpy.
 *
 * The VDP's frame is 268 x 224 on NTSC and 268 x 240 on PAL (MAME's visible
 * area: the 256 active columns with their borders); the texture is rebuilt
 * at the frame's own height each time, so a power cycle onto the other
 * standard needs nothing special here.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

#include <string.h>

#define FB_W SMSSESSION_FB_WIDTH
#define FB_MAX_H SMSSESSION_FB_MAX_HEIGHT

/* A television's pixel aspect: the VDP's dot clock against the TV's square-
 * pixel rate. 8:7 on NTSC; on PAL the 5.32 MHz dot clock against PAL's
 * 14.75 MHz square-pixel rate, halved. */
#define NTSC_PIXEL_ASPECT (8.0 / 7.0)
#define PAL_PIXEL_ASPECT (14.75 / 5.3203424 / 2.0)

struct _SMSDisplay {
    GtkWidget parent_instance;

    smssession *session;
    guint32 *fb;           /* XRGB8888 straight from the session */
    guint32 *bgra;         /* with alpha forced opaque for GdkMemoryTexture */
    int height;
    gboolean pal;          /* the frame came from a 50 Hz console */
    GdkTexture *texture;
    guint64 serial;
    guint tick_id;
    gboolean tv_aspect;
    gboolean smooth;
};

G_DEFINE_FINAL_TYPE(SMSDisplay, sms_display, GTK_TYPE_WIDGET)

static void rebuild_texture(SMSDisplay *self)
{
    GBytes *bytes;
    int i, n = FB_W * self->height;

    /* The session's pixels are 0x00RRGGBB; GDK_MEMORY_B8G8R8A8 on a little-endian
     * host reads the same bytes as B,G,R,A -- only the alpha needs setting. */
    for (i = 0; i < n; i++)
        self->bgra[i] = self->fb[i] | 0xFF000000u;

    bytes = g_bytes_new_static(self->bgra, (gsize)n * 4);
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_new(FB_W, self->height,
                                           GDK_MEMORY_B8G8R8A8, bytes,
                                           (gsize)FB_W * 4);
    g_bytes_unref(bytes);
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    SMSDisplay *self = SMS_DISPLAY(widget);
    int h = 0;
    (void)user_data;

    /* Hand the emulator the compositor's cadence. */
    smssession_notify_vsync(self->session,
                            gdk_frame_clock_get_frame_time(clock) * 1000);

    if (smssession_copy_frame(self->session, self->fb, &h, &self->serial)) {
        if (h > 0 && h <= FB_MAX_H) {
            self->height = h;
            self->pal = smssession_refresh_rate(self->session) == 50;
            rebuild_texture(self);
            gtk_widget_queue_draw(widget);
        }
    }
    return G_SOURCE_CONTINUE;
}

static void sms_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    SMSDisplay *self = SMS_DISPLAY(widget);
    int w = gtk_widget_get_width(widget);
    int h = gtk_widget_get_height(widget);
    double want, sw, sh, x, y;
    graphene_rect_t rect;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){ 0, 0, 0, 1 },
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w <= 0 || h <= 0 || self->height <= 0)
        return;

    /* A television showed each Master System pixel wider than it is tall:
     * 8:7 on NTSC, so the 268-pixel line spans about 306 square pixels at
     * 224 lines, and about 1.386:1 on PAL, about 371 across 240 lines. TV
     * mode shows that; square pixels show the raw buffer, which is useful
     * for pixel work and honest about nothing else. */
    want = (self->tv_aspect
                ? FB_W * (self->pal ? PAL_PIXEL_ASPECT : NTSC_PIXEL_ASPECT)
                : (double)FB_W)
           / (double)self->height;

    if ((double)w / (double)h > want) {
        sh = h;
        sw = sh * want;
    } else {
        sw = w;
        sh = sw / want;
    }
    x = (w - sw) / 2.0;
    y = (h - sh) / 2.0;

    graphene_rect_init(&rect, (float)x, (float)y, (float)sw, (float)sh);
    gtk_snapshot_append_scaled_texture(
        snapshot, self->texture,
        self->smooth ? GSK_SCALING_FILTER_LINEAR : GSK_SCALING_FILTER_NEAREST,
        &rect);
}

static void sms_display_dispose(GObject *object)
{
    SMSDisplay *self = SMS_DISPLAY(object);

    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_pointer(&self->fb, g_free);
    g_clear_pointer(&self->bgra, g_free);

    G_OBJECT_CLASS(sms_display_parent_class)->dispose(object);
}

static void sms_display_class_init(SMSDisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = sms_display_dispose;
    GTK_WIDGET_CLASS(klass)->snapshot = sms_display_snapshot;
}

static void sms_display_init(SMSDisplay *self)
{
    self->fb = g_new0(guint32, FB_W * FB_MAX_H);
    self->bgra = g_new0(guint32, FB_W * FB_MAX_H);
    self->tv_aspect = TRUE;
    self->smooth = FALSE;
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
}

GtkWidget *sms_display_new(smssession *session)
{
    SMSDisplay *self = g_object_new(SMS_TYPE_DISPLAY, NULL);
    self->session = session;
    self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick,
                                                 NULL, NULL);
    return GTK_WIDGET(self);
}

void sms_display_set_tv_aspect(SMSDisplay *self, gboolean tv)
{
    self->tv_aspect = tv;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void sms_display_set_smooth(SMSDisplay *self, gboolean smooth)
{
    self->smooth = smooth;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
