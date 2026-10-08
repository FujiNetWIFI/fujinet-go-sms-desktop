/*
 * SMSControllersWindow -- the AppKit controllers panel: a Master System
 * joypad per player (the D-pad on the left of the pad, buttons 1 and 2 on
 * the right), the console's Pause and Reset buttons and the session's
 * actions, the gamepads that are connected and which player each drives,
 * and the Map row.
 *
 * Buttons are a PadButton subclass that reports mouseDown and mouseUp
 * rather than an action on click: a controller button is HELD, and the
 * console reads its buttons once a frame, so a value present only for the
 * instant of a click falls between frames. mouseUp arrives even when the
 * pointer has left the button (AppKit tracks the drag for the view that got
 * mouseDown), so dragging off cannot strand the machine with a button held.
 *
 * Whatever holds a button -- the keyboard, a gamepad or a click here -- the
 * button lights up in the accent colour, from smssession_buttons_held and
 * smssession_switch_held.
 *
 * A utility panel of fixed size (no resize mask). Singleton, ordered out
 * rather than closed, so its position survives.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"

#include <string.h>

/* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
 * pad button. */
static int g_mapState = -2;
static SMSControllersWindow *g_singleton;
static smssession *g_session;

#define MAX_PAD_ROWS 4

/* NOT named `target`: NSControl already has a `target` property. */
@interface PadButton : NSButton
@property (nonatomic) int padTarget;
@property (nonatomic, copy) NSString *face;
@property (nonatomic) BOOL down;
@property (nonatomic) BOOL lit;
- (void)setLitState:(BOOL)lit;
@end

@implementation PadButton
- (void)setLitState:(BOOL)lit
{
    if (lit == self.lit) return;
    self.lit = lit;
    self.bezelColor = lit ? SMSAccentColor() : nil;
}

- (void)mouseDown:(NSEvent *)e
{
    (void)e;
    if (g_mapState == -1) {
        g_mapState = self.padTarget;
        [[NSNotificationCenter defaultCenter] postNotificationName:@"SMSPadMapTarget" object:nil];
        return;
    }
    if (g_mapState >= 0) return;
    self.down = YES;
    [self setLitState:YES];
    if (self.padTarget >= SMS_TARGET_SYSACT(0)) return;   /* fires on release */
    /* the joypads and the console's buttons are held while the mouse is */
    smssession_press(g_session, self.padTarget, 1);
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    if (!self.down) return;
    self.down = NO;
    [self setLitState:NO];
    if (g_mapState != -2) return;
    if (self.padTarget >= SMS_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not power-cycle the machine. Posted, so the
         * application runs them exactly as it runs a gamepad's. */
        smssession_sysaction_post(g_session, self.padTarget - SMS_TARGET_SYSACT(0));
        return;
    }
    smssession_press(g_session, self.padTarget, 0);
}
@end

/* Buttons 1 and 2: drawn as the pad's round buttons rather than with a
 * bezel, and lit by filling them with the accent colour. */
@interface RoundPadButton : PadButton
@end

@implementation RoundPadButton
- (void)setLitState:(BOOL)lit
{
    [super setLitState:lit];
    [self setNeedsDisplay:YES];
}

- (void)setTitle:(NSString *)title
{
    [super setTitle:title];
    [self setNeedsDisplay:YES];
}

/* It draws itself (drawRect: below), which a layer-updating button would
 * skip. */
