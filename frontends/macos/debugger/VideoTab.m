/*
 * The debugger's Video tab (AppKit).
 *
 * Everything that decides what to show lives in the core (smsvidtab.h): this file is views.
 * The picture comes from the controller's cached 0xFFRRGGBB buffer and is drawn as a CGImage
 * (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, no interpolation) over a copy of
 * that buffer; the image is rebuilt only when the controller re-rendered. A mouse move redraws
 * only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import "VideoTab.h"

#import "../KeyForward.h"

#include <stdio.h>
#include <string.h>

#include "smsvidtab.h"

#define MAX_OVERLAYS 8
#define SWATCH_ROWS 32

/* A C string as an NSString, never nil (invalid UTF-8 reads as empty). */
static NSString *str(const char *text)
{
    NSString *s = text ? [NSString stringWithUTF8String:text] : nil;
    return s ? s : @"";
}

/* A 0xFFRRGGBB word as a colour. */
static NSColor *colorFromRGB(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
                               green:((rgb >> 8) & 255) / 255.0
                                blue:(rgb & 255) / 255.0
                               alpha:1.0];
}

static NSFont *monoFont11(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

/* A selectable monospace label that wraps at `width` (0: no limit). */
static NSTextField *monoLabel(CGFloat width)
{
    NSTextField *label = [NSTextField wrappingLabelWithString:@""];
    label.font = monoFont11();
    label.selectable = YES;
    if (width > 0) label.preferredMaxLayoutWidth = width;
    return label;
}

/* ---- the picture ------------------------------------------------------------------ */

@interface SMSVideoPicture : NSView
@property (nonatomic, assign) smsvid_tab *tab;
@property (nonatomic, copy) void (^onChanged)(void);        /* the inspector changed */
@property (nonatomic, copy) void (^onBreak)(void);          /* a double-click: break on the pixel */
- (void)sync;                                               /* the controller re-rendered */
@end

@implementation SMSVideoPicture {
    CGImageRef _image;
    NSTrackingArea *_area;
    NSString *_message;
    NSSize _size;
}

- (BOOL)isFlipped { return YES; }

- (void)dealloc
{
    if (_image) CGImageRelease(_image);
}

- (NSSize)intrinsicContentSize { return _size.width > 0 ? _size : NSMakeSize(320, 240); }

- (void)sync
{
    int width = 0, height = 0;
    const uint32_t *pixels = smsvid_tab_picture(_tab, &width, &height);
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    if (pixels && width > 0 && height > 0) {
        /* a copy: the controller's buffer lives only until its next refresh */
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)pixels, (CFIndex)width * height * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        _image = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)width * 4, space,
                               kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider, NULL, false,
                               kCGRenderingIntentDefault);
        CGColorSpaceRelease(space);
        CGDataProviderRelease(provider);
        CFRelease(data);
        _message = nil;
        _size = NSMakeSize(width, height);
    } else {
        const char *why = smsvid_tab_why(_tab);
        _message = str(why ? why : "Nothing to show");
        _size = NSMakeSize(0, 0);
    }
    [self invalidateIntrinsicContentSize];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef context = [[NSGraphicsContext currentContext] CGContext];
    [[NSColor colorWithCalibratedRed:0.07 green:0.07 blue:0.09 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!_image) {
        NSDictionary *attrs = @{ NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.6 alpha:1],
                                 NSFontAttributeName: [NSFont systemFontOfSize:13] };
        NSString *message = _message ? _message : @"";
        const NSSize textSize = [message sizeWithAttributes:attrs];
        [message drawAtPoint:NSMakePoint((self.bounds.size.width - textSize.width) / 2,
                                         (self.bounds.size.height - textSize.height) / 2)
              withAttributes:attrs];
        return;
    }
    /* the view is flipped: draw the image upright */
    CGContextSaveGState(context);
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextTranslateCTM(context, 0, _size.height);
    CGContextScaleCTM(context, 1, -1);
    CGContextDrawImage(context, CGRectMake(0, 0, _size.width, _size.height), _image);
    CGContextRestoreGState(context);
    int hover[4] = { 0 }, pin[4] = { 0 };
    const int marks = smsvid_tab_marks(_tab, hover, pin);
    if (marks & 1) {
        [[NSColor colorWithCalibratedWhite:1 alpha:0.9] setStroke];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSMakeRect(hover[0] + 0.5, hover[1] + 0.5,
                                                                         hover[2] - 1, hover[3] - 1)];
        path.lineWidth = 1;
        [path stroke];
    }
    if (marks & 2) {
        [[NSColor colorWithCalibratedRed:1 green:0.85 blue:0.1 alpha:1] setStroke];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSMakeRect(pin[0] + 1, pin[1] + 1,
                                                                         pin[2] - 2, pin[3] - 2)];
        path.lineWidth = 2;
        [path stroke];
    }
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self userInfo:nil];
    [self addTrackingArea:_area];
}

