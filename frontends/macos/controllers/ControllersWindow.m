/*
 * SMSControllersWindow -- the AppKit Controllers panel: the Master System's
 * console strip (SEGA wordmark, PAUSE and, on the models that have it, RESET
 * or RAPID) and its two Control Pads, drawn from the session's layout
 * (smssession_controls_layout) with NSBezierPath, so every frontend shows the
 * same console; the session's own actions; the gamepads and the player each
 * drives; and Map mode.
 *
 * Each pad is a black body with a D-pad in a round dish (four arms and a
 * centre) and two square buttons, 1 and 2. One custom-drawn view
 * (SMSConsoleView) paints the console and takes the mouse; the layout, the
 * exact hit shapes and the drag maths are the core's.
 *
 * Gestures: a press on a D-pad arm holds that direction, a press on the disc
 * holds the direction the pointer is in (eight ways, nothing near the
 * middle) and follows the drag; a press on button 1 or 2, or on a console
 * button, holds it. The pointer is its own input source
 * (smssession_pointer_set), so the panel lets go of everything the mouse
 * holds when it is hidden, rebuilt or loses key status.
 *
 * What the console sees held -- host keyboard, gamepads and the mouse -- is
 * painted in the accent, so the panel doubles as an input tester for a new
 * gamepad. Keys typed with the panel key go to the console as in the main
 * window; no view here takes the keyboard, so the panel itself is the first
 * responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents (the
 * layout's extent is the same for every console, so a console change never
 * resizes it). Hidden at start, ordered out rather than closed, so its
 * position survives. The 30 Hz poll runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture or
 * choose one from the list (all SMS_TARGET_COUNT targets, the app's own
 * actions among them), then press a key or a gamepad button; the next one is
 * bound and whatever held it is told so in the status line. Clear unbinds the
 * picked control's key and gamepad button. Esc backs out one level; typed
 * keys are not typed while Map is armed. Rebinding steals the key from its
 * old target.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define UNIT        36.0    /* points per layout unit */
#define MARGIN      16.0
#define TOP_H        4.0    /* the case above the console strip */
#define MAX_CTL     32      /* room for the core's 22 controls */
#define TITLE_H     18.0
#define ROW_H       26.0
#define EFF_W      120.0
#define POPUP_W    120.0
#define BTN_H       28.0
#define SYS_H       28.0
#define SYS_GAP     10.0
#define MAX_PAD_ROWS 4
#define SLOW_TICKS   8      /* the 30 Hz tick's 250 ms part */
#define SYS_COUNT    3
#define DISH_RADIUS  1.55   /* layout units; the core's D-pad disc */
#define DRAG_OFF     1.6    /* a drag this many radii out lets go */
#define SHOW_KEYS_KEY  "controllers_show_bindings"

/* the palette (the colours live here; the core has only geometry) */
#define COL_CASE_TOP    0x232326
#define COL_CASE_BOT    0x131315
#define COL_CASE_HI     0x3C3C40
#define COL_STRIP       0x0B0B0C
#define COL_STRIP_EDGE  0x2A2A2D
#define COL_CAP_TOP     0x3A3A3E
#define COL_CAP_BOT     0x1C1C1F
#define COL_CAP_EDGE    0x050506
#define COL_CAP_SHADOW  0x070708
#define COL_RECESS      0x1B1B1D
#define COL_CAPTION     0x9A9AA0
#define COL_PAD_TOP     0x303033
#define COL_PAD_BOT     0x171719
#define COL_PAD_HI      0x505055
#define COL_PAD_EDGE    0x0A0A0B
#define COL_SEGA        0x8E8E94
#define COL_DISH        0x0C0C0D
#define COL_DISH_RING   0x2B2B2E
#define COL_ARM_TOP     0x2A2A2D
#define COL_ARM_BOT     0x151517
#define COL_ARM_EDGE    0x050505
#define COL_ARM_HI      0x4A4A4F
#define COL_ARM_GLYPH   0x777780
#define COL_BTN_TOP     0x2E2E32
#define COL_BTN_BOT     0x151517
#define COL_BTN_INK     0xE6E6E6
#define COL_WORD        0xF4F4F4
#define COL_CAP_INK     0xEDEDED
#define COL_WHITE       0xFFFFFF

static NSString *nsstr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) smssession_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgba(uint32_t value, CGFloat alpha)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:alpha];
}

static NSColor *rgb(uint32_t value)
{
    return rgba(value, 1.0);
}

/* from toward to by frac (0..1), per channel */
static uint32_t mixRgb(uint32_t fromRgb, uint32_t toRgb, double frac)
{
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double one = (double)((fromRgb >> shift) & 0xff);
        const double two = (double)((toRgb >> shift) & 0xff);
        out |= (uint32_t)(one + (two - one) * frac) << shift;
    }
    return out;
}

/* path filled with a vertical gradient from y=top (colour from) to y=bottom */
static void fillGradient(NSBezierPath *path, CGFloat top, CGFloat bottom, uint32_t fromRgb, uint32_t toRgb)
{
    NSGradient *grad = [[NSGradient alloc] initWithStartingColor:rgb(fromRgb) endingColor:rgb(toRgb)];
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    [grad drawFromPoint:NSMakePoint(0, top) toPoint:NSMakePoint(0, bottom)
                options:NSGradientDrawsBeforeStartingLocation | NSGradientDrawsAfterEndingLocation];
    [NSGraphicsContext restoreGraphicsState];
}

/* Lines of text centred in area, the font shrunk (to 6 pt) until they fit. */
static void drawFitted(NSString *text, NSRect area, CGFloat size, BOOL bold, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat count = (CGFloat)[lines count];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineH = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        CGFloat widest = 0;
        lineH = 0;
        for (NSString *line in lines) {
            const NSSize extent = [line sizeWithAttributes:attrs];
            if (extent.width > widest) widest = extent.width;
            if (extent.height > lineH) lineH = extent.height;
        }
        if ((widest <= area.size.width && lineH * count <= area.size.height + 2) || size <= 6) break;
        size -= 0.5;
    }
    CGFloat ypos = area.origin.y + floor((area.size.height - lineH * count) / 2);
    for (NSString *line in lines) {
        [line drawInRect:NSMakeRect(area.origin.x, ypos, area.size.width, lineH) withAttributes:attrs];
        ypos += lineH;
    }
}