- (BOOL)wantsUpdateLayer
{
    return NO;
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect r = NSInsetRect([self bounds], 2, 2);
    NSBezierPath *path = [NSBezierPath bezierPathWithOvalInRect:r];
    [(self.lit ? SMSAccentColor() : [NSColor colorWithWhite:0.30 alpha:1.0]) setFill];
    [path fill];
    [path setLineWidth:1.5];
    [[NSColor colorWithWhite:0.55 alpha:1.0] setStroke];
    [path stroke];

    /* The face ("1", "2") large; a binding in Map mode small and wrapped. */
    NSString *text = self.title ?: @"";
    const BOOL face = [text isEqualToString:self.face];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByWordWrapping;
    NSDictionary *attrs = @{ NSFontAttributeName: face ? [NSFont boldSystemFontOfSize:20] : [NSFont systemFontOfSize:10],
                             NSForegroundColorAttributeName: NSColor.whiteColor,
                             NSParagraphStyleAttributeName: para };
    const NSRect box = NSInsetRect(r, 8, 0);
    const NSRect used = [text boundingRectWithSize:box.size options:NSStringDrawingUsesLineFragmentOrigin
                                        attributes:attrs context:nil];
    [text drawInRect:NSMakeRect(box.origin.x, NSMidY(r) - used.size.height / 2, box.size.width, used.size.height)
      withAttributes:attrs];
}
@end

/* The joypad's body: a dark rounded rectangle the buttons sit on. */
@interface PadBodyView : NSView
@end

@implementation PadBodyView
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect([self bounds], 1, 1)
                                                         xRadius:10 yRadius:10];
    [[NSColor colorWithWhite:0.16 alpha:1.0] setFill];
    [path fill];
    [[NSColor colorWithWhite:0.40 alpha:1.0] setStroke];
    [path stroke];
}
@end

/* The console row: the console's own buttons, then the session's actions. */
#define CONSOLE_BUTTONS 5
#define CONSOLE_RESET   1    /* index of the Reset button, which not every console has */
#define CONSOLE_COLS    3

@implementation SMSControllersWindow {
    NSMutableArray<PadButton *> *_buttons;
    PadButton *_console[CONSOLE_BUTTONS];
    CGFloat _consoleTop;
    int _consoleModel;          /* the console the row was laid out for */
    NSButton *_mapButton;
    NSTextField *_hint;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    NSTextField *_noPads;
    unsigned _padGeneration;
    NSTimer *_captureTimer;
    NSTimer *_liveTimer;
}

#define KEY_W    60.0
#define KEY_H    34.0
#define GAP       6.0
#define ROUND    64.0      /* buttons 1 and 2 */
#define BTN_GAP  20.0
#define GROUP    96.0      /* between the D-pad and the buttons */
#define BODY_PAD 14.0
#define DPAD_W (3 * KEY_W + 2 * GAP)
#define DPAD_H (3 * KEY_H + 2 * GAP)
#define BTN_W  (2 * ROUND + BTN_GAP)
#define PAD_W  (BODY_PAD + DPAD_W + GROUP + BTN_W + BODY_PAD)
#define PAD_H  (BODY_PAD + DPAD_H + BODY_PAD)
#define MARGIN  12.0
#define ROW_H   26.0
#define CONSOLE_H (2 * KEY_H + GAP)

- (PadButton *)button:(PadButton *)b face:(NSString *)face target:(int)target
{
    [b setTitle:face];
    b.padTarget = target;
    b.face = face;
    /* Not focusable: clicking a pad button must not steal the key window's
     * first responder. */
    [b setRefusesFirstResponder:YES];
    [_buttons addObject:b];
    return b;
}

- (PadButton *)buttonWithFace:(NSString *)face target:(int)target frame:(NSRect)frame
{
    PadButton *b = [self button:[[PadButton alloc] initWithFrame:frame] face:face target:target];
    [b setBezelStyle:NSBezelStyleRounded];
    return b;
}

- (PadButton *)roundButtonWithFace:(NSString *)face target:(int)target frame:(NSRect)frame
{
    PadButton *b = [self button:[[RoundPadButton alloc] initWithFrame:frame] face:face target:target];
    [b setBordered:NO];
    return b;
}

/* The pad's cell (col, row) inside a block whose top-left is (x, top);
 * AppKit's y grows upwards, so rows are placed by their bottom edge. */
static NSRect cell(CGFloat x, CGFloat top, int col, int row, CGFloat w)
{
    return NSMakeRect(x + col * (w + GAP), top - (row + 1) * KEY_H - row * GAP, w, KEY_H);
}

