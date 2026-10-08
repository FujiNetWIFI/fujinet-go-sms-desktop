/*
 * SMSDisplayView -- the framebuffer, on a CVDisplayLink.
 *
 * CVDisplayLink is this platform's frame clock -- the equivalent of
 * GdkFrameClock and DwmFlush -- and feeding it to the session is what lets
 * the emulator phase-lock to the panel instead of beating against it.
 *
 * Its callback runs on its OWN high-priority thread, not the main one. So it
 * does the two cheap things (hand over the tick, pull the frame) and then
 * asks AppKit to redraw on the main thread. Drawing from the callback thread
 * would be a use of AppKit off the main thread, which is undefined.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

/* CoreVideo hands out host time in mach_absolute_time units, which are NOT
 * nanoseconds on every machine -- the timebase ratio converts them. */
#include <mach/mach_time.h>

#include <stdlib.h>
#include <string.h>

/* A Master System pixel on a television: 8:7 on NTSC (the 5.37 MHz dot
 * clock against the 4:3 picture), and on PAL the 14.75 MHz square-pixel
 * rate over the 5.32 MHz dot clock, halved -- about 1.386. */
#define PAR_NTSC (8.0 / 7.0)
#define PAR_PAL  (14.75 / 5.3203424 / 2.0)

@implementation SMSDisplayView {
    smssession *_session;
    CVDisplayLinkRef _link;
    uint32_t *_fb;
    int _height;
    uint64_t _serial;
    CGContextRef _ctx;
    CGColorSpaceRef _cs;
    BOOL _tv;
    BOOL _smooth;
}

static CVReturn displayCallback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                const CVTimeStamp *out, CVOptionFlags flagsIn,
                                CVOptionFlags *flagsOut, void *ctx)
{
    (void)link; (void)out; (void)flagsIn; (void)flagsOut;
    SMSDisplayView *self = (__bridge SMSDisplayView *)ctx;
    [self tick:now->hostTime];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(smssession *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 919, 672)];
    if (!self) return nil;
    _session = session;
    _tv = YES;
    _smooth = NO;

    const size_t n = (size_t)SMSSESSION_FB_WIDTH * SMSSESSION_FB_MAX_HEIGHT;
    _fb = calloc(n, sizeof *_fb);
    _cs = CGColorSpaceCreateDeviceRGB();
    /* The session's pixels are 0x00RRGGBB in host order: a little-endian
     * 32-bit context with the alpha byte skipped reads them as they are. */
    _ctx = CGBitmapContextCreate(_fb, SMSSESSION_FB_WIDTH, SMSSESSION_FB_MAX_HEIGHT, 8,
                                 SMSSESSION_FB_WIDTH * 4, _cs,
                                 kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);

    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    CVDisplayLinkSetOutputCallback(_link, displayCallback, (__bridge void *)self);
    CVDisplayLinkStart(_link);
    return self;
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}

- (void)dealloc
{
    [self stop];
    if (_ctx) CGContextRelease(_ctx);
    if (_cs) CGColorSpaceRelease(_cs);
    free(_fb);
}

- (void)tick:(uint64_t)hostTime
{
    static double toNs = 0.0;
    if (toNs == 0.0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        toNs = (double)tb.numer / (double)tb.denom;
    }
    smssession_notify_vsync(_session, (int64_t)((double)hostTime * toNs));

    int h = 0;
    if (!smssession_copy_frame(_session, _fb, &h, &_serial)) return;
    _height = h;
    /* AppKit is main-thread only; the display link's callback is not. */
    dispatch_async(dispatch_get_main_queue(), ^{ [self setNeedsDisplay:YES]; });
}

- (void)setTvAspect:(BOOL)tv { _tv = tv; [self setNeedsDisplay:YES]; }
- (void)setSmooth:(BOOL)smooth { _smooth = smooth; [self setNeedsDisplay:YES]; }

- (BOOL)isOpaque { return YES; }

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];

    CGContextSetRGBFillColor(dc, 0, 0, 0, 1);
    CGContextFillRect(dc, b);
    if (_height <= 0) return;

    CGImageRef whole = CGBitmapContextCreateImage(_ctx);
    if (!whole) return;
    /* Only the lines the VDP produced (224 on NTSC, 240 on PAL). */
    CGImageRef img = CGImageCreateWithImageInRect(whole, CGRectMake(0, 0, SMSSESSION_FB_WIDTH, _height));
    CGImageRelease(whole);
    if (!img) return;

    /* On a television each Master System pixel is wider than tall (see
     * PAR_*); square pixels show the 268 columns as they are. */
    const double par = _tv ? (smssession_refresh_rate(_session) == 50 ? PAR_PAL : PAR_NTSC) : 1.0;
    const double want = (double)SMSSESSION_FB_WIDTH * par / (double)_height;
    double w = b.size.width, h = b.size.height, sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    CGContextSetInterpolationQuality(dc, _smooth ? kCGInterpolationHigh : kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake((w - sw) / 2, (h - sh) / 2, sw, sh), img);
    CGImageRelease(img);
}
@end
