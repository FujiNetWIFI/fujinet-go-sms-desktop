/*
 * Debugger window (AppKit) over this app's Z80 / 315-5124 debugger engine,
 * via core/include/smsdebug.h. Mirrors the GTK, Qt and Win32 debuggers tab
 * for tab: Prompt, CPU & RAM, Disassembly, VDP, Sound & I/O, Breakpoints,
 * Cart.
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it detaches, and the machine runs on.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "../KeyForward.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "smsdebug.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define NUM_REGS 17
#define NUM_FLAGS 6
#define CRAM_ENTRIES 32
#define TAB_VDP 3            /* the tab refreshed live while running */

/* The console RAM dump: a header line, then 16 bytes a line. */
#define RAM_HEADER @"       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n"
#define RAM_LINE_LEN 56      /* "$C000: " + 16 * "XX " + "\n" */

static SMSDebuggerWindow *g_debugger;

/* The open panels' filters: a type per extension (a dynamic one for an
 * extension nothing has registered, which still matches by extension). */
static NSArray<UTType *> *typesForExtensions(NSArray<NSString *> *exts)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *t = [UTType typeWithFilenameExtension:ext];
        if (t) [types addObject:t];
    }
    return types;
}

/* F12 closes the debugger from inside it, as it opens it from the
 * machine's window. Taken as a key equivalent, so it works wherever the
 * focus is in the window. */
@interface DebuggerPanelWindow : NSWindow
@end

@implementation DebuggerPanelWindow
static BOOL isF12(NSEvent *event)
{
    NSString *chars = [event charactersIgnoringModifiers];
    return [event type] == NSEventTypeKeyDown && [chars length] == 1 &&
           [chars characterAtIndex:0] == NSF12FunctionKey;
}

- (BOOL)performKeyEquivalent:(NSEvent *)event
{
    if (isF12(event)) {
        [self performClose:nil];
        return YES;
    }
    return [super performKeyEquivalent:event];
}

/* and when nothing in the window took it as a key equivalent */
- (void)keyDown:(NSEvent *)event
{
    if (isF12(event)) {
        [self performClose:nil];
        return;
    }
    [super keyDown:event];
}
@end

/* Disassembly text view: a click toggles the breakpoint on the clicked
 * line; the wheel browses. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onToggleLine)(int line);
@property (nonatomic, copy) void (^onScroll)(int lines);
@end

@implementation DasmTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    for (NSUInteger i = 0; i < idx && i < text.length; i++)
        if ([text characterAtIndex:i] == '\n') line++;
    if (self.onToggleLine) self.onToggleLine(line);
}

- (void)scrollWheel:(NSEvent *)event
{
    if (self.onScroll) self.onScroll((int)(-event.scrollingDeltaY / 6.0));
}
@end

/* A VDP view: nearest-neighbour at 2x when it fits, otherwise as large as
 * fits at the image's own proportions; letterboxed either way. */
@interface VdpPictureView : NSView
@property (nonatomic) CGImageRef image;
@end

@implementation VdpPictureView
- (void)setImage:(CGImageRef)image
{
    if (_image) CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}
- (void)dealloc { if (_image) CGImageRelease(_image); }
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];
    CGContextSetRGBFillColor(dc, 0.1, 0.1, 0.1, 1);
    CGContextFillRect(dc, b);
    if (!_image) return;
    const double iw = (double)CGImageGetWidth(_image), ih = (double)CGImageGetHeight(_image);
    double w = b.size.width, h = b.size.height, sw, sh;
    if (w >= 2 * iw && h >= 2 * ih) { sw = 2 * iw; sh = 2 * ih; }
    else {
        const double want = iw / ih;
        if (w / h > want) { sh = h; sw = sh * want; } else { sw = w; sh = sw / want; }
    }
    CGContextSetInterpolationQuality(dc, kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake(floor((w - sw) / 2), floor((h - sh) / 2), sw, sh), _image);
}
@end

/* One CRAM entry's colour. */
@interface CramSwatch : NSView
@property (nonatomic, strong) NSColor *color;
@end

@implementation CramSwatch
- (void)setColor:(NSColor *)color
{
    _color = color;
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [(_color ?: NSColor.blackColor) setFill];
    NSRectFill([self bounds]);
    [[NSColor separatorColor] setStroke];
    [NSBezierPath strokeRect:NSInsetRect([self bounds], 0.5, 0.5)];
}
@end

@interface SMSDebuggerWindow ()
- (instancetype)initWithSession:(smssession *)session;
- (void)refreshAll;
@end

@implementation SMSDebuggerWindow {
    smssession *_session;
    smsdebug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    unsigned _seenGen;
    BOOL _wasStopped;
    int _runningTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;

    NSTextView *_promptOut;
    NSTextField *_promptIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    NSTextField *_reg[NUM_REGS];
    NSButton *_flag[NUM_FLAGS];
    NSTextField *_beam;
    NSTextView *_ram;
    NSTextField *_ramGoto, *_ramAddr, *_ramVal;

    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;

    NSTextView *_vdpText;
    NSPopUpButton *_vdpViewSel;
    NSPopUpButton *_vdpPalette;
    VdpPictureView *_vdpPic;
    uint32_t *_viewPx;
    CramSwatch *_cramSwatch[CRAM_ENTRIES];
    NSTextField *_cramVal[CRAM_ENTRIES];

    NSTextView *_sound;

    NSPopUpButton *_bpType;
    NSTextField *_bpRange, *_bpCond;
    NSStackView *_bpList;

    NSTextView *_cart;
}

#define NUM_FILE_KINDS 7
static const char *const kFileKinds[NUM_FILE_KINDS] = { "dis", "ram", "vram", "cram", "sram", "arena", "regs" };
static NSString *const kFileTitles[NUM_FILE_KINDS] = {
    @"Disassembly…", @"Console RAM…", @"VRAM…", @"CRAM…", @"Cartridge SRAM…", @"Mailbox Arena…", @"Registers…"
};
static NSString *const kFileNames[NUM_FILE_KINDS] = {
    @"disassembly.asm", @"ram.bin", @"vram.bin", @"cram.bin", @"sram.bin", @"arena.bin", @"registers.txt"
};