/* One joypad, laid out downwards from topY. Returns the y below it. */
- (CGFloat)buildController:(int)port intoView:(NSView *)parent topY:(CGFloat)topY
{
    CGFloat y = topY;
    const CGFloat x0 = MARGIN;

    NSTextField *title = [NSTextField labelWithString:(port ? @"Player 2" : @"Player 1")];
    [title setFont:[NSFont boldSystemFontOfSize:12]];
    [title setFrame:NSMakeRect(x0, y - 20, 120, 16)];
    [parent addSubview:title];
    y -= ROW_H + GAP;

    PadBodyView *body = [[PadBodyView alloc] initWithFrame:NSMakeRect(x0, y - PAD_H, PAD_W, PAD_H)];
    [parent addSubview:body];

    /* the D-pad, in the body's own coordinates */
    const CGFloat top = PAD_H - BODY_PAD;
    [body addSubview:[self buttonWithFace:@"▲" target:SMS_TARGET_PORT(port, SMS_ACT_UP)
                                    frame:cell(BODY_PAD, top, 1, 0, KEY_W)]];
    [body addSubview:[self buttonWithFace:@"◀" target:SMS_TARGET_PORT(port, SMS_ACT_LEFT)
                                    frame:cell(BODY_PAD, top, 0, 1, KEY_W)]];
    [body addSubview:[self buttonWithFace:@"▶" target:SMS_TARGET_PORT(port, SMS_ACT_RIGHT)
                                    frame:cell(BODY_PAD, top, 2, 1, KEY_W)]];
    [body addSubview:[self buttonWithFace:@"▼" target:SMS_TARGET_PORT(port, SMS_ACT_DOWN)
                                    frame:cell(BODY_PAD, top, 1, 2, KEY_W)]];

    /* buttons 1 and 2 side by side on the right, as on the pad */
    const CGFloat bx = BODY_PAD + DPAD_W + GROUP;
    const CGFloat by = (PAD_H - ROUND) / 2;
    [body addSubview:[self roundButtonWithFace:@"1" target:SMS_TARGET_PORT(port, SMS_ACT_1)
                                         frame:NSMakeRect(bx, by, ROUND, ROUND)]];
    [body addSubview:[self roundButtonWithFace:@"2" target:SMS_TARGET_PORT(port, SMS_ACT_2)
                                         frame:NSMakeRect(bx + ROUND + BTN_GAP, by, ROUND, ROUND)]];
    return y - PAD_H;
}