- (NSPoint)local:(NSEvent *)event { return [self convertPoint:event.locationInWindow fromView:nil]; }

- (void)mouseMoved:(NSEvent *)event
{
    const NSPoint point = [self local:event];
    if (smsvid_tab_hover(_tab, (int)point.x, (int)point.y)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (smsvid_tab_hover(_tab, -1, -1)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint point = [self local:event];
    if (event.clickCount >= 2) {
        smsvid_tab_hover(_tab, (int)point.x, (int)point.y);
        if (_onBreak) _onBreak();
    } else {
        smsvid_tab_pin(_tab, (int)point.x, (int)point.y);
    }
    [self setNeedsDisplay:YES];
    if (_onChanged) _onChanged();
}
@end

/* ---- a colour swatch ---------------------------------------------------------------- */

@interface SMSSwatchView : NSView
@property (nonatomic, strong) NSColor *color;
@end

@implementation SMSSwatchView
- (NSSize)intrinsicContentSize { return NSMakeSize(22, 16); }

- (void)setColor:(NSColor *)color
{
    _color = color;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [(_color ? _color : NSColor.blackColor) setFill];
    NSRectFill(self.bounds);
    [[NSColor colorWithCalibratedWhite:0.5 alpha:1] setStroke];
    NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds, 0.5, 0.5)];
    path.lineWidth = 1;
    [path stroke];
}
@end

/* ---- the tab ------------------------------------------------------------------------------ */

@interface SMSVideoTab () <NSTableViewDataSource, NSTableViewDelegate, NSTabViewDelegate>
@end

@implementation SMSVideoTab {
    smsvid_tab *_tab;
    NSView *_root;
    NSPopUpButton *_viewPopup, *_sourcePopup, *_zoomPopup;
    NSButton *_overlayBox[MAX_OVERLAYS];
    NSTextField *_status, *_note, *_pixelText, *_timingText;
    NSTextField *_swatchLabel[SWATCH_ROWS];
    SMSSwatchView *_swatchView[SWATCH_ROWS];
    SMSVideoPicture *_picture;
    NSScrollView *_scroll;
    NSTabView *_sections;
    NSTableView *_regsTable, *_linesTable;
    NSProgressIndicator *_beam;
    NSString *_regsSignature, *_linesSignature;
    BOOL _updating;
    int _regCount, _lineCount;
    int _viewportWidth, _viewportHeight;
    id _resizeObserver;
}

- (NSView *)view { return _root; }

- (instancetype)initWithDebugger:(smsdebug *)debugger
{
    self = [super init];
    if (!self) return nil;
    _tab = smsvid_tab_new(debugger);
    [self build];
    return self;
}

- (void)dealloc
{
    if (_resizeObserver) [[NSNotificationCenter defaultCenter] removeObserver:_resizeObserver];
    if (_tab) smsvid_tab_free(_tab);
    _tab = NULL;
}