/* A key name as it fits a control: arrows as glyphs, anything longer than
 * five characters ellipsized, "—" for none. */
static NSString *shortLegend(NSString *name)
{
    if ([name length] == 0) return @"—";
    if ([name isEqualToString:@"Up"]) return @"↑";
    if ([name isEqualToString:@"Down"]) return @"↓";
    if ([name isEqualToString:@"Left"]) return @"←";
    if ([name isEqualToString:@"Right"]) return @"→";
    if ([name length] > 5) return [[name substringToIndex:4] stringByAppendingString:@"…"];
    return name;
}

/* ---- views --------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface SMSFlippedView : NSView
@end

@implementation SMSFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class SMSControllersWindow;
@interface SMSControllersPanel : NSPanel
@property (nonatomic, weak) SMSControllersWindow *owner;
@end

@interface SMSControllersWindow () <NSWindowDelegate>
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (NSString *)tipForTarget:(int)target;
- (void)panelKeyDown:(NSEvent *)evt;
- (void)panelKeyUp:(NSEvent *)evt;
- (void)panelFlagsChanged:(NSEvent *)evt;
@end

/* The console: the black case, its strip with the wordmark and buttons, and
 * the two control pads. Everything on it is drawn here and hit-tested here
 * (by the core). */
@interface SMSConsoleView : SMSFlippedView
@property (nonatomic) smssession *session;
@property (nonatomic, weak) SMSControllersWindow *owner;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key where the glyph is */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configureForConsole:(int)console;
/* Reads what the console sees held; redraws when anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation SMSConsoleView {
    int _console;
    const sms_control *_ctl;
    int _count;
    float _layW, _layH;
    NSSize _size;
    BOOL _lit[MAX_CTL];
    BOOL _dishLit[SMS_PLAYERS];
    unsigned _held[SMS_PLAYERS];
    int _hover;                  /* the control under the pointer, or -1 */

    /* the mouse */
    int _downIdx;                /* the control pressed, or -1 */
    int _downPort;               /* the pad whose pointer mask is set, or -1 */
    int _downTarget;             /* the console button held, or -1 */
    unsigned _downMask;
    BOOL _dragging;              /* an arm or the disc: the mask follows the pointer */
    BOOL _fromArm, _moved;
    NSPoint _downPoint;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _downPort = -1;
    _downTarget = -1;
    _mapTarget = -1;
    _hover = -1;
    return self;
}

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
/* a Control-click is a press, not a context menu */
- (NSMenu *)menuForEvent:(NSEvent *)evt { (void)evt; return nil; }

- (NSSize)contentSize { return _size; }

- (void)updateTrackingAreas
{
    for (NSTrackingArea *area in [self trackingAreas]) [self removeTrackingArea:area];
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:[self bounds]
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self userInfo:nil]];
    [super updateTrackingAreas];
}

- (void)configureForConsole:(int)console
{
    _console = console;
    _ctl = smssession_controls_layout(console, &_count, &_layW, &_layH);
    if (!_ctl) _count = 0;
    if (_count > MAX_CTL) _count = MAX_CTL;
    _size = NSMakeSize(ceil(_layW * UNIT) + 2 * MARGIN, TOP_H + ceil(_layH * UNIT) + MARGIN);
    memset(_lit, 0, sizeof _lit);
    _hover = -1;

    [self removeAllToolTips];
    for (int idx = 0; idx < _count; idx++) {
        if (_ctl[idx].target < 0) continue;
        if (_ctl[idx].kind != SMS_CTL_SWITCH && _ctl[idx].kind != SMS_CTL_DIRECTION &&
            _ctl[idx].kind != SMS_CTL_BUTTON)
            continue;
        [self addToolTipRect:[self rectOf:&_ctl[idx]] owner:self userData:(void *)(intptr_t)idx];
    }
    [self setNeedsDisplay:YES];
}

- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)point
          userData:(void *)data
{
    (void)view; (void)tag; (void)point;
    const intptr_t idx = (intptr_t)data;
    if (idx < 0 || idx >= _count) return nil;
    return [self.owner tipForTarget:_ctl[idx].target];
}

/* ---- geometry ------------------------------------------------------------------ */

- (NSRect)rectOf:(const sms_control *)ctl
{
    return NSMakeRect(MARGIN + ctl->left * UNIT, TOP_H + ctl->top * UNIT, ctl->width * UNIT, ctl->height * UNIT);
}

/* The D-pad disc of a pad: its centre, in view points. */
- (BOOL)dishOfPort:(int)port centre:(NSPoint *)centre
{
    for (int idx = 0; idx < _count; idx++) {
        const sms_control *ctl = &_ctl[idx];
        if (ctl->kind != SMS_CTL_DPAD || ctl->port != port) continue;
        const NSRect box = [self rectOf:ctl];
        *centre = NSMakePoint(NSMidX(box), NSMidY(box));
        return YES;
    }
    return NO;
}

/* ---- the held state --------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    for (int port = 0; port < SMS_PLAYERS; port++)
        _held[port] = self.mapping ? 0 : smssession_buttons_held(_session, port);

    unsigned dirMask = 0;
    dirMask |= (1u << SMS_ACT_UP) | (1u << SMS_ACT_DOWN) | (1u << SMS_ACT_LEFT) | (1u << SMS_ACT_RIGHT);
    for (int port = 0; port < SMS_PLAYERS; port++) {
        const BOOL dish = (_held[port] & dirMask) != 0;
        if (dish != _dishLit[port]) {
            _dishLit[port] = dish;
            dirty = YES;
        }
    }

    for (int idx = 0; idx < _count; idx++) {
        const sms_control *ctl = &_ctl[idx];
        BOOL held = NO;
        switch (ctl->kind) {
        case SMS_CTL_SWITCH:
            held = !self.mapping && smssession_switch_held(_session, ctl->target - SMS_TARGET_SWITCH(0)) != 0;
            break;
        case SMS_CTL_DIRECTION:
        case SMS_CTL_BUTTON:
            held = ctl->port >= 0 && ctl->port < SMS_PLAYERS && (_held[ctl->port] & ctl->act_mask) != 0;
            break;
        default:
            break;
        }
        if (held != _lit[idx]) {
            _lit[idx] = held;
            dirty = YES;
        }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------------- */

- (NSString *)legendFor:(const sms_control *)ctl
{
    if (!(self.mapping || self.showKeys) || ctl->target < 0) return nil;
    return shortLegend([self.owner legendForTarget:ctl->target]);
}