- (instancetype)init
{
    const CGFloat width = MARGIN + PAD_W + MARGIN;
    const CGFloat height = MARGIN
        + 2 * (ROW_H + GAP + PAD_H + MARGIN)      /* the two joypads */
        + 18 + CONSOLE_H + MARGIN                 /* console row */
        + 18 + MAX_PAD_ROWS * ROW_H + MARGIN      /* gamepads */
        + 30 + MARGIN;                            /* map row */

    NSPanel *win = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, width, height)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setTitle:@"Controllers"];
    /* Floats above the machine's window and stays out of the way. */
    [win setLevel:NSFloatingWindowLevel];
    [win setReleasedWhenClosed:NO];
    [win setBecomesKeyOnlyIfNeeded:NO];

    self = [super initWithWindow:win];
    if (!self) return nil;
    [win setDelegate:self];

    _buttons = [NSMutableArray array];
    NSView *content = [win contentView];
    CGFloat y = height - MARGIN;
    y = [self buildController:0 intoView:content topY:y] - MARGIN;
    y = [self buildController:1 intoView:content topY:y] - MARGIN;

    /* The console's Pause and Reset buttons, the /RESET line, and the
     * session's actions. Laid out by layoutConsole, which leaves Reset out
     * on the consoles that have none. */
    NSTextField *consoleLabel = [NSTextField labelWithString:@"Console"];
    [consoleLabel setFont:[NSFont boldSystemFontOfSize:12]];
    [consoleLabel setFrame:NSMakeRect(MARGIN, y - 16, 120, 16)];
    [content addSubview:consoleLabel];
    y -= 18;
    static const struct { NSString *__unsafe_unretained face; int target; } console[CONSOLE_BUTTONS] = {
        { @"Pause",           SMS_TARGET_SWITCH(SMS_SW_PAUSE) },
        { @"Reset",           SMS_TARGET_SWITCH(SMS_SW_RESET) },
        { @"Soft Reset",      SMS_TARGET_SYSACT(SMS_SYSACT_SOFT_RESET) },
        { @"Reset to CONFIG", SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG) },
        { @"Debugger Stop",   SMS_TARGET_SYSACT(SMS_SYSACT_DEBUG_STOP) },
    };
    for (int i = 0; i < CONSOLE_BUTTONS; i++) {
        _console[i] = [self buttonWithFace:console[i].face target:console[i].target frame:NSMakeRect(0, 0, 10, KEY_H)];
        [content addSubview:_console[i]];
    }
    _consoleTop = y;
    _consoleModel = -1;
    [self layoutConsole];
    y -= CONSOLE_H + MARGIN;

    /* The gamepads: which player each one drives. */
    NSTextField *pads = [NSTextField labelWithString:@"Gamepads"];
    [pads setFont:[NSFont boldSystemFontOfSize:12]];
    [pads setFrame:NSMakeRect(MARGIN, y - 16, 120, 16)];
    [content addSubview:pads];
    y -= 18;
    _noPads = [NSTextField labelWithString:@"No gamepads connected"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_noPads setFrame:NSMakeRect(MARGIN, y - 20, PAD_W, 18)];
    [content addSubview:_noPads];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const CGFloat ry = y - (i + 1) * ROW_H;
        _padName[i] = [NSTextField labelWithString:@""];
        [_padName[i] setFrame:NSMakeRect(MARGIN, ry + 4, PAD_W - 170, 18)];
        [_padName[i] setLineBreakMode:NSLineBreakByTruncatingTail];
        [content addSubview:_padName[i]];
        _padAssign[i] = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(MARGIN + PAD_W - 160, ry, 160, 24)];
        [_padAssign[i] addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2"]];
        _padAssign[i].tag = i;
        _padAssign[i].target = self;
        _padAssign[i].action = @selector(padAssignChanged:);
        [_padAssign[i] setRefusesFirstResponder:YES];
        [content addSubview:_padAssign[i]];
    }
    y -= MAX_PAD_ROWS * ROW_H + MARGIN;

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setFrame:NSMakeRect(MARGIN, y - 30, 70, 30)];
    [_mapButton setRefusesFirstResponder:YES];
    [content addSubview:_mapButton];

    NSButton *defaults = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [defaults setFrame:NSMakeRect(MARGIN + 76, y - 30, 90, 30)];
    [defaults setRefusesFirstResponder:YES];
    [content addSubview:defaults];

    _hint = [NSTextField labelWithString:@""];
    [_hint setFrame:NSMakeRect(MARGIN + 176, y - 24, width - MARGIN - 190, 18)];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setLineBreakMode:NSLineBreakByTruncatingTail];
    [content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(refresh)
                                                 name:@"SMSPadMapTarget" object:nil];
    _padGeneration = (unsigned)-1;
    [self refresh];
    [self refreshPads];
    return self;
}

/* ---- the console row ----------------------------------------------------------- */

/* Reset exists only on the Master System, and as Rapid on the Japanese one;
 * the Master System II and the Mark III have none. The buttons that remain
 * flow three to a row. */
- (void)layoutConsole
{
    const int model = smssession_console(g_session);
    if (model == _consoleModel) return;
    _consoleModel = model;

    PadButton *reset = _console[CONSOLE_RESET];
    reset.face = model == SMS_CONSOLE_SMSJ ? @"Rapid" : @"Reset";
    reset.hidden = !sms_console_has_reset_button(model);
    if (reset.hidden && reset.down) {
        reset.down = NO;
        smssession_press(g_session, reset.padTarget, 0);
    }

    const CGFloat cw = (PAD_W - (CONSOLE_COLS - 1) * GAP) / CONSOLE_COLS;
    int slot = 0;
    for (int i = 0; i < CONSOLE_BUTTONS; i++) {
        if (_console[i].hidden) continue;
        const int col = slot % CONSOLE_COLS, row = slot / CONSOLE_COLS;
        [_console[i] setFrame:NSMakeRect(MARGIN + col * (cw + GAP), _consoleTop - (row + 1) * KEY_H - row * GAP,
                                         cw, KEY_H)];
        slot++;
    }
    [self refresh];
}