- (void)build
{
    int overlayCount = 0;
    const smsvid_overlay_info *overlays = smsvid_overlays(&overlayCount);
    if (overlayCount > MAX_OVERLAYS) overlayCount = MAX_OVERLAYS;
    int zoomCount = 0;
    const int *zooms = smsvid_zooms(&zoomCount);

    _viewPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < SMSVID_V_COUNT; i++) [_viewPopup addItemWithTitle:str(smsvid_view_name(i))];
    _viewPopup.target = self;
    _viewPopup.action = @selector(onView:);
    _viewPopup.toolTip = @"Display: the last finished frame, as the TV showed it.\n"
                          "Decoded: VRAM redrawn through each row's own registers and colours (see Source).\n"
                          "Beam: this frame above the beam, the last one dimmed below.\n"
                          "Values: the raw colour indices (cool = background palette, warm = sprites).";
    _sourcePopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < SMSVID_D_COUNT; i++) [_sourcePopup addItemWithTitle:str(smsvid_source_name(i))];
    _sourcePopup.target = self;
    _sourcePopup.action = @selector(onSource:);
    _sourcePopup.toolTip = @"What the Decoded view shows";
    _zoomPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < zoomCount; i++)
        [_zoomPopup addItemWithTitle:zooms[i] == 0 ? @"Fit" : [NSString stringWithFormat:@"%d×", zooms[i]]];
    _zoomPopup.target = self;
    _zoomPopup.action = @selector(onZoom:);
    NSButton *breakButton = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
    breakButton.toolTip = @"Add VRAM write breakpoints on the inspected pixel's tile pattern and name table entry";
    NSStackView *bar = [NSStackView stackViewWithViews:@[[NSTextField labelWithString:@"View"], _viewPopup,
                                                         [NSTextField labelWithString:@"Source"], _sourcePopup,
                                                         [NSTextField labelWithString:@"Zoom"], _zoomPopup,
                                                         breakButton]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 8;

    NSMutableArray<NSView *> *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int i = 0; i < overlayCount; i++) {
        _overlayBox[i] = [NSButton checkboxWithTitle:str(overlays[i].label) target:self action:@selector(onOverlay:)];
        [(i < 4 ? row1 : row2) addObject:_overlayBox[i]];
    }
    NSStackView *overlayRow1 = [NSStackView stackViewWithViews:row1];
    NSStackView *overlayRow2 = [NSStackView stackViewWithViews:row2];
    overlayRow1.orientation = overlayRow2.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    overlayRow1.spacing = overlayRow2.spacing = 10;

    _status = monoLabel(0);
    _picture = [[SMSVideoPicture alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
    _picture.tab = _tab;
    __weak SMSVideoTab *weak = self;
    _picture.onChanged = ^{ [weak updatePixel]; };
    _picture.onBreak = ^{ [weak breakOnPixel]; };
    _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 640, 400)];
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = YES;
    _scroll.documentView = _picture;
    _scroll.drawsBackground = NO;
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _note = [NSTextField labelWithString:@""];

    NSStackView *left = [NSStackView stackViewWithViews:@[bar, overlayRow1, overlayRow2, _status, _scroll, _note]];
    left.orientation = NSUserInterfaceLayoutOrientationVertical;
    left.alignment = NSLayoutAttributeLeading;
    left.spacing = 6;
    [_scroll.widthAnchor constraintEqualToAnchor:left.widthAnchor].active = YES;

    /* Fit follows the scroll view's size */
    _scroll.postsFrameChangedNotifications = YES;
    _resizeObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSViewFrameDidChangeNotification
                    object:_scroll
                     queue:nil
                usingBlock:^(NSNotification *note) {
        (void)note;
        [weak viewportChanged];
    }];

    /* ---- the sections, in smsvid_section_name order */
    _sections = [[NSTabView alloc] init];
    _sections.delegate = self;
    {   /* Pixel */
        _pixelText = monoLabel(370);
        NSButton *pixelBreak = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
        NSButton *unpin = [NSButton buttonWithTitle:@"Unpin" target:self action:@selector(onUnpin:)];
        NSStackView *buttons = [NSStackView stackViewWithViews:@[pixelBreak, unpin]];
        buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        buttons.spacing = 6;
        NSStackView *page = [NSStackView stackViewWithViews:@[_pixelText, buttons]];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 8;
        [self addSection:@"Pixel" content:page];
    }
    {   /* Registers */
        _regsTable = [[NSTableView alloc] init];
        [self addSection:@"Registers" content:[self scrollForTable:_regsTable
                                                           columns:@[@"name", @"val", @"meaning"]
                                                            titles:@[@"Register", @"Value", @"Meaning"]
                                                            widths:@[@70, @50, @400]]];
    }
    {   /* Colours: the 32 CRAM entries, each with a CRAM-write breakpoint */
        NSMutableArray<NSView *> *rows = [NSMutableArray array];
        for (int i = 0; i < SWATCH_ROWS; i++) {
            _swatchView[i] = [[SMSSwatchView alloc] initWithFrame:NSMakeRect(0, 0, 22, 16)];
            _swatchLabel[i] = monoLabel(0);
            _swatchLabel[i].selectable = NO;
            NSButton *rowBreak = [NSButton buttonWithTitle:@"Break on CRAM write" target:self
                                                    action:@selector(onSwatchBreak:)];
            rowBreak.tag = i;
            rowBreak.controlSize = NSControlSizeSmall;
            rowBreak.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
            NSStackView *row = [NSStackView stackViewWithViews:@[_swatchView[i], _swatchLabel[i], rowBreak]];
            row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            row.spacing = 8;
            [rows addObject:row];
        }
        NSStackView *page = [NSStackView stackViewWithViews:rows];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 4;
        page.translatesAutoresizingMaskIntoConstraints = NO;
        NSScrollView *colours = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
        colours.hasVerticalScroller = YES;
        colours.drawsBackground = NO;
        colours.documentView = page;
        NSClipView *clip = colours.contentView;
        [page.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
        [page.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
        [self addSection:@"Colours" content:colours];
    }
    {   /* Lines */
        _linesTable = [[NSTableView alloc] init];
        [self addSection:@"Lines" content:[self scrollForTable:_linesTable
                                                       columns:@[@"row", @"line", @"xscroll", @"yscroll", @"mode", @"regs", @"notes"]
                                                        titles:@[@"Row", @"Line", @"X", @"Y", @"Mode", @"R0-R10", @"Notes"]
                                                        widths:@[@36, @40, @32, @32, @36, @190, @70]]];
    }
    {   /* Timing */
        _beam = [[NSProgressIndicator alloc] init];
        _beam.indeterminate = NO;
        _beam.minValue = 0;
        _beam.maxValue = 1000;
        _timingText = monoLabel(370);
        NSStackView *page = [NSStackView stackViewWithViews:@[_beam, _timingText]];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 8;
        [_beam.widthAnchor constraintEqualToAnchor:page.widthAnchor].active = YES;
        [self addSection:@"Timing" content:page];
    }
    [_sections.widthAnchor constraintEqualToConstant:520].active = YES;

    NSStackView *root = [NSStackView stackViewWithViews:@[left, _sections]];
    root.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    root.alignment = NSLayoutAttributeTop;
    root.spacing = 10;
    _root = root;
    [left setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    /* start from the controller's state (the SMS_DEBUGGER_VIDEO_* variables) */
    _updating = YES;
    [_viewPopup selectItemAtIndex:smsvid_tab_view(_tab)];
    [_sourcePopup selectItemAtIndex:smsvid_tab_source(_tab)];
    _sourcePopup.enabled = smsvid_tab_view(_tab) == SMSVID_V_DECODED;
    for (int i = 0; i < zoomCount; i++)
        if (zooms[i] == smsvid_tab_zoom(_tab)) [_zoomPopup selectItemAtIndex:i];
    for (int i = 0; i < overlayCount; i++)
        _overlayBox[i].state = (smsvid_tab_overlays(_tab) & overlays[i].bit) ? NSControlStateValueOn
                                                                             : NSControlStateValueOff;
    const int section = smsvid_tab_env_section(_tab);
    if (section > 0 && section < (int)_sections.numberOfTabViewItems) [_sections selectTabViewItemAtIndex:section];
    _updating = NO;
}

/* Sets a table up and returns the scroll view that holds it. */
- (NSScrollView *)scrollForTable:(NSTableView *)table
                         columns:(NSArray<NSString *> *)ids
                          titles:(NSArray<NSString *> *)titles
                          widths:(NSArray<NSNumber *> *)widths
{
    for (NSUInteger i = 0; i < ids.count; i++) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:ids[i]];
        column.title = titles[i];
        column.width = widths[i].doubleValue;
        [table addTableColumn:column];
    }
    table.dataSource = self;
    table.delegate = self;
    table.usesAlternatingRowBackgroundColors = YES;
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    scroll.hasVerticalScroller = YES;
    scroll.documentView = table;
    return scroll;
}

