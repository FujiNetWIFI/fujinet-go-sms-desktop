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

/* ---- frame buffer pool ----------------------------------------------------
 * The session copies a frame straight into a pooled buffer and the texture
 * borrows that buffer: one copy per frame, no allocation in steady state
 * (a 250 KB malloc is an mmap/munmap pair every frame). The buffer comes
 * back to the pool when GSK drops the texture's bytes, possibly after the
 * widget is gone, so the pool is reference counted and each outstanding
 * buffer holds a reference. */
typedef struct {
    gatomicrefcount refs;
    GMutex lock;
    GPtrArray *spare;      /* idle guint32 buffers, FB_W * FB_MAX_H each */
} FramePool;

typedef struct {
    FramePool *pool;
    guint32 *px;
} FrameLoan;

#define POOL_KEEP 4

static FramePool *pool_new(void)
{
    FramePool *p = g_new0(FramePool, 1);
    g_atomic_ref_count_init(&p->refs);
    g_mutex_init(&p->lock);
    p->spare = g_ptr_array_new_with_free_func(g_free);
    return p;
}

static void pool_unref(FramePool *p)
{
    if (!g_atomic_ref_count_dec(&p->refs))
        return;
    g_ptr_array_free(p->spare, TRUE);
    g_mutex_clear(&p->lock);
    g_free(p);
}

static guint32 *pool_take(FramePool *p)
{
    guint32 *px = NULL;

    g_mutex_lock(&p->lock);
    if (p->spare->len > 0)
        px = g_ptr_array_steal_index(p->spare, p->spare->len - 1);
    g_mutex_unlock(&p->lock);
    return px ? px : g_malloc((gsize)FB_W * FB_MAX_H * sizeof(guint32));
}

static void pool_give(FramePool *p, guint32 *px)
{
    g_mutex_lock(&p->lock);
    if (p->spare->len < POOL_KEEP) {
        g_ptr_array_add(p->spare, px);
        px = NULL;
    }
    g_mutex_unlock(&p->lock);
    g_free(px);
}

static void loan_release(gpointer data)
{
    FrameLoan *loan = data;

    pool_give(loan->pool, loan->px);
    pool_unref(loan->pool);
    g_free(loan);
}

struct _SMSDisplay {
    GtkWidget parent_instance;

    smssession *session;
    FramePool *pool;
    int height;
    gboolean pal;          /* the frame came from a 50 Hz console */
    GdkTexture *texture;
    guint64 serial;
    guint tick_id;
    gulong suspended_id;   /* notify::suspended on the toplevel, while mapped */
    GtkWindow *toplevel;
    gboolean tv_aspect;
    gboolean smooth;
};

G_DEFINE_FINAL_TYPE(SMSDisplay, sms_display, GTK_TYPE_WIDGET)

/* Wrap px (XRGB8888 straight from the session, h lines) in the texture. The
 * previous texture, when it was the same size, goes in as the update source
 * so GSK can reuse its GPU texture instead of making a new one. The X byte is
 * ignored by GDK_MEMORY_B8G8R8X8, so the alpha never needs touching. Takes
 * ownership of px. */
static void publish_texture(SMSDisplay *self, guint32 *px, int h)
{
    GdkMemoryTextureBuilder *b = gdk_memory_texture_builder_new();
    FrameLoan *loan = g_new(FrameLoan, 1);
    GBytes *bytes;

    g_atomic_ref_count_inc(&self->pool->refs);
    loan->pool = self->pool;
    loan->px = px;
    bytes = g_bytes_new_with_free_func(px, (gsize)FB_W * h * sizeof(guint32),
                                       loan_release, loan);
    gdk_memory_texture_builder_set_bytes(b, bytes);
    gdk_memory_texture_builder_set_stride(b, (gsize)FB_W * 4);
    gdk_memory_texture_builder_set_width(b, FB_W);
    gdk_memory_texture_builder_set_height(b, h);
    gdk_memory_texture_builder_set_format(b, GDK_MEMORY_B8G8R8X8);
    if (self->texture && self->height == h) {
        cairo_rectangle_int_t all = { 0, 0, FB_W, h };
        cairo_region_t *region = cairo_region_create_rectangle(&all);

        gdk_memory_texture_builder_set_update_texture(b, self->texture);
        gdk_memory_texture_builder_set_update_region(b, region);
        cairo_region_destroy(region);
    }
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_builder_build(b);
    g_bytes_unref(bytes);
    g_object_unref(b);
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    SMSDisplay *self = SMS_DISPLAY(widget);
    guint32 *px = pool_take(self->pool);
    int h = 0;
    (void)user_data;

    /* Hand the emulator the compositor's cadence. */
    smssession_notify_vsync(self->session,
                            gdk_frame_clock_get_frame_time(clock) * 1000);

    if (smssession_copy_frame(self->session, px, &h, &self->serial) &&
        h > 0 && h <= FB_MAX_H) {
        self->pal = smssession_refresh_rate(self->session) == 50;
        publish_texture(self, px, h);
        self->height = h;
        gtk_widget_queue_draw(widget);
    } else {
        pool_give(self->pool, px);
    }
    return G_SOURCE_CONTINUE;
}

/* The tick runs only while somebody can see the result. A hidden or
 * suspended window gets no frame callbacks from the compositor anyway, but a
 * minimised one on some stacks keeps ticking, and every tick is a wakeup.
 * The emulator does not care: with no ticks the vsync stream goes stale
 * after a quarter second and the session paces on the wall clock, which is
 * what it does for a PAL console anyway. The first tick after the window
 * comes back copies whatever frame is newest. */
static void update_tick(SMSDisplay *self)
{
    gboolean want = gtk_widget_get_mapped(GTK_WIDGET(self)) &&
                    !(self->toplevel && gtk_window_is_suspended(self->toplevel));

    if (want && !self->tick_id)
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick,
                                                     NULL, NULL);
    else if (!want && self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
}

static void on_suspended(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj; (void)pspec;
    update_tick(SMS_DISPLAY(user_data));
}

static void sms_display_map(GtkWidget *widget)
{
    SMSDisplay *self = SMS_DISPLAY(widget);
    GtkRoot *root;

    GTK_WIDGET_CLASS(sms_display_parent_class)->map(widget);
    root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root)) {
        self->toplevel = GTK_WINDOW(root);
        self->suspended_id = g_signal_connect(self->toplevel, "notify::suspended",
                                              G_CALLBACK(on_suspended), self);
    }
    update_tick(self);
}

static void sms_display_unmap(GtkWidget *widget)
{
    SMSDisplay *self = SMS_DISPLAY(widget);

    if (self->toplevel && self->suspended_id)
        g_signal_handler_disconnect(self->toplevel, self->suspended_id);
    self->suspended_id = 0;
    self->toplevel = NULL;
    GTK_WIDGET_CLASS(sms_display_parent_class)->unmap(widget);
    update_tick(self);
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
    if (self->pool) {
        pool_unref(self->pool);
        self->pool = NULL;
    }

    G_OBJECT_CLASS(sms_display_parent_class)->dispose(object);
}

static void sms_display_class_init(SMSDisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = sms_display_dispose;
    GTK_WIDGET_CLASS(klass)->snapshot = sms_display_snapshot;
    GTK_WIDGET_CLASS(klass)->map = sms_display_map;
    GTK_WIDGET_CLASS(klass)->unmap = sms_display_unmap;
}

static void sms_display_init(SMSDisplay *self)
{
    self->pool = pool_new();
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