- (BOOL)selected:(const sms_control *)ctl
{
    return self.mapping && ctl->target >= 0 && ctl->target == self.mapTarget;
}

/* Map picked something else: shade this control back to 40 % by laying the
 * surface colour over it. */
- (BOOL)dimmed:(const sms_control *)ctl
{
    return self.mapping && self.mapTarget >= 0 && ctl->target != self.mapTarget;
}

- (void)shade:(NSBezierPath *)path over:(uint32_t)surface
{
    [rgba(surface, 0.6) setFill];
    [path fill];
}

/* the colours a control is painted in: lit, hovered, or plain */
- (uint32_t)lighten:(uint32_t)value index:(int)idx lit:(BOOL)lit
{
    if (lit || idx != _hover || self.mapping) return value;
    return mixRgb(value, COL_WHITE, 0.12);
}

/* The strip along the top of the console: the SEGA wordmark, with the model
 * in the accent colour, and the accent stripe under it. */
- (void)drawStrip:(const sms_control *)ctl
{
    NSColor *accent = rgb(SMSSESSION_ACCENT_RGB);
    const NSRect strip = [self rectOf:ctl];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:strip xRadius:6 yRadius:6];
    [rgb(COL_STRIP) setFill];
    [path fill];
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    [accent setFill];
    NSRectFill(NSMakeRect(strip.origin.x, NSMaxY(strip) - 3, strip.size.width, 3));
    [NSGraphicsContext restoreGraphicsState];
    [rgb(COL_STRIP_EDGE) setStroke];
    path.lineWidth = 1;
    [path stroke];

    /* the wordmark has the room left of the first console button */
    CGFloat room = strip.size.width;
    for (int idx = 0; idx < _count; idx++)
        if (_ctl[idx].kind == SMS_CTL_SWITCH) room = MIN(room, [self rectOf:&_ctl[idx]].origin.x - strip.origin.x);
    room -= 32;

    NSString *legend = nsstr(ctl->legend);
    NSString *word = @"SEGA";
    NSString *model = [legend hasPrefix:@"SEGA "] ? [legend substringFromIndex:5] : legend;
    for (CGFloat size = 20; size >= 10; size -= 1) {
        NSFont *font = [NSFont boldSystemFontOfSize:size];
        NSMutableAttributedString *mark = [[NSMutableAttributedString alloc]
            initWithString:word
                attributes:@{ NSFontAttributeName: font,
                              NSForegroundColorAttributeName: rgb(COL_WORD),
                              NSKernAttributeName: @(3.0) }];
        [mark appendAttributedString:[[NSAttributedString alloc]
            initWithString:[@" " stringByAppendingString:model]
                attributes:@{ NSFontAttributeName: font,
                              NSForegroundColorAttributeName: accent,
                              NSKernAttributeName: @(1.0) }]];
        const NSSize extent = [mark size];
        if (extent.width <= room || size <= 10) {
            [mark drawAtPoint:NSMakePoint(strip.origin.x + 16,
                                          strip.origin.y + floor((strip.size.height - 3 - extent.height) / 2))];
            break;
        }
    }
}

/* PAUSE, RESET or RAPID: a rectangular cap on a shadow; pressed it sinks and
 * takes the accent. In Map the bound key sits under the name. */
- (void)drawSwitch:(const sms_control *)ctl index:(int)idx lit:(BOOL)lit
{
    NSColor *accent = rgb(SMSSESSION_ACCENT_RGB);
    const NSRect cap = [self rectOf:ctl];
    NSRect face = cap;
    if (lit) face.origin.y += 2;
    else {
        [rgb(COL_CAP_SHADOW) setFill];
        [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(cap.origin.x, cap.origin.y + 3, cap.size.width, cap.size.height)
                                         xRadius:3 yRadius:3] fill];
    }
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(face, 0.5, 0.5) xRadius:3 yRadius:3];
    if (lit) {
        [accent setFill];
        [path fill];
    } else {
        fillGradient(path, face.origin.y, NSMaxY(face), [self lighten:COL_CAP_TOP index:idx lit:lit],
                     [self lighten:COL_CAP_BOT index:idx lit:lit]);
    }
    [(lit ? rgb(COL_WHITE) : rgb(COL_CAP_EDGE)) setStroke];
    path.lineWidth = lit ? 2 : 1;
    [path stroke];
    if ([self dimmed:ctl]) [self shade:path over:COL_STRIP];

    NSColor *ink = lit ? rgb(COL_WHITE) : rgb(COL_CAP_INK);
    NSString *key = [self legendFor:ctl];
    const NSRect inner = NSInsetRect(face, 3, 2);
    if (key.length)
        drawFitted([NSString stringWithFormat:@"%@\n%@", nsstr(ctl->label), key], inner, 9, YES, ink);
    else
        drawFitted(nsstr(ctl->label), inner, 10, YES, ink);
}

/* A numbered button: a rounded square on a shadow. */
- (void)drawButton:(const sms_control *)ctl index:(int)idx lit:(BOOL)lit
{
    NSColor *accent = rgb(SMSSESSION_ACCENT_RGB);
    const NSRect body = [self rectOf:ctl];
    NSRect face = body;
    const CGFloat radius = 0.28 * UNIT;
    if (lit) face.origin.y += 2;
    else {
        [rgb(COL_CAP_SHADOW) setFill];
        [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(body.origin.x, body.origin.y + 3, body.size.width, body.size.height)
                                         xRadius:radius yRadius:radius] fill];
    }
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(face, 0.5, 0.5)
                                                         xRadius:radius yRadius:radius];
    if (lit) {
        [accent setFill];
        [path fill];
    } else {
        fillGradient(path, face.origin.y, NSMaxY(face), [self lighten:COL_BTN_TOP index:idx lit:lit],
                     [self lighten:COL_BTN_BOT index:idx lit:lit]);
    }
    [(lit ? rgb(COL_WHITE) : rgb(COL_ARM_EDGE)) setStroke];
    path.lineWidth = lit ? 2 : 1;
    [path stroke];
    if ([self dimmed:ctl]) [self shade:path over:COL_PAD_BOT];

    NSString *key = [self legendFor:ctl];
    NSColor *ink = lit ? rgb(COL_WHITE) : rgb(COL_BTN_INK);
    if (key.length)
        drawFitted(key, NSInsetRect(face, 3, 3), 12, YES, ink);
    else
        drawFitted(nsstr(ctl->label), NSInsetRect(face, 3, 3), 15, YES, ink);
}