- (void)addSection:(NSString *)title content:(NSView *)content
{
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title;
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
    [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
    [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
    if ([content isKindOfClass:[NSScrollView class]])
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
    item.view = holder;
    [_sections addTabViewItem:item];
}

/* ---- actions -------------------------------------------------------------------------- */

- (void)onView:(id)sender
{
    (void)sender;
    if (_updating) return;
    smsvid_tab_set_view(_tab, (int)_viewPopup.indexOfSelectedItem);
    _sourcePopup.enabled = smsvid_tab_view(_tab) == SMSVID_V_DECODED;
    [self refreshView:SMSVID_TAB_PICTURE | SMSVID_TAB_TABLES];
}

- (void)onSource:(id)sender
{
    (void)sender;
    if (_updating) return;
    smsvid_tab_set_source(_tab, (int)_sourcePopup.indexOfSelectedItem);
    [self refreshView:SMSVID_TAB_PICTURE | SMSVID_TAB_TABLES];
}

- (void)onZoom:(id)sender
{
    (void)sender;
    int zoomCount = 0;
    const int *zooms = smsvid_zooms(&zoomCount);
    const NSInteger index = _zoomPopup.indexOfSelectedItem;
    if (_updating || index < 0 || index >= zoomCount) return;
    smsvid_tab_set_zoom(_tab, zooms[index]);
    [self refreshView:SMSVID_TAB_PICTURE];
}

- (void)onOverlay:(id)sender
{
    (void)sender;
    if (_updating) return;
    int overlayCount = 0;
    const smsvid_overlay_info *overlays = smsvid_overlays(&overlayCount);
    if (overlayCount > MAX_OVERLAYS) overlayCount = MAX_OVERLAYS;
    unsigned mask = 0;
    for (int i = 0; i < overlayCount; i++)
        if (_overlayBox[i].state == NSControlStateValueOn) mask |= overlays[i].bit;
    smsvid_tab_set_overlays(_tab, mask);
    [self refreshView:SMSVID_TAB_PICTURE];
}

/* The pixel's VRAM write breakpoints (its tile pattern and name table entry), with a note of what happened. */
- (void)breakOnPixel
{
    const int ident = smsvid_tab_break_on_pixel(_tab);
    _note.stringValue = ident >= 0 ? @"Break on VRAM writes to the pixel's tile and name table entry (see Breakpoints)"
                                   : @"Nothing to break on";
}

- (void)onBreak:(id)sender
{
    (void)sender;
    [self breakOnPixel];
}

- (void)onSwatchBreak:(id)sender
{
    const int index = (int)((NSButton *)sender).tag;
    const smsvid_swatch *swatches = NULL;
    const int count = smsvid_tab_swatches(_tab, &swatches);
    const int ident = smsvid_tab_break_on_swatch(_tab, index);
    if (ident >= 0 && swatches && index >= 0 && index < count)
        _note.stringValue = [NSString stringWithFormat:@"Break on CRAM writes to %@ (see Breakpoints)",
                                                       str(swatches[index].name)];
    else
        _note.stringValue = @"Nothing to break on";
}

- (void)onUnpin:(id)sender
{
    (void)sender;
    smsvid_tab_unpin(_tab);
    [_picture setNeedsDisplay:YES];
    [self updatePixel];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    if (!_updating) [self refreshView:SMSVID_TAB_TABLES];
}

/* The picture's room changed: Fit needs a new picture. */
- (void)viewportChanged
{
    const NSSize viewport = _scroll.contentSize;
    if ((int)viewport.width == _viewportWidth && (int)viewport.height == _viewportHeight) return;
    if (smsvid_tab_zoom(_tab) == 0) {
        [self refreshView:SMSVID_TAB_PICTURE];
    } else {
        _viewportWidth = (int)viewport.width;
        _viewportHeight = (int)viewport.height;
        smsvid_tab_set_viewport(_tab, _viewportWidth > 2 ? _viewportWidth - 2 : 1,
                                _viewportHeight > 2 ? _viewportHeight - 2 : 1);
    }
}

/* ---- the sections ----------------------------------------------------------------------- */

- (void)updatePixel
{
    const smsvid_pixel *pixel = smsvid_tab_pixel(_tab);
    _pixelText.stringValue = pixel ? str(pixel->text)
                                   : @"Move over the picture to inspect a pixel.\nClick to pin it; click again to unpin.\n"
                                      "Double-click to break on writes to it.";
    /* the swatch rows mark the inspected colour */
    if ([self sectionIndex] == SMSVID_S_COLOURS) [self fillColours];
}

- (NSInteger)sectionIndex
{
    return (NSInteger)[_sections.tabViewItems indexOfObject:_sections.selectedTabViewItem];
}

static NSString *hexBytes(const uint8_t *values, int count)
{
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < count; i++) [text appendFormat:@"%s%02X", i ? " " : "", values[i]];
    return text;
}

static NSString *changeNotes(int mask)
{
    NSMutableString *text = [NSMutableString string];
    if (mask & SMSVID_LC_XSCROLL) [text appendString:@"X "];
    if (mask & SMSVID_LC_YSCROLL) [text appendString:@"Y "];
    if (mask & SMSVID_LC_REGS) [text appendString:@"R "];
    if (mask & SMSVID_LC_MODE) [text appendString:@"M "];
    if (mask & SMSVID_LC_CRAM) [text appendString:@"C "];
    return text;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)table
{
    return table == _linesTable ? _lineCount : _regCount;
}

- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    NSString *text = @"";
    BOOL hot = NO;
    if (table == _regsTable) {
        const smsvid_reg *regs = NULL;
        const int count = smsvid_tab_regs(_tab, &regs);
        if (!regs || row < 0 || row >= count) return nil;
        if ([column.identifier isEqualToString:@"name"]) text = str(regs[row].name);
        else if ([column.identifier isEqualToString:@"val"]) text = str(regs[row].value);
        else text = str(regs[row].meaning);
        hot = regs[row].age < SMSVID_REG_FADE;
    } else {
        const smsvid_line_row *lines = NULL;
        const int count = smsvid_tab_lines(_tab, &lines);
        if (!lines || row < 0 || row >= count) return nil;
        NSString *identifier = column.identifier;
        if ([identifier isEqualToString:@"row"]) text = [NSString stringWithFormat:@"%d", lines[row].row];
        else if ([identifier isEqualToString:@"line"]) text = [NSString stringWithFormat:@"%d", lines[row].line];
        else if ([identifier isEqualToString:@"xscroll"]) text = [NSString stringWithFormat:@"%02X", lines[row].x_scroll];
        else if ([identifier isEqualToString:@"yscroll"]) text = [NSString stringWithFormat:@"%02X", lines[row].y_scroll];
        else if ([identifier isEqualToString:@"mode"]) text = [NSString stringWithFormat:@"%d", lines[row].mode];
        else if ([identifier isEqualToString:@"regs"]) text = hexBytes(lines[row].reg, 11);
        else text = [NSString stringWithFormat:@"%@%@%@", lines[row].blank ? @"off " : @"",
                                               lines[row].hint ? @"hint " : @"", changeNotes(lines[row].changed_mask)];
        hot = lines[row].changed_mask != 0;
    }
    NSTextField *field = [table makeViewWithIdentifier:column.identifier owner:self];
    if (![field isKindOfClass:[NSTextField class]]) {
        field = [NSTextField labelWithString:@""];
        field.identifier = column.identifier;
        field.font = monoFont11();
    }
    field.stringValue = text;
    field.drawsBackground = hot;
    field.backgroundColor = hot ? SMSAccentColor() : NSColor.clearColor;
    field.textColor = hot ? NSColor.whiteColor : NSColor.labelColor;
    return field;
}