/* SMS_DEBUGGER_TAB takes an index or one of these. */
#define NUM_TABS 7
static const char *const kTabNames[NUM_TABS] = {
    "prompt", "cpu", "disassembly", "vdp", "sound", "breakpoints", "cart"
};

/* ---- helpers --------------------------------------------------------------- */

static NSString *stripControl(const char *s)
{
    NSMutableString *out = [NSMutableString string];
    char buf[2] = { 0, 0 };
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') {
            buf[0] = *s;
            [out appendString:[NSString stringWithUTF8String:buf] ?: @""];
        }
    }
    return out;
}

/* $hex, 0xhex, hexh, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    else if (t.length > 1 && [t.lowercaseString hasSuffix:@"h"]) t = [t substringToIndex:t.length - 1];
    if (t.length == 0) return NO;
    char *end;
    long v = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* A label, or a number. -1 when neither. */
- (int)resolveAddr:(NSString *)text
{
    long v;
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (t.length == 0) return -1;
    int addr = smsdebug_label_address(_dbg, t.UTF8String);
    if (addr < 0 && parseNum(t, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    if (wrap) {
        view.autoresizingMask = NSViewWidthSizable;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* Replace a text view's text, keeping the reader's place. */
static void setTextKeepingPlace(NSTextView *view, NSString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    view.string = text;
    if (scroll) [scroll.contentView scrollToPoint:at];
}

static NSColor *colorFromXrgb(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                               green:((rgb >> 8) & 0xff) / 255.0
                                blue:(rgb & 0xff) / 255.0
                               alpha:1.0];
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    [b setRefusesFirstResponder:YES];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

/* A pull-down button: `title` on its face, `items` underneath (each one's
 * tag is its index). */
- (NSPopUpButton *)pullDown:(NSString *)title items:(NSArray<NSString *> *)items action:(SEL)sel
{
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [p addItemWithTitle:title];
    p.lastItem.tag = -1;
    for (NSUInteger i = 0; i < items.count; i++) {
        [p addItemWithTitle:items[i]];
        p.lastItem.tag = (NSInteger)i;
    }
    p.target = self;
    p.action = sel;
    [p setRefusesFirstResponder:YES];
    return p;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    return s;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 6;
    return s;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* ---- lifetime -------------------------------------------------------------- */

+ (void)showForSession:(smssession *)session
{
    if (!g_debugger) g_debugger = [[SMSDebuggerWindow alloc] initWithSession:session];
    [g_debugger->_window makeKeyAndOrderFront:nil];
    smsdebug_attach(g_debugger->_dbg);
    [g_debugger refreshAll];
}

+ (void)toggleForSession:(smssession *)session
{
    if (g_debugger && [g_debugger->_window isVisible]) {
        /* windowWillClose: detaches */
        [g_debugger->_window close];
        return;
    }
    [self showForSession:session];
}

- (instancetype)initWithSession:(smssession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = smssession_debugger(session);
    _viewPx = calloc(SMSDEBUG_VIEW_MAX_PIXELS, sizeof *_viewPx);
    _history = [NSMutableArray array];
    [self buildWindow];
    __weak SMSDebuggerWindow *weakSelf = self;
    _tick = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    free(_viewPx);
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    smsdebug_detach(_dbg);
}

- (void)onTick
{
    if (![_window isVisible]) return;
    const unsigned gen = smsdebug_generation(_dbg);
    const BOOL stopped = smsdebug_is_stopped(_dbg) != 0;
    if (gen != _seenGen || stopped != _wasStopped) {
        _seenGen = gen;
        _wasStopped = stopped;
        [self refreshAll];
    } else if (!stopped && ++_runningTicks >= 5) {
        /* running: the live values twice a second */
        _runningTicks = 0;
        [self refreshStatus];
        [self refreshCpu];
        if ([_tabs indexOfTabViewItem:_tabs.selectedTabViewItem] == TAB_VDP) [self refreshVdp];
        [self refreshSound];
        [self refreshCart];
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildPrompt
{
    NSScrollView *outScroll;
    _promptOut = monoView(&outScroll, YES);
    _promptOut.string = @"Debugger prompt. Type 'help' for every command.\n";
    _promptIn = [self field:@"command (help, step, break $0038, bpw $C000, print hl, mem $C000 32, ...) — Tab completes, ↑↓ history"
                      width:0 action:@selector(runPrompt:)];
    _promptIn.delegate = self;

    NSStackView *v = vstack(@[outScroll, _promptIn]);
    fill(v, outScroll);
    fill(v, _promptIn);
    return v;
}

- (NSView *)buildCpu
{
    static NSString *const names[NUM_REGS] = {
        @"PC", @"SP", @"AF", @"BC", @"DE", @"HL", @"IX", @"IY",
        @"AF'", @"BC'", @"DE'", @"HL'", @"I", @"R", @"IM", @"IFF1", @"IFF2"
    };
    NSMutableArray *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int i = 0; i < NUM_REGS; i++) {
        /* the 16-bit pairs, then I and R, then IM and the two flip-flops */
        const CGFloat width = i < 12 ? 52 : (i < 14 ? 36 : 28);
        NSMutableArray *row = i < 8 ? row1 : row2;
        [row addObject:[self label:names[i]]];
        _reg[i] = [self field:@"" width:width action:@selector(applyRegister:)];
        _reg[i].tag = i;
        [row addObject:_reg[i]];
    }
    static NSString *const fnames[NUM_FLAGS] = { @"S", @"Z", @"H", @"P/V", @"N", @"C" };
    NSMutableArray *flags = [NSMutableArray arrayWithObject:[self label:@"Flags"]];
    for (int i = 0; i < NUM_FLAGS; i++) {
        _flag[i] = [NSButton checkboxWithTitle:fnames[i] target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [flags addObject:_flag[i]];
    }
    _beam = [self label:@""];
    _beam.textColor = NSColor.secondaryLabelColor;

    NSScrollView *ramScroll;
    _ram = monoView(&ramScroll, NO);
    _ramGoto = [self field:@"$C000 or a label" width:140 action:@selector(gotoRam:)];
    NSStackView *ramHead = hstack(@[[self label:@"Console RAM ($C000-$DFFF)"], [self label:@"Go to"], _ramGoto]);
    _ramAddr = [self field:@"$C000" width:110 action:@selector(writeRam:)];
    _ramVal = [self field:@"$00" width:60 action:@selector(writeRam:)];
    NSTextField *hint = [self label:@"RAM takes it as a write; below $C000 it goes into the cartridge's memory"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *edit = hstack(@[[self label:@"Write address"], _ramAddr, [self label:@"value"], _ramVal, hint]);

    NSStackView *v = vstack(@[hstack(row1), hstack(row2), hstack(flags), _beam, ramHead, ramScroll, edit]);
    fill(v, ramScroll);
    return v;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(refreshDisasm)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address or label" width:150 action:@selector(jumpTo:)];
    NSTextField *hint = [self label:@"Click a line to toggle its breakpoint (*); scroll to browse"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump, hint]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _disasm.horizontallyResizable = YES;
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak SMSDebuggerWindow *weakSelf = self;
    _disasm.onToggleLine = ^(int line) {
        SMSDebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        smsdebug_breakpoint_toggle(s->_dbg, s->_lineAddr[line]);
        [s refreshDisasm];
        [s refreshBps];
    };
    _disasm.onScroll = ^(int lines) {
        SMSDebuggerWindow *s = weakSelf;
        if (!s || lines == 0) return;
        s->_followPc.state = NSControlStateValueOff;
        s->_disasmAddr = (uint16_t)smsdebug_row_address(s->_dbg, s->_disasmAddr, lines);
        [s refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *v = vstack(@[row, scroll]);
    fill(v, scroll);
    return v;
}

- (NSView *)buildVdp
{
    NSScrollView *textScroll;
    _vdpText = monoView(&textScroll, NO);
    NSStackView *left = vstack(@[textScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the text takes the column's height */
    fill(left, textScroll);

    _vdpViewSel = [[NSPopUpButton alloc] init];
    [_vdpViewSel addItemsWithTitles:@[@"Name table", @"Tiles", @"Sprites", @"Palette"]];
    _vdpViewSel.target = self;
    _vdpViewSel.action = @selector(refreshVdp);
    _vdpPalette = [[NSPopUpButton alloc] init];
    [_vdpPalette addItemsWithTitles:@[@"Background palette", @"Sprite palette"]];
    _vdpPalette.target = self;
    _vdpPalette.action = @selector(refreshVdp);
    NSStackView *controls = hstack(@[_vdpViewSel, _vdpPalette]);

    _vdpPic = [[VdpPictureView alloc] initWithFrame:NSMakeRect(0, 0, 512, 512)];
    [_vdpPic.widthAnchor constraintGreaterThanOrEqualToConstant:512].active = YES;
    [_vdpPic.heightAnchor constraintGreaterThanOrEqualToConstant:400].active = YES;

    /* CRAM: the background palette over the sprite palette, each entry's
     * colour above its value (Enter writes it). */
    NSMutableArray *cramRows = [NSMutableArray array];
    for (int row = 0; row < 2; row++) {
        NSMutableArray *cells = [NSMutableArray array];
        for (int col = 0; col < 16; col++) {
            const int i = row * 16 + col;
            _cramSwatch[i] = [[CramSwatch alloc] initWithFrame:NSMakeRect(0, 0, 28, 16)];
            [_cramSwatch[i].widthAnchor constraintEqualToConstant:28].active = YES;
            [_cramSwatch[i].heightAnchor constraintEqualToConstant:16].active = YES;
            _cramVal[i] = [self field:@"" width:28 action:@selector(writeCram:)];
            _cramVal[i].tag = i;
            NSStackView *cellStack = vstack(@[_cramSwatch[i], _cramVal[i]]);
            cellStack.spacing = 2;
            [cells addObject:cellStack];
        }
        NSStackView *r = hstack(cells);
        r.spacing = 3;
        [cramRows addObject:r];
    }
    NSStackView *cram = vstack(@[[self label:@"CRAM (background, then sprites)"], cramRows[0], cramRows[1]]);

    NSStackView *right = vstack(@[controls, _vdpPic, cram]);
    fill(right, _vdpPic);

    NSStackView *h = hstack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:360].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

- (NSView *)buildSound
{
    NSScrollView *scroll;
    _sound = monoView(&scroll, YES);
    return scroll;
}

- (NSView *)buildBreaks
{
    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Read/Write", @"Port In", @"Port Out", @"Port In/Out"]];
    _bpRange = [self field:@"$0038, $C000-$C0FF, a label, or port $7E" width:240 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): a == $10 && [$C000] > 2" width:0 action:@selector(addBreakpoint:)];
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clear = [self button:@"Clear all" action:@selector(clearBreaks:)];
    NSStackView *row = hstack(@[_bpType, _bpRange, _bpCond, add, clear]);

    /* The rows scroll: a session can collect more breakpoints than the tab
     * is tall. The stack is the scroll view's document, pinned to the top
     * and to the clip view's width so rows keep the tab's width. */
    _bpList = vstack(@[]);
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;
    NSTextField *hint = [self label:@"Untick to disable; conditions are C expressions over registers, flags, [addr], value and addr"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *v = vstack(@[row, hint, bpScroll]);
    fill(v, row);
    fill(v, bpScroll);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                         forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

- (NSView *)buildCart
{
    NSScrollView *scroll;
    _cart = monoView(&scroll, NO);
    return scroll;
}

- (void)buildWindow
{
    _window = [[DebuggerPanelWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1100, 780)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row. F5/F7/F8/Shift+F8 work as the buttons' key equivalents
     * wherever the focus is inside the window. */
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *over = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    NSButton *scan = [self button:@"Scanline+1" action:@selector(scanline:)];
    NSButton *frame = [self button:@"Frame+1" action:@selector(frame:)];
    _runBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    _runBtn.keyEquivalentModifierMask = 0;
    step.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    step.keyEquivalentModifierMask = 0;
    over.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    over.keyEquivalentModifierMask = 0;
    outBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    outBtn.keyEquivalentModifierMask = NSEventModifierFlagShift;
    NSPopUpButton *symbols = [self pullDown:@"Load Symbols…"
                                      items:@[@"From a File…", @"Beside the Cartridge"]
                                     action:@selector(loadSymbols:)];
    NSMutableArray *saveTitles = [NSMutableArray array];
    for (int i = 0; i < NUM_FILE_KINDS; i++) [saveTitles addObject:kFileTitles[i]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView *toolbar = hstack(@[_runBtn, step, over, outBtn, scan, frame, symbols, save, _status]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Prompt", [self buildPrompt]], @[@"CPU & RAM", [self buildCpu]],
                        @[@"Disassembly", [self buildDisasm]], @[@"VDP", [self buildVdp]],
                        @[@"Sound & I/O", [self buildSound]], @[@"Breakpoints", [self buildBreaks]],
                        @[@"Cart", [self buildCart]] ];
    for (NSArray *page in pages) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:page[0]];
        item.label = page[0];
        NSView *content = page[1];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        [_tabs addTabViewItem:item];
    }
    const char *tab = getenv("SMS_DEBUGGER_TAB");
    if (tab && *tab) {
        int t = -1;
        for (int i = 0; i < NUM_TABS; i++)
            if (!strcasecmp(tab, kTabNames[i])) t = i;
        if (t < 0 && tab[0] >= '0' && tab[0] <= '9') t = atoi(tab);
        if (t >= 0 && t < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:t];
    }

    NSStackView *root = vstack(@[toolbar, _tabs]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

- (void)toggleRun:(id)sender
{
    (void)sender;
    if (smsdebug_is_stopped(_dbg)) smsdebug_resume(_dbg); else smsdebug_stop(_dbg);
    [self refreshAll];
}
- (void)step:(id)sender { (void)sender; smsdebug_step(_dbg); [self refreshAll]; }
- (void)stepOver:(id)sender { (void)sender; smsdebug_step_over(_dbg); [self refreshAll]; }
- (void)stepOut:(id)sender { (void)sender; smsdebug_step_out(_dbg); [self refreshAll]; }
- (void)scanline:(id)sender { (void)sender; smsdebug_scanline(_dbg, 1); [self refreshAll]; }
- (void)frame:(id)sender { (void)sender; smsdebug_frame(_dbg, 1); [self refreshAll]; }

- (void)appendPrompt:(NSString *)text
{
    [_promptOut.textStorage appendAttributedString:
        [[NSAttributedString alloc] initWithString:text attributes:@{ NSFontAttributeName: _promptOut.font ?: [NSFont userFixedPitchFontOfSize:11] }]];
    [_promptOut scrollRangeToVisible:NSMakeRange(_promptOut.string.length, 0)];
}

- (void)runPrompt:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_promptIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (cmd.length == 0) return;
    if (![_history.lastObject isEqualToString:cmd]) [_history addObject:cmd];
    _historyPos = (NSInteger)_history.count;
    [self appendPrompt:[NSString stringWithFormat:@"> %@\n", cmd]];
    smsdebug_command(_dbg, cmd.UTF8String, out, sizeof out);
    [self appendPrompt:[stripControl(out) stringByAppendingString:@"\n"]];
    _promptIn.stringValue = @"";
    [self refreshAll];
}

/* In the prompt, Tab completes instead of moving the focus, and Up/Down
 * walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _promptIn) return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger n = (NSInteger)_history.count;
        if (n == 0) return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0) _historyPos = 0;
        if (_historyPos > n) _historyPos = n;
        _promptIn.stringValue = _historyPos < n ? _history[(NSUInteger)_historyPos] : @"";
        [_promptIn.currentEditor setSelectedRange:NSMakeRange(_promptIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:)) return NO;
    NSString *text = _promptIn.stringValue;
    const NSRange sp = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *word = sp.location == NSNotFound ? text : [text substringFromIndex:sp.location + 1];
    char comps[4096];
    const int n = smsdebug_completions(_dbg, word.UTF8String, comps, sizeof comps);
    if (n == 1) {
        NSString *c = [[NSString stringWithUTF8String:comps] componentsSeparatedByString:@"\n"][0];
        NSString *head = sp.location == NSNotFound ? @"" : [text substringToIndex:sp.location + 1];
        _promptIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, c];
        [_promptIn.currentEditor setSelectedRange:NSMakeRange(_promptIn.stringValue.length, 0)];
    } else if (n > 1) {
        NSString *list = [NSString stringWithUTF8String:comps] ?: @"";
        if (![list hasSuffix:@"\n"]) list = [list stringByAppendingString:@"\n"];
        [self appendPrompt:list];
    }
    return YES;
}

/* "From a File…" asks; "Beside the Cartridge" takes <cart>.sym/.map/.noi
 * next to the opened cartridge. */
- (void)loadSymbols:(NSPopUpButton *)sender
{
    char msg[512];
    if (sender.selectedItem.tag == 1) {
        smsdebug_load_symbols(_dbg, NULL, msg, sizeof msg);
    } else {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.title = @"Load Symbols (WLA-DX .sym, z88dk .map, SDCC .noi, or an address list)";
        p.allowedContentTypes = typesForExtensions(@[@"sym", @"map", @"noi", @"txt"]);
        const char *cart = smssession_cart_path(_session);
        if (cart && cart[0])
            [p setDirectoryURL:[NSURL fileURLWithPath:[[NSString stringWithUTF8String:cart]
                                                          stringByDeletingLastPathComponent] isDirectory:YES]];
        if ([p runModal] != NSModalResponseOK) return;
        smsdebug_load_symbols(_dbg, [[p URL] fileSystemRepresentation], msg, sizeof msg);
    }
    [self appendPrompt:[stripControl(msg) stringByAppendingString:@"\n"]];
    _status.stringValue = stripControl(msg);
    [self refreshAll];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_FILE_KINDS) return;
    NSSavePanel *p = [NSSavePanel savePanel];
    p.title = [NSString stringWithFormat:@"Save %@", kFileTitles[kind]];
    [p setNameFieldStringValue:kFileNames[kind]];
    if ([p runModal] != NSModalResponseOK) return;
    char msg[512];
    smsdebug_save(_dbg, kFileKinds[kind], [[p URL] fileSystemRepresentation], msg, sizeof msg);
    [self appendPrompt:[stripControl(msg) stringByAppendingString:@"\n"]];
    _status.stringValue = stripControl(msg);
}

- (void)applyRegister:(NSTextField *)sender
{
    static const int regIds[NUM_REGS] = {
        SMS_REG_PC, SMS_REG_SP, SMS_REG_AF, SMS_REG_BC, SMS_REG_DE, SMS_REG_HL, SMS_REG_IX, SMS_REG_IY,
        SMS_REG_AF2, SMS_REG_BC2, SMS_REG_DE2, SMS_REG_HL2, SMS_REG_I, SMS_REG_R, SMS_REG_IM,
        SMS_REG_IFF1, SMS_REG_IFF2
    };
    long v;
    if (sender.tag >= 0 && sender.tag < NUM_REGS && parseNum(sender.stringValue, &v))
        smsdebug_cpu_set(_dbg, regIds[sender.tag], (int)v);
    [self refreshAll];
}

- (void)flagClicked:(NSButton *)sender
{
    static const int flagIds[NUM_FLAGS] = { SMS_FLAG_S, SMS_FLAG_Z, SMS_FLAG_H, SMS_FLAG_PV, SMS_FLAG_N, SMS_FLAG_C };
    if (smsdebug_is_stopped(_dbg))
        smsdebug_cpu_set(_dbg, flagIds[sender.tag], sender.state == NSControlStateValueOn);
    [self refreshCpu];
}

- (void)writeRam:(id)sender
{
    (void)sender;
    long v;
    const int a = [self resolveAddr:_ramAddr.stringValue];
    if (a >= 0 && parseNum(_ramVal.stringValue, &v))
        smsdebug_write(_dbg, (uint16_t)a, (uint8_t)v);
    [self refreshAll];
}

/* Bring a RAM address's line to the top of the dump ($E000-$FFFF mirror
 * the 8K too). */
- (void)gotoRam:(id)sender
{
    (void)sender;
    const int a = [self resolveAddr:_ramGoto.stringValue];
    if (a < 0xC000) {
        _status.stringValue = [NSString stringWithFormat:@"Not a console RAM address: %@", _ramGoto.stringValue];
        return;
    }
    const NSUInteger line = (NSUInteger)(((a - 0xC000) & 0x1FFF) / 16);
    const NSUInteger at = [RAM_HEADER length] + line * RAM_LINE_LEN;
    if (at >= _ram.string.length) return;
    /* to the end first, so scrolling back up leaves the line at the top */
    [_ram scrollRangeToVisible:NSMakeRange(_ram.string.length, 0)];
    [_ram scrollRangeToVisible:NSMakeRange(at, RAM_LINE_LEN - 1)];
    [_ram setSelectedRange:NSMakeRange(at, 6)];
}

- (void)writeCram:(NSTextField *)sender
{
    long v;
    if (sender.tag >= 0 && sender.tag < CRAM_ENTRIES && parseNum(sender.stringValue, &v))
        smsdebug_cram_write(_dbg, (int)sender.tag, (uint8_t)v);
    [_window makeFirstResponder:nil];
    [self refreshVdp];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    const int addr = [self resolveAddr:_jump.stringValue];
    if (addr < 0) return;
    _followPc.state = NSControlStateValueOff;
    _disasmAddr = (uint16_t)addr;
    [self refreshDisasm];
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[7] = {
        SMSDEBUG_BP_EXEC, SMSDEBUG_BP_READ, SMSDEBUG_BP_WRITE, SMSDEBUG_BP_READ | SMSDEBUG_BP_WRITE,
        SMSDEBUG_BP_IN, SMSDEBUG_BP_OUT, SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT
    };
    NSString *range = [_bpRange.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (range.length == 0) return;
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 7 ? sel : 0];
    const BOOL port = (type & (SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT)) != 0;
    NSArray<NSString *> *ends = [range componentsSeparatedByString:@"-"];
    const int start = [self resolveAddr:ends[0]];
    const int end = ends.count > 1 ? [self resolveAddr:ends[1]] : start;
    if (start < 0 || end < 0 || end < start || (port && end > 0xFF)) {
        NSString *what = port ? @"port" : @"address";
        [self appendPrompt:[NSString stringWithFormat:@"breakpoint: bad %@ %@\n", what, range]];
        _status.stringValue = [NSString stringWithFormat:@"Bad %@: %@", what, range];
        return;
    }
    const int bpId = smsdebug_breakpoint_add(_dbg, type, (uint16_t)start, (uint16_t)end,
                                             _bpCond.stringValue.UTF8String);
    if (bpId < 0) {
        _status.stringValue = @"The condition does not parse";
        [self appendPrompt:[NSString stringWithFormat:@"breakpoint: the condition does not parse: %@\n",
                            _bpCond.stringValue]];
        return;
    }
    _bpRange.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshAll];
}

- (void)bpEnabled:(NSButton *)sender
{
    smsdebug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshAll];
}

- (void)bpRemove:(NSButton *)sender
{
    smsdebug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshAll];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    smsdebug_breakpoint_clear(_dbg);
    [self refreshAll];
}

/* ---- refreshers ------------------------------------------------------------------ */

- (void)refreshStatus
{
    char reason[160];
    int addr;
    const BOOL stopped = smsdebug_is_stopped(_dbg) != 0;
    smsdebug_stop_reason(_dbg, reason, sizeof reason, &addr);
    _status.stringValue = stopped ? [NSString stringWithFormat:@"Stopped%s%s", reason[0] ? ": " : "", reason] : @"Running";
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    _runBtn.bezelColor = stopped ? SMSAccentColor() : nil;
}

- (void)refreshCpu
{
    smsdebug_cpu c;
    smsdebug_cpu_get(_dbg, &c);
    const int vals[NUM_REGS] = {
        c.pc, c.sp, c.af, c.bc, c.de, c.hl, c.ix, c.iy, c.af2, c.bc2, c.de2, c.hl2,
        c.i, c.r, c.im, c.iff1, c.iff2
    };
    for (int i = 0; i < NUM_REGS; i++)
        if (!_reg[i].currentEditor)
            _reg[i].stringValue = [NSString stringWithFormat:(i < 12 ? @"%04X" : (i < 14 ? @"%02X" : @"%d")), vals[i]];
    const int flags[NUM_FLAGS] = { c.sf, c.zf, c.hf, c.pf, c.nf, c.cf };
    for (int i = 0; i < NUM_FLAGS; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
    _beam.stringValue = [NSString stringWithFormat:
        @"vpos %d   hpos %d   V counter $%02X   H counter $%02X   frame %u   cycles %llu   WZ $%04X%s%s%s",
        c.vpos, c.hpos, c.vcount & 0xff, c.hcount & 0xff, c.frame, (unsigned long long)c.cycles, c.wz & 0xffff,
        c.int_line ? "   INT" : "", c.nmi_line ? "   NMI" : "", c.halted ? "   HALT" : ""];
}

- (void)refreshRam
{
    static uint8_t ram[8192];
    smsdebug_ram_get(_dbg, ram);
    NSMutableString *text = [NSMutableString stringWithString:RAM_HEADER];
    for (int row = 0; row < 8192 / 16; row++) {
        [text appendFormat:@"$%04X: ", 0xC000 + row * 16];
        for (int col = 0; col < 16; col++) [text appendFormat:@"%02X ", ram[row * 16 + col]];
        [text appendString:@"\n"];
    }
    /* keep the reader's place */
    setTextKeepingPlace(_ram, text);
}

- (void)refreshDisasm
{
    static smsdebug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        smsdebug_cpu c;
        smsdebug_cpu_get(_dbg, &c);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)smsdebug_row_address(_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    const int n = smsdebug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    _lineCount = n;
    NSMutableString *text = [NSMutableString string];
    NSUInteger pcStart = 0, pcLen = 0;
    BOOL havePc = NO;
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        NSString *line = [NSString stringWithFormat:@"%c%c %04X  %-11s  %-16s %-24s%s%s\n",
                          lines[i].has_breakpoint ? '*' : ' ', lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm,
                          lines[i].comment[0] ? " ; " : "", lines[i].comment];
        if (lines[i].is_pc) { havePc = YES; pcStart = text.length; pcLen = line.length - 1; }
        [text appendString:line];
    }
    if (n == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];
    _disasm.string = text;
    if (havePc) {
        [_disasm.textStorage addAttributes:@{ NSBackgroundColorAttributeName: SMSAccentColor(),
                                              NSForegroundColorAttributeName: NSColor.whiteColor }
                                     range:NSMakeRange(pcStart, pcLen)];
    }
}

- (void)refreshVdp
{
    smsdebug_vdp v;
    smsdebug_vdp_get(_dbg, &v);
    const BOOL mode4 = v.mode == 4;
    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"%s   %s   %d lines   %s\n\n", v.kind_5246 ? "315-5246" : "315-5124",
        v.mode_name ? v.mode_name : "?", v.y_pixels, v.is_pal ? "PAL" : "NTSC"];
    for (int r = 0; r < 11; r++) {
        char desc[160];
        smsdebug_vdp_describe_register(_dbg, r, desc, sizeof desc);
        [s appendFormat:@"R%-2d  $%02X  %@\n", r, v.reg[r], stripControl(desc)];
    }
    [s appendFormat:@"\nStatus     $%02X   frame int %d  overflow %d  collision %d\n",
        v.status, (v.status >> 7) & 1, (v.status >> 6) & 1, (v.status >> 5) & 1];
    [s appendFormat:@"Address    $%04X   code %d   read buffer $%02X%s\n",
        v.addr, v.code, v.buffer, v.second_byte ? "   (control write half done)" : ""];
    [s appendFormat:@"Line ctr   %d   H counter $%02X\n", v.line_counter, v.hcounter];
    [s appendFormat:@"Display    %s   VINT %s   HINT %s   sprites 8x%d%s\n",
        v.display_on ? "on" : "off", v.vint_on ? "on" : "off", v.hint_on ? "on" : "off",
        v.sprites_16 ? 16 : 8, v.sprites_zoom ? " zoomed" : ""];
    if (mode4) {
        [s appendFormat:@"Tables     name $%04X   SAT $%04X   sprite patterns $%04X\n",
            v.name_base, v.sat_base, v.sprite_pattern_base];
        [s appendFormat:@"Scroll     X %d   Y %d%s%s%s%s\n", v.scroll_x, v.scroll_y,
            v.left_column_blank ? "   left column blank" : "", v.hscroll_lock_top ? "   top rows locked" : "",
            v.vscroll_lock_right ? "   right columns locked" : "", v.sprite_shift ? "   sprites shifted left 8" : ""];
    } else {
        [s appendFormat:@"Tables     name $%04X   colour $%04X   pattern $%04X   SAT $%04X   sprite patterns $%04X\n",
            v.name_base, v.color_base, v.pattern_base, v.sat_base, v.sprite_pattern_base];
    }
    [s appendFormat:@"Backdrop   %d\n", v.backdrop];
    [s appendFormat:@"Pending    VINT %d   HINT %d   INT line %d   NMI line %d%s\n",
        v.vint_pending, v.hint_pending, v.int_line, v.nmi_line, v.pause_held ? "   Pause held" : ""];
    [s appendFormat:@"Beam       vpos %d   hpos %d   V counter $%02X   frame %u\n\n", v.vpos, v.hpos, v.vcount & 0xff, v.frame];

    smsdebug_sprite spr[64];
    const int n = smsdebug_sprites_get(_dbg, spr);
    [s appendString:mode4 ? @"Sprites  #    Y    X  tile  visible\n" : @"Sprites  #    Y    X  tile  colour  EC  visible\n"];
    for (int i = 0; i < n; i++) {
        if (mode4)
            [s appendFormat:@"        %2d  %3d  %3d  $%03X  %s\n", i, spr[i].y, spr[i].x, spr[i].tile,
                spr[i].visible ? "yes" : "-"];
        else
            [s appendFormat:@"        %2d  %3d  %3d  $%02X   %2d     %d   %s\n", i, spr[i].y, spr[i].x, spr[i].tile,
                spr[i].color, spr[i].early_clock, spr[i].visible ? "yes" : "-"];
    }
    setTextKeepingPlace(_vdpText, s);

    for (int i = 0; i < CRAM_ENTRIES; i++) {
        _cramSwatch[i].color = colorFromXrgb(v.cram_rgb[i]);
        _cramSwatch[i].toolTip = [NSString stringWithFormat:@"%s %d: $%02X", i < 16 ? "background" : "sprite",
                                  i & 15, v.cram[i]];
        if (!_cramVal[i].currentEditor)
            _cramVal[i].stringValue = [NSString stringWithFormat:@"%02X", v.cram[i]];
    }

    /* the palette choice applies to the tiles only */
    const NSInteger view = _vdpViewSel.indexOfSelectedItem;
    _vdpPalette.enabled = view == SMSDEBUG_VIEW_TILES;
    int w = 0, h = 0;
    if (smsdebug_vdp_view(_dbg, (int)view, (int)_vdpPalette.indexOfSelectedItem, _viewPx, &w, &h) && w > 0 && h > 0) {
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)_viewPx, (CFIndex)w * h * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
                                       kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
                                       NULL, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CFRelease(data);
        CGColorSpaceRelease(space);
        _vdpPic.image = img;   /* the view owns it now */
    }
}

- (void)refreshSound
{
    smsdebug_io io;
    smsdebug_io_get(_dbg, &io);
    NSMutableString *s = [NSMutableString string];
    [s appendString:@"PSG (SN76489; volume 0 is loudest, 15 off)\n"];
    for (int ch = 0; ch < 3; ch++)
        [s appendFormat:@"  Tone %d   period %4d   volume %2d\n", ch, io.tone_period[ch], io.tone_volume[ch]];
    [s appendFormat:@"  Noise    %s   rate %d   volume %2d   LFSR $%04X\n",
        io.noise_mode ? "white   " : "periodic", io.noise_rate, io.noise_volume, io.lfsr];
    [s appendFormat:@"  Audible  PSG %s   FM %s\n\n", io.psg_audible ? "yes" : "no", io.fm_audible ? "yes" : "no"];

    if (io.fm_present) {
        [s appendString:@"YM2413 (FM) registers\n       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n"];
        for (int row = 0; row < 4; row++) {
            [s appendFormat:@"  $%X0: ", row];
            for (int col = 0; col < 16; col++) [s appendFormat:@"%02X ", io.fm_regs[row * 16 + col]];
            [s appendString:@"\n"];
        }
        [s appendString:@"\n"];
    } else {
        [s appendString:@"YM2413 (FM) not present on this console\n\n"];
    }

    [s appendFormat:@"Memory control ($3E)  $%02X   cartridge %s   BIOS %s   work RAM %s   I/O chip %s\n",
        io.mem_ctrl, io.cart_enabled ? "on" : "off", io.bios_enabled ? "on" : "off",
        io.ram_enabled ? "on" : "off", io.io_enabled ? "on" : "off"];
    [s appendFormat:@"I/O control ($3F)     $%02X\n", io.io_ctrl];
    if (io.bios_present)
        [s appendFormat:@"BIOS                  present   pages $%02X $%02X $%02X\n",
            io.bios_page[0], io.bios_page[1], io.bios_page[2]];
    else
        [s appendString:@"BIOS                  none (the cartridge boots directly)\n"];
    [s appendFormat:@"Mapper ($FFFC-$FFFF)  $%02X $%02X $%02X $%02X\n",
        io.mapper[0], io.mapper[1], io.mapper[2], io.mapper[3]];
    [s appendFormat:@"Ports                 $DC = $%02X   $DD = $%02X\n\n", io.port_dc, io.port_dd];

    /* the pad bits are in sms_action order */
    static const char *const names[SMS_ACT_PER_PORT] = { "Up", "Down", "Left", "Right", "Button 1", "Button 2" };
    for (int port = 0; port < 2; port++) {
        [s appendFormat:@"Player %d   $%02X ", port + 1, io.pad[port]];
        BOOL any = NO;
        for (int b = 0; b < SMS_ACT_PER_PORT; b++)
            if (io.pad[port] & (1u << b)) {
                [s appendFormat:@" %s", names[b]];
                any = YES;
            }
        if (!any) [s appendString:@" (nothing held)"];
        [s appendString:@"\n"];
    }
    [s appendFormat:@"Console    Pause %s   Reset %s\n", io.pause_held ? "held" : "up", io.reset_held ? "held" : "up"];
    [s appendFormat:@"Region     %s\n", io.japanese ? "Japanese" : "export"];
    [s appendFormat:@"Console    %s\n", io.console_name ? io.console_name : "?"];
    setTextKeepingPlace(_sound, s);
}

- (void)refreshBps
{
    static smsdebug_breakpoint bps[MAX_BPS];
    const int n = smsdebug_breakpoint_list(_dbg, bps, MAX_BPS);
    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:[self label:@"No breakpoints"]];
    for (int i = 0; i < n; i++) {
        static const struct { int bit; const char *name; } kinds[5] = {
            { SMSDEBUG_BP_EXEC, "exec" }, { SMSDEBUG_BP_READ, "read" }, { SMSDEBUG_BP_WRITE, "write" },
            { SMSDEBUG_BP_IN, "in" }, { SMSDEBUG_BP_OUT, "out" },
        };
        NSMutableString *what = [NSMutableString stringWithFormat:@"#%-3d ", bps[i].id];
        BOOL first = YES;
        for (int k = 0; k < 5; k++)
            if (bps[i].type & kinds[k].bit) {
                [what appendFormat:@"%s%s", first ? "" : "/", kinds[k].name];
                first = NO;
            }
        const BOOL port = (bps[i].type & (SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT)) != 0;
        [what appendFormat:(port ? @"  port $%02X" : @"  $%04X"), bps[i].start];
        if (bps[i].end != bps[i].start) [what appendFormat:(port ? @"-$%02X" : @"-$%04X"), bps[i].end];
        char label[64];
        if (!port && smsdebug_address_label(_dbg, bps[i].start, label, sizeof label) > 0)
            [what appendFormat:@"  %s", label];
        if (bps[i].condition[0]) [what appendFormat:@"  if %s", bps[i].condition];
        [what appendFormat:@"   hits %u", bps[i].hits];
        NSButton *box = [NSButton checkboxWithTitle:what target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[i].id;
        [_bpList addArrangedSubview:hstack(@[box, remove])];
    }
}

- (void)refreshCart
{
    smsdebug_cart c;
    char info[256];
    smsdebug_cart_get(_dbg, &c);
    smsdebug_cart_info(_dbg, info, sizeof info);
    NSMutableString *s = [NSMutableString stringWithFormat:@"%@\n\n", stripControl(info)];
    if (!c.present) {
        [s appendString:@"(no cartridge running)\n"];
        setTextKeepingPlace(_cart, s);
        return;
    }
    [s appendFormat:@"Mode        %s (%d)%s%s\n", c.mode_name ? c.mode_name : "?", c.mode,
        c.direct ? "   opened from a file" : "", c.booted_game ? "   a game has booted" : ""];
    [s appendFormat:@"Link        %s%s\n", c.link_up ? "up" : "down", c.busy ? " (busy)" : ""];
    if (c.mapper >= 0)
        [s appendFormat:@"Mapper      %s (%d)   banks $%02X $%02X $%02X $%02X $%02X $%02X\n", c.mapper_name, c.mapper,
            c.bank[0], c.bank[1], c.bank[2], c.bank[3], c.bank[4], c.bank[5]];
    else
        [s appendString:@"Mapper      none (CONFIG)\n"];
    [s appendFormat:@"RAM         %s, %s   %u bytes\n", c.ram_enabled ? "enabled" : "disabled",
        c.ram_writable ? "writable" : "read-only", c.ram_size];
    [s appendFormat:@"Image       %u bytes   CRC-32 %08X   claim %d\n", c.image_size, c.image_crc, c.claim];
    [s appendFormat:@"Mailbox     ACKSEQ $%02X   STATUS $%02X   last error %u   reply cmd $%02X   RXLEN %u\n",
        c.ackseq, c.status, c.last_error, c.reply_cmd, c.rxlen];
    [s appendFormat:@"Boot        state %d   %d%%   error %d   %u of %u bytes\n",
        c.boot_state, c.boot_pct, c.boot_err, c.boot_got, c.boot_total];
    [s appendFormat:@"Load        state %d   window %d of %d   %d%%\n", c.load_state, c.load_win, c.load_nwin, c.load_pct];
    [s appendFormat:@"BIOS snoop  phase %d   $C000 = $%02X   $3E = $%02X   $3F = $%02X\n",
        c.bios_phase, c.snoop_c000, c.snoop_3e, c.snoop_3f];
    [s appendString:@"            VDP"];
    for (int r = 0; r < 11; r++) [s appendFormat:@" R%d=$%02X", r, c.snoop_vdp[r]];
    [s appendFormat:@"\nQueue       %u\n\n", c.queue_depth];

    /* which 8K SRAM bank backs each 1K page of $0000-$BFFF */
    [s appendString:@"Page map (1K page -> SRAM bank; -- = cartridge memory)\n"];
    for (int row = 0; row < 3; row++) {
        [s appendFormat:@"  $%04X:", row * 0x4000];
        for (int col = 0; col < 16; col++) {
            const int bank = c.page_bank[row * 16 + col];
            if (bank < 0) [s appendString:@"  --"];
            else [s appendFormat:@" %3d", bank];
        }
        [s appendString:@"\n"];
    }
    setTextKeepingPlace(_cart, s);
}

- (void)refreshAll
{
    [self refreshStatus];
    [self refreshCpu];
    [self refreshRam];
    [self refreshDisasm];
    [self refreshVdp];
    [self refreshSound];
    [self refreshBps];
    [self refreshCart];
}
@end