/* The D-pad's dish and the cross in it; the arms are painted on top by
 * drawArm. */
- (void)drawDish:(const sms_control *)ctl
{
    NSColor *accent = rgb(SMSSESSION_ACCENT_RGB);
    const NSRect box = [self rectOf:ctl];
    const int port = ctl->port >= 0 && ctl->port < SMS_PLAYERS ? ctl->port : 0;
    const BOOL lit = _dishLit[port];
    NSBezierPath *dish = [NSBezierPath bezierPathWithOvalInRect:box];
    [rgb(COL_DISH) setFill];
    [dish fill];
    [(lit ? accent : rgb(COL_DISH_RING)) setStroke];
    dish.lineWidth = lit ? 2.5 : 1.5;
    [dish stroke];

    /* the cross: three cells each way, arms one cell wide */
    const CGFloat ctrX = NSMidX(box), ctrY = NSMidY(box);
    const CGFloat half = UNIT / 2;
    NSBezierPath *cross = [NSBezierPath bezierPath];
    [cross moveToPoint:NSMakePoint(ctrX - half, ctrY - 3 * half)];
    [cross lineToPoint:NSMakePoint(ctrX + half, ctrY - 3 * half)];
    [cross lineToPoint:NSMakePoint(ctrX + half, ctrY - half)];
    [cross lineToPoint:NSMakePoint(ctrX + 3 * half, ctrY - half)];
    [cross lineToPoint:NSMakePoint(ctrX + 3 * half, ctrY + half)];
    [cross lineToPoint:NSMakePoint(ctrX + half, ctrY + half)];
    [cross lineToPoint:NSMakePoint(ctrX + half, ctrY + 3 * half)];
    [cross lineToPoint:NSMakePoint(ctrX - half, ctrY + 3 * half)];
    [cross lineToPoint:NSMakePoint(ctrX - half, ctrY + half)];
    [cross lineToPoint:NSMakePoint(ctrX - 3 * half, ctrY + half)];
    [cross lineToPoint:NSMakePoint(ctrX - 3 * half, ctrY - half)];
    [cross lineToPoint:NSMakePoint(ctrX - half, ctrY - half)];
    [cross closePath];
    cross.lineJoinStyle = NSLineJoinStyleRound;
    fillGradient(cross, ctrY - 3 * half, ctrY + 3 * half, COL_ARM_TOP, COL_ARM_BOT);
    [rgb(COL_ARM_EDGE) setStroke];
    cross.lineWidth = 2;
    [cross stroke];

    /* the top edge of the three arms that have one */
    [rgb(COL_ARM_HI) setStroke];
    NSBezierPath *edge = [NSBezierPath bezierPath];
    edge.lineWidth = 1;
    [edge moveToPoint:NSMakePoint(ctrX - half + 2, ctrY - 3 * half + 1.5)];
    [edge lineToPoint:NSMakePoint(ctrX + half - 2, ctrY - 3 * half + 1.5)];
    [edge moveToPoint:NSMakePoint(ctrX - 3 * half + 2, ctrY - half + 1.5)];
    [edge lineToPoint:NSMakePoint(ctrX - half - 2, ctrY - half + 1.5)];
    [edge moveToPoint:NSMakePoint(ctrX + half + 2, ctrY - half + 1.5)];
    [edge lineToPoint:NSMakePoint(ctrX + 3 * half - 2, ctrY - half + 1.5)];
    [edge stroke];

    /* the dimple in the middle, with the thumb-stick screw hole */
    NSBezierPath *dimple = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(ctrX - 0.30 * UNIT, ctrY - 0.30 * UNIT, 0.60 * UNIT, 0.60 * UNIT)];
    [rgb(COL_DISH) setFill];
    [dimple fill];
    NSBezierPath *hole = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(ctrX - 0.12 * UNIT, ctrY - 0.12 * UNIT, 0.24 * UNIT, 0.24 * UNIT)];
    [rgb(COL_ARM_EDGE) setFill];
    [hole fill];
}

/* One cross arm: lit in the accent when its direction is held; the glyph (a
 * triangle pointing outward) or, with bindings shown, its key. */