- (void)fillRegs
{
    const smsvid_reg *regs = NULL;
    const int count = smsvid_tab_regs(_tab, &regs);
    NSMutableString *signature = [NSMutableString string];
    for (int i = 0; regs && i < count; i++)
        [signature appendFormat:@"%s%c", regs[i].value, regs[i].age < SMSVID_REG_FADE ? '*' : '.'];
    [signature appendFormat:@"%d", count];
    if ([signature isEqualToString:_regsSignature]) return;
    _regsSignature = signature;
    _regCount = regs ? count : 0;
    [_regsTable reloadData];
}

- (void)fillLines
{
    const smsvid_line_row *lines = NULL;
    const int count = smsvid_tab_lines(_tab, &lines);
    NSMutableString *signature = [NSMutableString string];
    for (int i = 0; lines && i < count; i++) {
        [signature appendFormat:@"%d:%d:", lines[i].row, lines[i].line];
        [signature appendString:hexBytes(lines[i].reg, 11)];
        [signature appendFormat:@"%02X%02X%d%d%d%d;", lines[i].x_scroll, lines[i].y_scroll, lines[i].mode,
                                lines[i].blank, lines[i].hint, lines[i].changed_mask];
    }
    [signature appendFormat:@"%d", count];
    if ([signature isEqualToString:_linesSignature]) return;
    _linesSignature = signature;
    _lineCount = lines ? count : 0;
    [_linesTable reloadData];
}