/* ---- the controls ------------------------------------------------------------ */

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    smssession_gamepad_assign(g_session, (int)sender.tag, (int)sender.indexOfSelectedItem - 1);
    [self refreshPads];
}

- (void)refreshPads
{
    _padGeneration = smssession_gamepad_generation(g_session);
    const int n = smssession_gamepad_count(g_session);
    _noPads.hidden = n > 0;
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const BOOL shown = i < n;
        _padName[i].hidden = !shown;
        _padAssign[i].hidden = !shown;
        if (!shown) continue;
        char name[128];
        smssession_gamepad_name(g_session, i, name, sizeof name);
        const int eff = smssession_gamepad_effective_port(g_session, i);
        _padName[i].stringValue = eff >= 0
            ? [NSString stringWithFormat:@"%s — player %d", name, eff + 1]
            : [NSString stringWithFormat:@"%s — unused", name];
        [_padAssign[i] selectItemAtIndex:smssession_gamepad_assignment(g_session, i) + 1];
    }
}

/* The live part, twenty times a second while the panel is up: what is held,
 * whether a gamepad came or went, and which console is running. */
- (void)tick
{
    if (smssession_gamepad_generation(g_session) != _padGeneration) [self refreshPads];
    [self layoutConsole];
    if (g_mapState != -2) return;
    const unsigned held[2] = { smssession_buttons_held(g_session, 0), smssession_buttons_held(g_session, 1) };
    for (PadButton *b in _buttons) {
        if (b.down || b.padTarget >= SMS_TARGET_SYSACT(0)) continue;
        if (b.padTarget >= SMS_TARGET_SWITCH(0)) {
            [b setLitState:smssession_switch_held(g_session, b.padTarget - SMS_TARGET_SWITCH(0)) != 0];
            continue;
        }
        const int port = b.padTarget / SMS_ACT_PER_PORT;
        const int act = b.padTarget % SMS_ACT_PER_PORT;
        [b setLitState:(held[port] & (1u << act)) != 0];
    }
}

/* ---- Map mode ---------------------------------------------------------------- */

- (void)toggleMap:(id)sender
{
    (void)sender;
    _hint.stringValue = @"";
    [self setMapState:(g_mapState == -2) ? -1 : -2];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    smssession_bindings_reset(g_session);
    _hint.stringValue = @"Default bindings restored";
    [self refresh];
}

- (void)setMapState:(int)state
{
    g_mapState = state;
    [_captureTimer invalidate];
    _captureTimer = nil;
    if (state >= 0) {
        smssession_gamepad_capture_begin(g_session);
        _captureTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [self pollCapture];
        }];
    } else {
        smssession_gamepad_capture_cancel(g_session);
        if (state == -2) _hint.stringValue = @"";
    }
    [self refresh];
}