- (void)drawArm:(const sms_control *)ctl index:(int)idx lit:(BOOL)lit
{
    NSColor *accent = rgb(SMSSESSION_ACCENT_RGB);
    const NSRect arm = [self rectOf:ctl];
    NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSInsetRect(arm, 1, 1)];
    if (lit) {
        [accent setFill];
        [path fill];
        [rgb(COL_WHITE) setStroke];
        path.lineWidth = 2;
        [path stroke];
    } else if (idx == _hover && !self.mapping) {
        [rgba(COL_WHITE, 0.12) setFill];
        [path fill];
    }
    if ([self dimmed:ctl]) [self shade:path over:COL_DISH];

    NSString *key = [self legendFor:ctl];
    if (key.length) {
        drawFitted(key, NSInsetRect(arm, 2, 2), 12, YES, lit ? rgb(COL_WHITE) : rgb(COL_CAP_INK));
        return;
    }
    /* the triangle points the way the arm does (y grows downward here) */
    double dirX = 0, dirY = 0;
    if (ctl->act_mask & (1u << SMS_ACT_UP)) dirY = -1;
    else if (ctl->act_mask & (1u << SMS_ACT_DOWN)) dirY = 1;
    else if (ctl->act_mask & (1u << SMS_ACT_LEFT)) dirX = -1;
    else dirX = 1;
    const CGFloat span = 0.30 * UNIT;
    const NSPoint mid = NSMakePoint(NSMidX(arm), NSMidY(arm));
    NSBezierPath *tri = [NSBezierPath bezierPath];
    [tri moveToPoint:NSMakePoint(mid.x + dirX * span * 0.55, mid.y + dirY * span * 0.55)];
    [tri lineToPoint:NSMakePoint(mid.x - dirX * span * 0.45 - dirY * span * 0.6,
                                 mid.y - dirY * span * 0.45 + dirX * span * 0.6)];
    [tri lineToPoint:NSMakePoint(mid.x - dirX * span * 0.45 + dirY * span * 0.6,
                                 mid.y - dirY * span * 0.45 - dirX * span * 0.6)];
    [tri closePath];
    [(lit ? rgb(SMSSESSION_ACCENT_DARK_RGB) : rgb(COL_ARM_GLYPH)) setFill];
    [tri fill];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect bounds = [self bounds];
    NSColor *accent = rgb(SMSSESSION_ACCENT_RGB);

    /* the case: a dark vertical gradient with a highlight along its top */
    NSGradient *grad = [[NSGradient alloc] initWithStartingColor:rgb(COL_CASE_TOP) endingColor:rgb(COL_CASE_BOT)];
    [grad drawFromPoint:NSMakePoint(0, 0) toPoint:NSMakePoint(0, bounds.size.height)
                options:NSGradientDrawsBeforeStartingLocation | NSGradientDrawsAfterEndingLocation];
    [rgb(COL_CASE_HI) setFill];
    NSRectFill(NSMakeRect(0, 0, bounds.size.width, 1.5));

    for (int idx = 0; idx < _count; idx++) {
        const sms_control *ctl = &_ctl[idx];
        switch (ctl->kind) {
        case SMS_CTL_CONSOLE:
            [self drawStrip:ctl];
            break;
        case SMS_CTL_SWITCH:
            [self drawSwitch:ctl index:idx lit:_lit[idx]];
            break;
        case SMS_CTL_CAPTION: {
            /* the recess behind a pad, from its caption down to its foot */
            const NSRect cap = [self rectOf:ctl];
            const CGFloat foot = TOP_H + _layH * UNIT;
            NSBezierPath *recess = [NSBezierPath bezierPathWithRoundedRect:
                NSMakeRect(cap.origin.x - 6, cap.origin.y - 3, cap.size.width + 12, foot - cap.origin.y + 6)
                                                                   xRadius:10 yRadius:10];
            [rgb(COL_RECESS) setFill];
            [recess fill];
            [nsstr(ctl->label) drawAtPoint:NSMakePoint(cap.origin.x + 8, cap.origin.y + 4)
                            withAttributes:@{ NSFontAttributeName: [NSFont boldSystemFontOfSize:11],
                                              NSForegroundColorAttributeName: rgb(COL_CAPTION) }];
            break;
        }
        case SMS_CTL_PAD: {
            const NSRect body = [self rectOf:ctl];
            NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:body xRadius:0.7 * UNIT yRadius:0.7 * UNIT];
            fillGradient(path, body.origin.y, NSMaxY(body), COL_PAD_TOP, COL_PAD_BOT);
            [NSGraphicsContext saveGraphicsState];
            [path addClip];
            [rgb(COL_PAD_HI) setFill];
            NSRectFill(NSMakeRect(body.origin.x, body.origin.y, body.size.width, 1.5));
            [NSGraphicsContext restoreGraphicsState];
            [rgb(COL_PAD_EDGE) setStroke];
            path.lineWidth = 1;
            [path stroke];
            NSDictionary *mark = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:SMS_PAD_MARK_PX],
                                    NSForegroundColorAttributeName: rgb(COL_SEGA),
                                    NSKernAttributeName: @(SMS_PAD_MARK_SPACING) };
            const NSRect box = NSMakeRect(body.origin.x + SMS_PAD_MARK_LEFT * UNIT,
                                          body.origin.y + SMS_PAD_MARK_TOP * UNIT,
                                          SMS_PAD_MARK_WIDTH * UNIT, SMS_PAD_MARK_HEIGHT * UNIT);
            const NSSize extent = [@"SEGA" sizeWithAttributes:mark];
            [@"SEGA" drawAtPoint:NSMakePoint(NSMidX(box) - extent.width / 2, NSMidY(box) - extent.height / 2)
                  withAttributes:mark];
            break;
        }
        case SMS_CTL_DPAD:
            [self drawDish:ctl];
            break;
        case SMS_CTL_DIRECTION:
            [self drawArm:ctl index:idx lit:_lit[idx]];
            break;
        case SMS_CTL_BUTTON:
            [self drawButton:ctl index:idx lit:_lit[idx]];
            break;
        default:
            break;
        }
    }

    /* Map mode: the picked control is called out */
    if (self.mapping && self.mapTarget >= 0) {
        for (int idx = 0; idx < _count; idx++) {
            const sms_control *ctl = &_ctl[idx];
            if (ctl->target != self.mapTarget) continue;
            NSBezierPath *ring = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect([self rectOf:ctl], -2, -2)
                                                                 xRadius:4 yRadius:4];
            ring.lineWidth = 3;
            [accent setStroke];
            [ring stroke];
        }
    }
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)setPointerMask:(unsigned)mask
{
    if (_downPort < 0) return;
    _downMask = mask;
    smssession_pointer_set(_session, _downPort, mask);
}

- (void)letGo
{
    for (int port = 0; port < SMS_PLAYERS; port++) smssession_pointer_set(_session, port, 0);
    if (_downTarget >= 0) smssession_press(_session, _downTarget, 0);
    _downIdx = -1;
    _downPort = -1;
    _downTarget = -1;
    _downMask = 0;
    _dragging = NO;
    _fromArm = NO;
    _moved = NO;
}

- (int)hitAt:(NSPoint)point
{
    const int idx = smssession_controls_hit(_console, (float)((point.x - MARGIN) / UNIT),
                                            (float)((point.y - TOP_H) / UNIT));
    return idx >= 0 && idx < _count ? idx : -1;
}

/* The directions a drag from the disc's centre to point holds. Far outside
 * the disc nothing is held (dragging off lets go). */
- (unsigned)maskForPort:(int)port atPoint:(NSPoint)point
{
    NSPoint ctr;
    if (![self dishOfPort:port centre:&ctr]) return 0;
    const CGFloat radius = DISH_RADIUS * UNIT;
    const CGFloat delX = point.x - ctr.x, delY = point.y - ctr.y;
    if (hypot(delX, delY) > radius * DRAG_OFF) return 0;
    return smssession_dpad_from_vector((float)delX, (float)delY, (float)radius);
}