- (void)fillColours
{
    const smsvid_swatch *swatches = NULL;
    const int count = smsvid_tab_swatches(_tab, &swatches);
    for (int i = 0; i < SWATCH_ROWS; i++) {
        if (!swatches || i >= count) {
            _swatchLabel[i].stringValue = @"";
            _swatchView[i].color = nil;
            continue;
        }
        _swatchView[i].color = colorFromRGB(swatches[i].rgb);
        _swatchLabel[i].stringValue = [NSString stringWithFormat:@"%@%-6s $%02X  #%06X  %ld pixels",
                                                                 swatches[i].inspected ? @"> " : @"  ",
                                                                 swatches[i].name, swatches[i].cram & 255,
                                                                 swatches[i].rgb & 0xFFFFFF, swatches[i].uses];
    }
}

- (void)fillTiming
{
    const char *const *lines = NULL;
    const int count = smsvid_tab_timing(_tab, &lines);
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; lines && i < count; i++) {
        NSString *line = str(lines[i]);
        const NSRange tab = [line rangeOfString:@"\t"];
        if (tab.location != NSNotFound)
            [text appendFormat:@"%-13s %@\n", [[line substringToIndex:tab.location] UTF8String],
                               [line substringFromIndex:tab.location + 1]];
        else
            [text appendFormat:@"%@\n", line];
    }
    _timingText.stringValue = count ? text : @"The machine is off";
    _beam.doubleValue = smsvid_tab_beam_permille(_tab);
}