- (void)pollCapture
{
    int button;
    if (g_mapState < 0) return;
    if (smssession_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        const int target = g_mapState;
        smssession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
        [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
        _hint.stringValue = stolen[0]
            ? [NSString stringWithFormat:@"%s: %s (taken from %s)", sms_target_name(target),
                                         sms_pad_button_name(button), stolen]
            : [NSString stringWithFormat:@"%s: %s", sms_target_name(target), sms_pad_button_name(button)];
    }
}

/* In Map mode every button shows its binding; otherwise its face. */
- (void)refresh
{
    [_mapButton setTitle:(g_mapState == -2 ? @"Map" : @"Done")];
    _mapButton.bezelColor = g_mapState == -2 ? nil : SMSAccentColor();
    if (g_mapState == -1 && _hint.stringValue.length == 0)
        [_hint setStringValue:@"Click a control to remap"];
    else if (g_mapState >= 0)
        [_hint setStringValue:[NSString stringWithFormat:@"Press a key or gamepad button for %s",
                               sms_target_name(g_mapState)]];

    for (PadButton *b in _buttons) {
        if (g_mapState != -2) {
            const sms_binding bind = smssession_binding_get(g_session, b.padTarget);
            char key[32];
            smssession_keysym_name(bind.keysym, key, sizeof key);
            NSString *text = key[0] ? [NSString stringWithUTF8String:key] : @"—";
            if (bind.button != SMS_PAD_BTN_NONE)
                text = [text stringByAppendingFormat:@" / %s", sms_pad_button_name(bind.button)];
            [b setTitle:text];
            [b setToolTip:[NSString stringWithUTF8String:sms_target_name(b.padTarget)]];
            [b setLitState:(b.padTarget == g_mapState)];
        } else {
            [b setTitle:b.face];
            [b setToolTip:nil];
            if (!b.down) [b setLitState:NO];
        }
    }
}

/* ---- the keyboard ------------------------------------------------------------- */

/* Keyboard here behaves exactly as in the main window, so typing drives the
 * machine whichever window is key -- except in Map mode, where the next key
 * pressed is the binding. */
- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    const uint32_t ks = SMSKeysymFromEvent(e);
    if (g_mapState >= 0) {
        if (ks) {
            char stolen[128], name[32];
            const int target = g_mapState;
            smssession_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
            smssession_keysym_name(ks, name, sizeof name);
            [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
            _hint.stringValue = stolen[0]
                ? [NSString stringWithFormat:@"%s: %s (taken from %s)", sms_target_name(target), name, stolen]
                : [NSString stringWithFormat:@"%s: %s", sms_target_name(target), name];
        }
        return;
    }
    if (g_mapState == -1 || !ks) return;
    /* Cmd shortcuts are the menus' (and AppKit sends no keyUp under Cmd). */
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    if (ks == SMS_KEYSYM_F9) { [SMSControllersWindow toggleWithSession:g_session]; return; }

    const int sa = smssession_key_sysaction(g_session, ks);
    if (sa >= 0) { smssession_sysaction_post(g_session, sa); return; }
    smssession_key(g_session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    if (g_mapState != -2) return;
    const uint32_t ks = SMSKeysymFromEvent(e);
    if (ks) smssession_key(g_session, ks, 0);
}

/* Modifiers bind too (Shift, Control and Option are keys like any other). */
- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = SMSKeysymFromFlagsChange(e, &down);
    if (!ks) return;
    if (g_mapState >= 0) {
        if (!down) return;
        char stolen[128], name[32];
        const int target = g_mapState;
        smssession_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
        smssession_keysym_name(ks, name, sizeof name);
        [self setMapState:-1];
        _hint.stringValue = stolen[0]
            ? [NSString stringWithFormat:@"%s: %s (taken from %s)", sms_target_name(target), name, stolen]
            : [NSString stringWithFormat:@"%s: %s", sms_target_name(target), name];
        return;
    }
    if (g_mapState == -2) smssession_key(g_session, ks, down);
}

/* ---- lifetime ------------------------------------------------------------------ */

- (void)hidePanel
{
    [self setMapState:-2];
    [_liveTimer invalidate];
    _liveTimer = nil;
    smssession_release_all(g_session);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self hidePanel];
}

+ (void)toggleWithSession:(smssession *)session
{
    g_session = session;
    if (!g_singleton) g_singleton = [[SMSControllersWindow alloc] init];

    if ([[g_singleton window] isVisible]) {
        [g_singleton hidePanel];
        [[g_singleton window] orderOut:nil];
    } else {
        [g_singleton refreshPads];
        [g_singleton layoutConsole];
        [g_singleton refresh];
        g_singleton->_liveTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [g_singleton tick];
        }];
        [[g_singleton window] makeKeyAndOrderFront:nil];
    }
}

+ (BOOL)isVisible
{
    return g_singleton && [[g_singleton window] isVisible];
}
@end