- (void)mouseDown:(NSEvent *)evt
{
    const NSPoint point = [self convertPoint:[evt locationInWindow] fromView:nil];
    if (_downIdx >= 0) return;
    const int idx = [self hitAt:point];
    if (idx < 0) return;
    const sms_control *ctl = &_ctl[idx];

    if (self.mapping) {
        /* Map: a part with a target of its own is picked; the disc has none */
        if (ctl->target >= 0 && ctl->kind != SMS_CTL_DPAD) [self.owner mapPick:ctl->target];
        return;
    }

    switch (ctl->kind) {
    case SMS_CTL_SWITCH:
        _downIdx = idx;
        _downTarget = ctl->target;
        smssession_press(_session, ctl->target, 1);
        break;
    case SMS_CTL_BUTTON:
        _downIdx = idx;
        _downPort = ctl->port;
        [self setPointerMask:ctl->act_mask];
        break;
    case SMS_CTL_DIRECTION:
        /* exactly that arm until the pointer moves; then it follows the drag */
        _downIdx = idx;
        _downPort = ctl->port;
        _dragging = YES;
        _fromArm = YES;
        _moved = NO;
        _downPoint = point;
        [self setPointerMask:ctl->act_mask];
        break;
    case SMS_CTL_DPAD:
        _downIdx = idx;
        _downPort = ctl->port;
        _dragging = YES;
        _fromArm = NO;
        [self setPointerMask:[self maskForPort:ctl->port atPoint:point]];
        break;
    default:
        break;
    }
    [self poll];
}

- (void)mouseDragged:(NSEvent *)evt
{
    if (_downIdx < 0) return;
    const NSPoint point = [self convertPoint:[evt locationInWindow] fromView:nil];
    if (_dragging) {
        unsigned mask;
        if (_fromArm && !_moved && hypot(point.x - _downPoint.x, point.y - _downPoint.y) < 4.0) {
            mask = _downMask;
        } else {
            _moved = YES;
            mask = [self maskForPort:_downPort atPoint:point];
        }
        if (mask != _downMask) {
            [self setPointerMask:mask];
            [self poll];
        }
        return;
    }
    /* a button or a console button: dragging off lets go */
    if (!NSPointInRect(point, [self rectOf:&_ctl[_downIdx]])) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseUp:(NSEvent *)evt
{
    (void)evt;
    if (_downIdx < 0) return;
    [self letGo];
    [self poll];
}

- (void)mouseMoved:(NSEvent *)evt
{
    const int idx = [self hitAt:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (idx == _hover) return;
    _hover = idx;
    [self setNeedsDisplay:YES];
}

- (void)mouseExited:(NSEvent *)evt
{
    (void)evt;
    if (_hover < 0) return;
    _hover = -1;
    [self setNeedsDisplay:YES];
}
@end

@implementation SMSControllersPanel
- (void)keyDown:(NSEvent *)evt { [self.owner panelKeyDown:evt]; }
- (void)keyUp:(NSEvent *)evt { [self.owner panelKeyUp:evt]; }
- (void)flagsChanged:(NSEvent *)evt { [self.owner panelFlagsChanged:evt]; }
/* Escape is the console's Reset to CONFIG (or backs out of Map), not "close
 * the panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

static SMSControllersWindow *g_singleton;
static smssession *g_session;
static SMSAppKeyHandler g_appKeys;

@implementation SMSControllersWindow {
    SMSControllersPanel *_panel;
    SMSFlippedView *_content;
    SMSConsoleView *_console;
    NSButton *_sysButton[SYS_COUNT];
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "drives Player N" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSTextField *_pickLabel;
    NSPopUpButton *_pickPopup;              /* every target, for Map */
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    int _builtConsole;
    uint32_t _swallowKey;                   /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

static const struct { NSString *__unsafe_unretained title; int action; } kSysButtons[SYS_COUNT] = {
    { @"Reset to CONFIG",  SMS_SYSACT_RESET_CONFIG },
    { @"Soft Reset",       SMS_SYSACT_SOFT_RESET },
    { @"Stop in Debugger", SMS_SYSACT_DEBUG_STOP },
};

- (instancetype)init
{
    self = [super init];
    if (!self) return nil;
    _mapState = -2;
    _builtConsole = -1;
    _padGeneration = (unsigned)-1;

    _panel = [[SMSControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 600, 560)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    [_panel setAcceptsMouseMovedEvents:YES];   /* the hover light */
    _panel.owner = self;
    [_panel setDelegate:self];
    SMSSetHelpContext(_panel, @"controllers");

    _content = [[SMSFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 600, 560)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _console = [[SMSConsoleView alloc] initWithFrame:NSZeroRect];
    _console.owner = self;
    _console.session = g_session;
    _console.mapTarget = -1;
    _console.showKeys = smssession_get_int(g_session, SHOW_KEYS_KEY, 0) > 0;
    [_content addSubview:_console];

    /* the session's own actions; each fires on release like a button */
    for (int idx = 0; idx < SYS_COUNT; idx++) {
        _sysButton[idx] = [NSButton buttonWithTitle:kSysButtons[idx].title target:self action:@selector(sysPressed:)];
        _sysButton[idx].tag = idx;
        [_sysButton[idx] setRefusesFirstResponder:YES];
        [_content addSubview:_sysButton[idx]];
    }

    /* Gamepads: one row per pad, its name, the player it drives and the one it
     * is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int idx = 0; idx < MAX_PAD_ROWS; idx++) {
        _padName[idx] = [NSTextField labelWithString:@""];
        [_padName[idx] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[idx]];
        _padEff[idx] = [NSTextField labelWithString:@""];
        [_padEff[idx] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[idx] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[idx]];
        /* -1 = automatic (connection order), 0 and 1 = that player */
        _padAssign[idx] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[idx] addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2"]];
        _padAssign[idx].tag = idx;
        _padAssign[idx].target = self;
        _padAssign[idx].action = @selector(padAssignChanged:);
        [_padAssign[idx] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[idx]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_console.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    _pickLabel = [NSTextField labelWithString:@"Control"];
    [_content addSubview:_pickLabel];
    _pickPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int target = 0; target < SMS_TARGET_COUNT; target++)
        [[_pickPopup menu] addItemWithTitle:nsstr(sms_target_name(target)) action:nil keyEquivalent:@""];
    _pickPopup.target = self;
    _pickPopup.action = @selector(pickChanged:);
    [_pickPopup setRefusesFirstResponder:YES];
    [_content addSubview:_pickPopup];
    _helpButton = SMSHelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];
    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [self setMapState:-2];
    [self rebuild];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- the console ------------------------------------------------------------------- */

/* The running console, and so the layout; Settings can be ahead of it, which
 * the layout must not follow until the machine does. */
- (int)consoleNow
{
    const int console = smssession_console(g_session);
    return console >= 0 && console < SMS_CONSOLE_COUNT ? console : SMS_CONSOLE_SMS1;
}

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    [_panel setContentMinSize:size];
    [_panel setContentMaxSize:size];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const NSSize extent = _console.contentSize;
    const CGFloat width = extent.width;
    const CGFloat inner = width - 2 * MARGIN;
    const CGFloat sysY = extent.height + 6;
    const CGFloat padsY = sysY + SYS_H + 12;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + MAX_PAD_ROWS * ROW_H + 8;
    const CGFloat pickY = mapY + BTN_H + 8;
    const CGFloat hintY = pickY + 25 + 6;
    const CGFloat total = hintY + 34 + MARGIN;

    [self fitPanel:NSMakeSize(width, total)];
    [_console setFrame:NSMakeRect(0, 0, extent.width, extent.height)];
    const CGFloat sysW = (inner - (SYS_COUNT - 1) * SYS_GAP) / SYS_COUNT;
    for (int idx = 0; idx < SYS_COUNT; idx++)
        [_sysButton[idx] setFrame:NSMakeRect(MARGIN + idx * (sysW + SYS_GAP), sysY, sysW, SYS_H)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, 200, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 4, inner, 18)];
    for (int idx = 0; idx < MAX_PAD_ROWS; idx++) {
        const CGFloat ypos = rowsY + idx * ROW_H;
        [_padName[idx] setFrame:NSMakeRect(MARGIN, ypos + 4, inner - POPUP_W - EFF_W - 16, 18)];
        [_padEff[idx] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 8, ypos + 4, EFF_W, 18)];
        [_padAssign[idx] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, ypos, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 64, BTN_H)];
    [_clearButton setFrame:NSMakeRect(MARGIN + 70, mapY, 64, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 140, mapY, 84, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 236, mapY + 3, 130, 22)];
    [_helpButton setFrameOrigin:NSMakePoint(width - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_pickLabel setFrame:NSMakeRect(MARGIN, pickY + 4, 50, 18)];
    [_pickPopup setFrame:NSMakeRect(MARGIN + 54, pickY, 240, 25)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 34)];
    [_console setNeedsDisplay:YES];
}

- (void)rebuild
{
    [self letGo];
    _builtConsole = [self consoleNow];
    [_console configureForConsole:_builtConsole];
    [self relayout];
    [self refreshLabels];
}

/* Lets go of what the mouse holds in this panel. */
- (void)letGo
{
    [_console letGo];
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    return keyName(smssession_binding_get(g_session, target).keysym);
}

/* "Player 1: Left", the key and the gamepad button, for a tooltip. */
- (NSString *)tipForTarget:(int)target
{
    const sms_binding bind = smssession_binding_get(g_session, target);
    NSString *key = keyName(bind.keysym);
    NSString *pad = bind.button != SMS_PAD_BTN_NONE ? nsstr(sms_pad_button_name(bind.button)) : @"";
    return [NSString stringWithFormat:@"%@\nKey: %@\nGamepad: %@", nsstr(sms_target_name(target)),
            key.length ? key : @"none", pad.length ? pad : @"none"];
}

/* What is bound to a target, for the status line. */
- (NSString *)bindingText:(int)target
{
    const sms_binding bind = smssession_binding_get(g_session, target);
    NSString *key = keyName(bind.keysym);
    NSString *text = key.length ? key : @"no key";
    if (bind.button != SMS_PAD_BTN_NONE)
        text = [NSString stringWithFormat:@"%@, pad %@", text, nsstr(sms_pad_button_name(bind.button))];
    return text;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    const BOOL mapping = _mapState != -2;
    _console.mapping = mapping;
    _console.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_console setNeedsDisplay:YES];
    [_console poll];

    const BOOL legends = mapping || _console.showKeys;
    for (int idx = 0; idx < SYS_COUNT; idx++) {
        const int target = SMS_TARGET_SYSACT(kSysButtons[idx].action);
        NSString *key = legends ? [self legendForTarget:target] : @"";
        NSString *title = key.length ? [NSString stringWithFormat:@"%@ (%@)", kSysButtons[idx].title, key]
                                     : kSysButtons[idx].title;
        if (![[_sysButton[idx] title] isEqualToString:title]) [_sysButton[idx] setTitle:title];
        _sysButton[idx].bezelColor = (_mapState == target) ? SMSAccentColor() : nil;
        [_sysButton[idx] setToolTip:[self tipForTarget:target]];
    }
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        [self letGo];
        smssession_gamepad_capture_begin(g_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc cancels.",
                             nsstr(sms_target_name(state)), [self bindingText:state]];
    } else {
        smssession_gamepad_capture_cancel(g_session);
        _hint.stringValue = state == -1
            ? @"Click a control, or choose one from the list, to remap it. Esc leaves Map."
            : @"Press and hold the pad's arrows and buttons and the console's buttons. "
              @"Drag across the D-pad for diagonals.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    _mapButton.bezelColor = state == -2 ? nil : SMSAccentColor();
    _clearButton.enabled = state >= 0;
    _pickPopup.enabled = state != -2;
    if (state >= 0 && state < [_pickPopup numberOfItems]) [_pickPopup selectItemAtIndex:state];
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2) [self setMapState:target];
}