/* ---- refresh ------------------------------------------------------------------------------ */

- (void)refreshView:(unsigned)what
{
    if (what & SMSVID_TAB_PICTURE) {
        const NSSize viewport = _scroll.contentSize;
        _viewportWidth = (int)viewport.width;
        _viewportHeight = (int)viewport.height;
        if (viewport.width > 4 && viewport.height > 4)
            smsvid_tab_set_viewport(_tab, (int)viewport.width - 2, (int)viewport.height - 2);
        [_picture sync];
        [_picture setFrameSize:_picture.intrinsicContentSize];
    }
    if (what & SMSVID_TAB_TABLES) {
        _status.stringValue = str(smsvid_tab_status(_tab));
        switch ([self sectionIndex]) {
        case SMSVID_S_REGISTERS: [self fillRegs]; break;
        case SMSVID_S_COLOURS: [self fillColours]; break;
        case SMSVID_S_LINES: [self fillLines]; break;
        case SMSVID_S_TIMING: [self fillTiming]; break;
        default: break;
        }
        const smsvid_pixel *pixel = smsvid_tab_pixel(_tab);
        _pixelText.stringValue = pixel ? str(pixel->text)
                                       : @"Move over the picture to inspect a pixel.\nClick to pin it; click again to unpin.\n"
                                          "Double-click to break on writes to it.";
    }
}

- (void)refreshForce:(BOOL)force
{
    unsigned what = smsvid_tab_refresh(_tab, force ? 1 : 0);
    if (force) what |= SMSVID_TAB_PICTURE | SMSVID_TAB_TABLES;
    if (what & SMSVID_TAB_OFF) what |= SMSVID_TAB_PICTURE | SMSVID_TAB_TABLES;     /* the message */
    [self refreshView:what];
}

@end