- (void)pickChanged:(NSPopUpButton *)sender
{
    if (_mapState == -2) return;
    const NSInteger sel = sender.indexOfSelectedItem;
    if (sel >= 0 && sel < SMS_TARGET_COUNT) [self setMapState:(int)sel];
}

/* A system action's button: fires on release, or picks the action in Map. */
- (void)sysPressed:(NSButton *)sender
{
    const int idx = (int)sender.tag;
    if (idx < 0 || idx >= SYS_COUNT) return;
    if (_mapState != -2) {
        [self setMapState:SMS_TARGET_SYSACT(kSysButtons[idx].action)];
        return;
    }
    /* posted, so the application runs it exactly as it runs a gamepad's */
    smssession_sysaction_post(g_session, kSysButtons[idx].action);
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = nsstr(sms_target_name(target));
    _hint.stringValue = stolen[0]
        ? [NSString stringWithFormat:@"%@ is now %@ (it was %@)", what, name, nsstr(stolen)]
        : [NSString stringWithFormat:@"%@ is now %@", what, name];
}

- (void)bindKey:(uint32_t)keysym swallow:(BOOL)swallow
{
    char stolen[128];
    stolen[0] = '\0';
    const int target = _mapState;
    smssession_binding_set_key(g_session, target, keysym, stolen, (int)sizeof stolen);
    if (swallow) _swallowKey = keysym;
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int button = SMS_PAD_BTN_NONE;
    char stolen[128];
    if (_mapState < 0 || !smssession_gamepad_capture_poll(g_session, &button)) return;
    stolen[0] = '\0';
    const int target = _mapState;
    smssession_binding_set_button(g_session, target, button, stolen, (int)sizeof stolen);
    [self setMapState:-1];
    [self showBound:nsstr(sms_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

/* Unbinds the picked control's key and gamepad button; it stays picked. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    char stolen[16];
    if (_mapState < 0) return;
    smssession_binding_set_key(g_session, _mapState, 0, stolen, (int)sizeof stolen);
    smssession_binding_set_button(g_session, _mapState, SMS_PAD_BTN_NONE, stolen, (int)sizeof stolen);
    [self refreshLabels];
    _hint.stringValue = @"Cleared. Press a key or a gamepad button to bind it again.";
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    smssession_bindings_reset(g_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL enabled = [_showKeysBox state] == NSControlStateValueOn;
    smssession_set_int(g_session, SHOW_KEYS_KEY, enabled ? 1 : 0);
    _console.showKeys = enabled;
    [self refreshLabels];
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int idx = 0; idx < _padRows; idx++) {
        const int eff = smssession_gamepad_effective_port(g_session, idx);
        NSString *text = eff >= 0 ? [NSString stringWithFormat:@"drives Player %d", eff + 1] : @"drives nothing";
        if (![_padEff[idx].stringValue isEqualToString:text]) _padEff[idx].stringValue = text;
    }
}

- (void)refreshPads
{
    const int count = smssession_gamepad_count(g_session);
    _padGeneration = smssession_gamepad_generation(g_session);
    _padRows = count < MAX_PAD_ROWS ? (count > 0 ? count : 0) : MAX_PAD_ROWS;
    _noPads.hidden = count > 0;
    for (int idx = 0; idx < MAX_PAD_ROWS; idx++) {
        const BOOL shown = idx < _padRows;
        _padName[idx].hidden = !shown;
        _padEff[idx].hidden = !shown;
        _padAssign[idx].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        smssession_gamepad_name(g_session, idx, name, (int)sizeof name);
        _padName[idx].stringValue = nsstr(name);
        _padName[idx].toolTip = nsstr(name);
        const int assign = smssession_gamepad_assignment(g_session, idx);
        [_padAssign[idx] selectItemAtIndex:(assign < 0 || assign >= SMS_PLAYERS ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    smssession_gamepad_assign(g_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads, auto-assignment, a console change and
 * the legends (the bindings may have been reset in Settings). */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (smssession_gamepad_generation(g_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
        if ([self consoleNow] != _builtConsole) [self rebuild];
        else if (_mapState != -2 || _console.showKeys) [self refreshLabels];
    }
    [_console poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak SMSControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *timer) {
            (void)timer;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)evt
{
    if ([evt modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t keysym = SMSKeysymFromEvent(evt);
    if (!keysym) return;
    if (_swallowKey && keysym == _swallowKey) return;
    if ([evt isARepeat]) return;
    if (keysym == SMS_KEYSYM_F9) {
        [self hide];
        return;
    }
    if (keysym == SMS_KEYSYM_F11 || keysym == SMS_KEYSYM_F12) {
        if (g_appKeys) g_appKeys(keysym);
        return;
    }
    if (_mapState >= 0) {
        /* the next key is the binding; Esc backs out of the pick */
        if (keysym == SMS_KEYSYM_ESCAPE) [self setMapState:-1];
        else [self bindKey:keysym swallow:YES];
        return;
    }
    if (_mapState == -1) {
        /* armed: typing does nothing, Esc leaves Map */
        if (keysym == SMS_KEYSYM_ESCAPE) [self setMapState:-2];
        return;
    }
    /* a key bound to a system action goes to the app's queue, as a gamepad's does */
    const int sysact = smssession_key_sysaction(g_session, keysym);
    if (sysact >= 0) {
        smssession_sysaction_post(g_session, sysact);
        return;
    }
    smssession_key(g_session, keysym, 1);
}

- (void)panelKeyUp:(NSEvent *)evt
{
    const uint32_t keysym = SMSKeysymFromEvent(evt);
    if (!keysym) return;
    if (_swallowKey && keysym == _swallowKey) {
        _swallowKey = 0;
        return;
    }
    if (keysym == SMS_KEYSYM_F9 || keysym == SMS_KEYSYM_F11 || keysym == SMS_KEYSYM_F12) return;
    smssession_key(g_session, keysym, 0);
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise
 * they are the console's. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)evt
{
    int down = 0;
    const uint32_t keysym = SMSKeysymFromFlagsChange(evt, &down);
    if (!keysym) return;
    if (_mapState >= 0 && down) {
        [self bindKey:keysym swallow:NO];
        return;
    }
    if (_mapState != -2 && down) return;
    smssession_key(g_session, keysym, down);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    smssession_release_all(g_session);
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    if ([self consoleNow] != _builtConsole) [self rebuild];
    [self refreshPads];
    [self setMapState:-2];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

/* The panel's mouse holds and the keys typed in it: the main window releases
 * its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    smssession_release_all(g_session);
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}

+ (SMSControllersWindow *)instanceForSession:(smssession *)session
{
    g_session = session;
    if (!g_singleton) g_singleton = [[SMSControllersWindow alloc] init];
    return g_singleton;
}

+ (void)setAppKeyHandler:(SMSAppKeyHandler)handler
{
    g_appKeys = [handler copy];
}

+ (void)toggleWithSession:(smssession *)session
{
    SMSControllersWindow *inst = [self instanceForSession:session];
    if ([inst isVisible]) [inst hide];
    else [inst show];
}

+ (void)showWithSession:(smssession *)session
{
    SMSControllersWindow *inst = [self instanceForSession:session];
    if ([inst isVisible]) [inst->_panel makeKeyAndOrderFront:nil];
    else [inst show];
}

+ (BOOL)isVisible
{
    return g_singleton && [g_singleton isVisible];
}

+ (void)bindingsChanged
{
    if (g_singleton) [g_singleton refreshLabels];
}
@end
