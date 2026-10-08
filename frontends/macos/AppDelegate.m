/*
 * SMSAppDelegate -- the window, the menu bar and the session's lifetime.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "DisplayView.h"
#import "KeyForward.h"
#import "controllers/ControllersWindow.h"
#import "debugger/DebuggerWindow.h"

#include <stdlib.h>
#include <string.h>

#ifndef SMS_VERSION_STRING
#define SMS_VERSION_STRING "unknown"
#endif

#define APP_TITLE @"FujiNet Go SMS"

/* How long a gamepad connect/disconnect message stays over the picture. */
#define TOAST_SECONDS 3.0

/* How long a menu's Pause or Reset holds the console button: the console
 * samples its buttons once a frame, so a press and release in the same
 * instant would fall between two frames and never be seen. */
#define TAP_SECONDS 0.1

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

@class SMSAppDelegate;

/* The content view sits between AppKit and the session: it forwards keys
 * and the drop, and leaves the display purely about pixels. */
@interface SMSContentView : NSView
@property (nonatomic) smssession *session;
@property (nonatomic, weak) SMSAppDelegate *owner;
@end

@interface SMSAppDelegate ()
- (void)runSysaction:(int)sa;
- (void)loadMedia:(NSString *)path;
@end

@implementation SMSContentView

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    /* Cmd shortcuts are the menus'. One no menu claims still arrives here,
     * but AppKit sends no keyUp while Command is down, so forwarding it
     * would leave the machine holding the key. */
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t ks = SMSKeysymFromEvent(e);
    if (!ks) return;
    if (ks == SMS_KEYSYM_F9) { [SMSControllersWindow toggleWithSession:self.session]; return; }
    if (ks == SMS_KEYSYM_F11) { [[self window] toggleFullScreen:nil]; return; }
    if (ks == SMS_KEYSYM_F12) { [SMSDebuggerWindow toggleForSession:self.session]; return; }

    const int sa = smssession_key_sysaction(self.session, ks);
    if (sa >= 0) { [self.owner runSysaction:sa]; return; }
    smssession_key(self.session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    const uint32_t ks = SMSKeysymFromEvent(e);
    if (ks) smssession_key(self.session, ks, 0);
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp; a binding moved
 * onto Shift, Control or Option would otherwise never press. */
- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = SMSKeysymFromFlagsChange(e, &down);
    if (ks) smssession_key(self.session, ks, down);
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]] options:nil];
    if ([urls count] == 0) return NO;
    [self.owner loadMedia:[(NSURL *)urls[0] path]];
    return YES;
}
@end

@implementation SMSAppDelegate {
    smssession *_session;
    const char *_mediaPath;
    NSString *_pendingMedia;     /* a file Finder handed over before launch finished */
    BOOL _launched;
    BOOL _terminating;
    NSWindow *_window;
    SMSDisplayView *_display;
    SMSContentView *_content;
    NSTimer *_statusTimer;
    NSTimer *_sysactTimer;

    /* the transient gamepad hot-plug / import message */
    NSTextField *_toast;
    unsigned _toastGeneration;
    NSUInteger _toastSerial;

    NSMenuItem *_aspectItem;

    NSWindow *_settingsWindow;
    BOOL _sessionDirty;
    NSPopUpButton *_padList, *_padPort, *_aspectPopup;
    NSPopUpButton *_consolePopup, *_biosPopup;
    NSButton *_biosImport;
    NSButton *_fmUnitBox, *_fmMutesBox;
    NSTextField *_fmNote, *_ymNote, *_webUiLabel;
    NSTimer *_settingsTimer;
    unsigned _padGeneration;

    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
}

- (instancetype)initWithSession:(smssession *)session mediaPath:(const char *)mediaPath
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mediaPath = mediaPath;
    return self;
}

/* The console the settings name now: what the next restart boots, and
 * ahead of smssession_console() while Settings has a change pending. */
- (int)settingsConsole
{
    const int c = smssession_get_int(_session, "console", SMS_CONSOLE_SMS1);
    return (c >= 0 && c < SMS_CONSOLE_COUNT) ? c : SMS_CONSOLE_SMS1;
}

- (NSString *)lastError
{
    const char *e = smssession_last_error(_session);
    return (e && e[0]) ? [NSString stringWithUTF8String:e] : @"The machine is not running.";
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    /* 268x224 at 8:7 pixels is about 306x224; three times that is a
     * comfortable first window on any Mac. */
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 919, 672)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:APP_TITLE];
    [_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [_window center];

    _content = [[SMSContentView alloc] initWithFrame:[[_window contentView] bounds]];
    _content.session = _session;
    _content.owner = self;
    [_content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

    _display = [[SMSDisplayView alloc] initWithSession:_session];
    [_display setFrame:[_content bounds]];
    [_display setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_display setTvAspect:smssession_get_int(_session, "aspect", 0) == 0];
    [_display setSmooth:smssession_get_int(_session, "smooth", 0) != 0];
    [_content addSubview:_display];

    /* The toast: a label over the top of the picture that fades. */
    _toast = [NSTextField labelWithString:@""];
    _toast.font = [NSFont boldSystemFontOfSize:13];
    _toast.textColor = NSColor.whiteColor;
    _toast.drawsBackground = YES;
    _toast.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.7];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.alphaValue = 0.0;
    [_toast setFrame:NSMakeRect(0, [_content bounds].size.height - 34, [_content bounds].size.width, 26)];
    [_toast setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [_content addSubview:_toast];

    [_window setContentView:_content];
    [self buildMenu];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_content];
    [_window setDelegate:self];

    /* A file from the command line or from Finder is routed as a drop: a
     * cartridge boots straight away; a BIOS, or anything for the SD folder,
     * is handled once the machine is up. */
    NSString *media = _pendingMedia;
    if (!media && _mediaPath)
        media = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:_mediaPath
                                                                            length:strlen(_mediaPath)];
    _pendingMedia = nil;

    smssession_start_opts opts;
    smssession_default_opts(_session, &opts);
    if (media && !smssession_media_is_bios([media fileSystemRepresentation]) &&
        smssession_media_is_cartridge([media fileSystemRepresentation])) {
        opts.cart_path = [media fileSystemRepresentation];
        media = nil;
    }
    if (smssession_start(_session, &opts) != 0) {
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Could not start"];
        [a setInformativeText:[self lastError]];
        [a runModal];
    }
    _launched = YES;
    _toastGeneration = smssession_gamepad_generation(_session);
    if (media) [self loadMedia:media];

    /* The family's launch hooks, for when the app misbehaves before a menu
     * is reachable. */
    if (getenv("SMS_OPEN_CONTROLLERS")) [SMSControllersWindow toggleWithSession:_session];
    if (getenv("SMS_OPEN_DEBUGGER")) [SMSDebuggerWindow showForSession:_session];
    if (getenv("SMS_OPEN_SETTINGS")) [self showSettings:nil];

    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
        block:^(NSTimer *t) { (void)t; [self updateTitle]; }];
    /* System actions the gamepad thread resolved (it cannot touch AppKit),
     * and gamepads coming and going. */
    _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES
        block:^(NSTimer *t) {
            (void)t;
            int sa;
            while (smssession_sysaction_take(self->_session, &sa)) [self runSysaction:sa];
            const unsigned gen = smssession_gamepad_generation(self->_session);
            if (gen != self->_toastGeneration) {
                self->_toastGeneration = gen;
                char msg[160];
                if (smssession_gamepad_last_event(self->_session, msg, sizeof msg) > 0)
                    [self showToast:[NSString stringWithUTF8String:msg]];
            }
        }];
    [self updateTitle];
}

- (void)showToast:(NSString *)text
{
    if (!text) return;
    _toast.stringValue = text;
    _toast.alphaValue = 1.0;
    const NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(TOAST_SECONDS * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* a newer message restarts the clock */
        if (serial != self->_toastSerial) return;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.5;
            self->_toast.animator.alphaValue = 0.0;
        } completionHandler:nil];
    });
}

- (void)runSysaction:(int)sa
{
    switch (sa) {
    case SMS_SYSACT_RESET_CONFIG:
    case SMS_SYSACT_SOFT_RESET:
        smssession_sysaction(_session, sa);
        break;
    case SMS_SYSACT_DEBUG_STOP:
        /* the debugger window attaches, which stops the machine */
        [SMSDebuggerWindow showForSession:_session];
        break;
    default: break;
    }
    [self updateTitle];
}

- (void)updateTitle
{
    NSString *state;
    char st[160];
    if (!smssession_is_running(_session)) {
        state = @"stopped";
    } else {
        smssession_cart_status(_session, st, sizeof st);
        /* the status dot: filled while the cartridge's link to FujiNet is up */
        const int linkUp = smssession_cart_link_up(_session);
        state = [NSString stringWithFormat:@"%@FujiNet: %s",
                 linkUp > 0 ? @"● " : (linkUp == 0 ? @"○ " : @""), st];
        const char *cart = smssession_cart_path(_session);
        if (cart && cart[0])
            state = [NSString stringWithFormat:@"%@ — %@",
                     [[NSString stringWithUTF8String:cart] lastPathComponent], state];
        const char *console = sms_console_name(smssession_console(_session));
        if (console)
            state = [NSString stringWithFormat:@"%s — %@", console, state];
    }
    /* The title bar is the status bar here: an AppKit window has no natural
     * place for one, and a floating HUD over the picture would be worse. */
    [_window setTitle:[NSString stringWithFormat:@"%@ — %@", APP_TITLE, state]];
}

/* Losing key status with keys held would leave the machine believing they
 * are still down. */
- (void)windowDidResignKey:(NSNotification *)note
{
    if (note.object == _window) smssession_release_all(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    _terminating = YES;
    [_statusTimer invalidate];
    [_sysactTimer invalidate];
    [_settingsTimer invalidate];
    [_logTimer invalidate];
    [_display stop];
    smssession_stop(_session);
    smssession_free(_session);
    _session = NULL;
}

- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename
{
    (void)app;
    /* Opening a file from Finder launches the app and delivers the file
     * before applicationDidFinishLaunching:, when there is no machine yet
     * to load it into: hold it until there is. */
    if (!_launched) {
        _pendingMedia = filename;
        return YES;
    }
    [self loadMedia:filename];
    return YES;
}

/* ---- menu ------------------------------------------------------------------ */

- (NSMenuItem *)item:(NSMenu *)menu title:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *it = [menu addItemWithTitle:title action:sel keyEquivalent:key];
    [it setTarget:self];
    return it;
}

- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];

    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [self item:appMenu title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self item:appMenu title:@"Settings…" action:@selector(showSettings:) key:@","];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide " APP_TITLE action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *others = [appMenu addItemWithTitle:@"Hide Others"
                                            action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [others setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
    [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit " APP_TITLE action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    [self item:machine title:@"Open Cartridge…" action:@selector(openCart:) key:@"o"];
    [self item:machine title:@"Eject Cartridge" action:@selector(ejectCart:) key:@""];
    [self item:machine title:@"Import Cartridge to SD…" action:@selector(importToSd:) key:@""];
    [self item:machine title:@"Import BIOS…" action:@selector(importBios:) key:@""];
    [machine addItem:[NSMenuItem separatorItem]];
    /* Return, Backspace and F3 are bindings (the console's Pause and Reset
     * buttons, the /RESET line), not key equivalents: a menu equivalent
     * would take the key before the bindings table sees it and could not be
     * remapped. The titles name whatever key is bound now (see
     * validateMenuItem:). */
    [self item:machine title:@"Pause" action:@selector(pauseButton:) key:@""];
    [self item:machine title:@"Reset Button" action:@selector(resetButton:) key:@""];
    [self item:machine title:@"Soft Reset" action:@selector(softReset:) key:@""];
    [self item:machine title:@"Reset to CONFIG" action:@selector(resetConfig:) key:@"r"];
    [machineItem setSubmenu:machine];
    [bar addItem:machineItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [self item:view title:@"Controllers (F9)" action:@selector(toggleControllers:) key:@"j"];
    [self item:view title:@"Debugger (F12)" action:@selector(toggleDebugger:) key:@"d"];
    [view addItem:[NSMenuItem separatorItem]];
    _aspectItem = [self item:view title:@"TV Aspect" action:@selector(toggleAspect:) key:@""];
    [_aspectItem setState:(smssession_get_int(_session, "aspect", 0) == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    NSMenuItem *sm = [self item:view title:@"Smooth Scaling" action:@selector(toggleSmooth:) key:@""];
    [sm setState:(smssession_get_int(_session, "smooth", 0) ? NSControlStateValueOn : NSControlStateValueOff)];
    /* F11 toggles it too, from the machine's window (see SMSContentView). */
    NSMenuItem *fs = [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    [fs setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];
    [viewItem setSubmenu:view];
    [bar addItem:viewItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [self item:fuji title:@"Web UI" action:@selector(openWebUI:) key:@""];
    [self item:fuji title:@"Console Log" action:@selector(showFujiNetLog:) key:@""];
    [fujiItem setSubmenu:fuji];
    [bar addItem:fujiItem];

    /* Window and Help: AppKit fills these in (the window list, the Help
     * search field) once they are registered as such. */
    NSMenuItem *windowItem = [[NSMenuItem alloc] init];
    NSMenu *windows = [[NSMenu alloc] initWithTitle:@"Window"];
    [windows addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windows addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [windows addItem:[NSMenuItem separatorItem]];
    [windows addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];
    [windowItem setSubmenu:windows];
    [bar addItem:windowItem];

    NSMenuItem *helpItem = [[NSMenuItem alloc] init];
    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    [self item:help title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];
    [helpItem setSubmenu:help];
    [bar addItem:helpItem];

    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:windows];
    [NSApp setHelpMenu:help];
}

/* ---- actions ---------------------------------------------------------------- */

- (void)alert:(NSString *)title text:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    if (text) [a setInformativeText:text];
    [a runModal];
}

- (void)showAbout:(id)sender
{
    (void)sender;
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    NSAttributedString *credits = [[NSAttributedString alloc]
        initWithString:@"A Sega Master System with a built-in FujiNet.\n\n"
                        "Licensed under the GNU General Public License, version 3 or later "
                        "(GPL-3.0-or-later).\n\n"
                        "Built on MAME's Master System drivers (BSD-3-Clause), floooh's "
                        "z80.h (zlib), ymfm (BSD-3-Clause) and the FujiNet firmware."
            attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:NSFont.smallSystemFontSize],
                          NSForegroundColorAttributeName: NSColor.labelColor,
                          NSParagraphStyleAttributeName: para }];
    [NSApp orderFrontStandardAboutPanelWithOptions:@{
        NSAboutPanelOptionApplicationName: APP_TITLE,
        NSAboutPanelOptionApplicationVersion: [NSString stringWithUTF8String:SMS_VERSION_STRING],
        NSAboutPanelOptionVersion: @"",
        NSAboutPanelOptionCredits: credits,
    }];
}

- (void)openCartAtPath:(NSString *)path
{
    if (smssession_load_cart(_session, [path fileSystemRepresentation]) != 0)
        [self alert:@"Could not open" text:[self lastError]];
    [self updateTitle];
}

/* Whatever was imported, the session's message says what happened -- the
 * console it fits, the custom-CRC warning, the next power cycle. Returns
 * the image's index, or -1. */
- (int)importBiosAtPath:(NSString *)path
{
    char msg[512];
    const int idx = smssession_import_bios(_session, [path fileSystemRepresentation], msg, sizeof msg);
    [self alert:(idx >= 0 ? @"BIOS imported" : @"Import failed")
           text:(msg[0] ? [NSString stringWithUTF8String:msg] : [self lastError])];
    [self refreshMachineSettings];
    return idx;
}

- (void)loadMedia:(NSString *)path
{
    const char *fs = [path fileSystemRepresentation];
    if (smssession_media_is_bios(fs)) {
        [self importBiosAtPath:path];
        return;
    }
    if (smssession_media_is_cartridge(fs)) {
        [self openCartAtPath:path];
        return;
    }
    char dest[1024];
    if (smssession_import_media(_session, fs, dest, sizeof dest) != 0) {
        [self alert:@"Import failed" text:[self lastError]];
        return;
    }
    [self showToast:[NSString stringWithFormat:@"%@ copied to FujiNet's SD folder. Mount it from CONFIG.",
                     [[NSString stringWithUTF8String:dest] lastPathComponent]]];
}

- (NSString *)pickFile:(NSString *)title types:(NSArray<NSString *> *)types
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setTitle:title];
    p.allowedContentTypes = typesForExtensions(types);
    if ([p runModal] != NSModalResponseOK) return nil;
    return [[p URL] path];
}

/* A .cfg beside the image travels with it (the session finds it). */
- (NSString *)pickCartridge:(NSString *)title
{
    return [self pickFile:title types:@[@"sms", @"sg", @"bin", @"rom"]];
}

- (void)openCart:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Open Cartridge"];
    if (path) [self openCartAtPath:path];
}

- (void)ejectCart:(id)sender
{
    (void)sender;
    if (smssession_eject(_session) != 0)
        [self alert:@"Could not eject" text:[self lastError]];
    [self updateTitle];
}

- (void)importToSd:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Import Cartridge to SD"];
    if (!path) return;
    char dest[1024];
    if (smssession_import_cart_to_sd(_session, [path fileSystemRepresentation], dest, sizeof dest) != 0) {
        [self alert:@"Import failed" text:[self lastError]];
        return;
    }
    [self showToast:[NSString stringWithFormat:@"%@ is on the SD host. Boot it from CONFIG.",
                     [[NSString stringWithUTF8String:dest] lastPathComponent]]];
}

/* The YM2413's instrument ROM comes in this way too. From Settings, an
 * image that fits the console chosen there becomes its BIOS (the session
 * gives it to the console running now), applied with the window's other
 * machine options when it closes. */
- (void)importBios:(id)sender
{
    NSString *path = [self pickFile:@"Import BIOS" types:@[@"sms", @"bin", @"rom", @"ic2"]];
    if (!path) return;
    const int idx = [self importBiosAtPath:path];
    if (idx < 0 || sender != _biosImport) return;
    const int console = [self settingsConsole];
    const sms_bios_info *b = smssession_bios_info(idx);
    if (b && ((b->consoles >> console) & 1u)) smssession_set_console_bios(_session, console, b->name);
    _sessionDirty = YES;
    [self refreshMachineSettings];
}

/* Press now, release a beat later (see TAP_SECONDS). */
- (void)tapSwitch:(int)sw
{
    const int target = SMS_TARGET_SWITCH(sw);
    smssession_press(_session, target, 1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(TAP_SECONDS * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        if (!self->_terminating) smssession_press(self->_session, target, 0);
    });
}

- (void)pauseButton:(id)sender { (void)sender; [self tapSwitch:SMS_SW_PAUSE]; }
- (void)resetButton:(id)sender { (void)sender; [self tapSwitch:SMS_SW_RESET]; }
- (void)softReset:(id)sender { (void)sender; [self runSysaction:SMS_SYSACT_SOFT_RESET]; }
- (void)resetConfig:(id)sender { (void)sender; [self runSysaction:SMS_SYSACT_RESET_CONFIG]; }

/* "Pause (Return)": the key bound to a target now, if any. */
- (NSString *)title:(NSString *)base forTarget:(int)target
{
    char key[32];
    const sms_binding b = smssession_binding_get(_session, target);
    if (!b.keysym || smssession_keysym_name(b.keysym, key, sizeof key) <= 0) return base;
    return [NSString stringWithFormat:@"%@ (%s)", base, key];
}

/* The console buttons' titles follow the bindings (the Controllers panel
 * remaps them), and the Reset button exists only on the consoles that have
 * one: the Master System, and the Japanese one's Rapid button in its
 * place. */
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const SEL a = [item action];
    const int console = smssession_console(_session);
    if (a == @selector(pauseButton:)) {
        [item setTitle:[self title:@"Pause" forTarget:SMS_TARGET_SWITCH(SMS_SW_PAUSE)]];
        return smssession_is_running(_session);
    }
    if (a == @selector(resetButton:)) {
        [item setTitle:[self title:(console == SMS_CONSOLE_SMSJ ? @"Rapid Button" : @"Reset Button")
                         forTarget:SMS_TARGET_SWITCH(SMS_SW_RESET)]];
        return smssession_is_running(_session) && sms_console_has_reset_button(console);
    }
    if (a == @selector(softReset:)) {
        [item setTitle:[self title:@"Soft Reset" forTarget:SMS_TARGET_SYSACT(SMS_SYSACT_SOFT_RESET)]];
        return smssession_is_running(_session);
    }
    if (a == @selector(resetConfig:)) {
        [item setTitle:[self title:@"Reset to CONFIG" forTarget:SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG)]];
        return smssession_is_running(_session);
    }
    if (a == @selector(ejectCart:))
        return smssession_is_running(_session);
    return YES;
}

- (void)toggleControllers:(id)sender { (void)sender; [SMSControllersWindow toggleWithSession:_session]; }
- (void)toggleDebugger:(id)sender { (void)sender; [SMSDebuggerWindow toggleForSession:_session]; }

/* aspect 0: the TV's pixel aspect (the default); 1: square pixels. */
- (void)setAspect:(int)aspect
{
    smssession_set_int(_session, "aspect", aspect);
    [_display setTvAspect:aspect == 0];
    [_aspectItem setState:(aspect == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    if (_aspectPopup) [_aspectPopup selectItemAtIndex:aspect];
}

- (void)toggleAspect:(id)sender
{
    (void)sender;
    [self setAspect:smssession_get_int(_session, "aspect", 0) == 0 ? 1 : 0];
}

- (void)toggleSmooth:(id)sender
{
    NSMenuItem *item = sender;
    const BOOL on = ([item state] != NSControlStateValueOn);
    [item setState:(on ? NSControlStateValueOn : NSControlStateValueOff)];
    [_display setSmooth:on];
    smssession_set_int(_session, "smooth", on ? 1 : 0);
}

/* ---- settings ----------------------------------------------------------------
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. The analog stick, the
 * picture and the volume apply live; the console, its BIOS and the FM
 * options power-cycle the machine, and the host options restart the
 * session, when the window closes (smssession_restart does whichever the
 * changes need).
 */

- (NSTextField *)sectionLabel:(NSString *)title
{
    NSTextField *label = [NSTextField labelWithString:title];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

- (NSTextField *)note:(NSString *)text
{
    NSTextField *n = [NSTextField labelWithString:text];
    n.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    n.textColor = NSColor.secondaryLabelColor;
    return n;
}

- (NSPopUpButton *)popUpForKey:(const char *)key fallback:(int)def names:(const char *(*)(int))names
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] init];
    for (int i = 0; names(i); i++) [popup addItemWithTitle:[NSString stringWithUTF8String:names(i)]];
    NSInteger current = smssession_get_int(_session, key, def);
    if (current < 0 || current >= (NSInteger)popup.numberOfItems) current = def;
    [popup selectItemAtIndex:current];
    popup.identifier = @(key);
    popup.target = self;
    popup.action = @selector(settingChanged:);
    return popup;
}

- (NSButton *)checkBoxForKey:(const char *)key title:(NSString *)title fallback:(int)def
{
    NSButton *box = [NSButton checkboxWithTitle:title target:self action:@selector(settingChanged:)];
    box.state = smssession_get_int(_session, key, def) ? NSControlStateValueOn : NSControlStateValueOff;
    box.identifier = @(key);
    return box;
}

/* The rows that depend on the console chosen: its BIOS choices (every
 * imported image that fits it), its FM sound, and whether the YM2413's
 * instrument ROM is in. */
- (void)refreshMachineSettings
{
    if (!_settingsWindow) return;
    const int console = [self settingsConsole];
    if ((int)_consolePopup.indexOfSelectedItem != console) [_consolePopup selectItemAtIndex:console];

    [_biosPopup removeAllItems];
    if (!sms_console_has_bios_socket(console)) {
        [_biosPopup addItemWithTitle:@"None (this console has no BIOS socket)"];
        _biosPopup.lastItem.representedObject = @"";
        _biosPopup.enabled = NO;
    } else {
        const char *current = smssession_console_bios(_session, console);
        NSInteger sel = 0;
        [_biosPopup addItemWithTitle:@"None (boot the cartridge)"];
        _biosPopup.lastItem.representedObject = @"";
        /* <= count: the custom image sits just past the table */
        for (int i = 0; i <= smssession_bios_count(); i++) {
            const sms_bios_info *b = smssession_bios_info(i);
            if (!b || !((b->consoles >> console) & 1u) || !smssession_bios_available(_session, i)) continue;
            [_biosPopup addItemWithTitle:[NSString stringWithUTF8String:b->desc]];
            _biosPopup.lastItem.representedObject = [NSString stringWithUTF8String:b->name];
            if (current && !strcmp(current, b->name)) sel = _biosPopup.numberOfItems - 1;
        }
        /* A choice whose image has gone from the ROM directory: say so
         * rather than pretend it is None. */
        if (current && current[0] && sel == 0) {
            [_biosPopup addItemWithTitle:[NSString stringWithFormat:@"%s (not imported)", current]];
            _biosPopup.lastItem.representedObject = [NSString stringWithUTF8String:current];
            sel = _biosPopup.numberOfItems - 1;
        }
        [_biosPopup selectItemAtIndex:sel];
        _biosPopup.enabled = YES;
    }

    /* The Mark III takes the FM Sound Unit; the Japanese Master System has
     * the YM2413 built in; the others have no FM at all. */
    const BOOL unit = console == SMS_CONSOLE_MARK3;
    _fmUnitBox.hidden = !unit;
    _fmMutesBox.hidden = !unit;
    _fmMutesBox.enabled = _fmUnitBox.state == NSControlStateValueOn;
    _fmNote.hidden = unit;
    _fmNote.stringValue = sms_console_has_fm(console) ? @"YM2413 built in"
                                                      : @"None (the Japanese Master System and the Mark III have FM)";

    const int ym = smssession_bios_find("ym2413");
    _ymNote.stringValue = (ym >= 0 && smssession_bios_available(_session, ym))
        ? @"YM2413 instrument ROM imported: FM plays its patches"
        : @"YM2413 instrument ROM not imported: FM plays ymfm's built-in patches";
}

- (void)refreshPadList
{
    const NSInteger sel = _padList.indexOfSelectedItem;
    [_padList removeAllItems];
    const int n = smssession_gamepad_count(_session);
    if (n == 0) [_padList addItemWithTitle:@"(no gamepads connected)"];
    for (int i = 0; i < n; i++) {
        char name[128];
        const int eff = smssession_gamepad_effective_port(_session, i);
        smssession_gamepad_name(_session, i, name, sizeof name);
        /* Menu items with equal titles collapse; the index keeps them apart. */
        [_padList addItemWithTitle:(eff >= 0
            ? [NSString stringWithFormat:@"%d: %s  [player %d]", i + 1, name, eff + 1]
            : [NSString stringWithFormat:@"%d: %s  [unused]", i + 1, name])];
    }
    if (sel >= 0 && sel < n) [_padList selectItemAtIndex:sel];
    [self padSelected:nil];
}

- (void)padSelected:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < smssession_gamepad_count(_session))
        [_padPort selectItemAtIndex:smssession_gamepad_assignment(_session, (int)sel) + 1];
}

- (void)padPortChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < smssession_gamepad_count(_session))
        smssession_gamepad_assign(_session, (int)sel, (int)_padPort.indexOfSelectedItem - 1);
    [self refreshPadList];
}

- (void)volumeChanged:(NSSlider *)sender
{
    smssession_set_volume(_session, (int)sender.integerValue);
}

- (void)aspectChanged:(NSPopUpButton *)sender
{
    [self setAspect:(int)sender.indexOfSelectedItem];
}

- (void)biosChanged:(NSPopUpButton *)sender
{
    NSString *name = sender.selectedItem.representedObject;
    smssession_set_console_bios(_session, [self settingsConsole], name ? name.UTF8String : "");
    _sessionDirty = YES;
}

- (void)refreshWebUi
{
    if (!_webUiLabel) return;
    const char *url = smssession_fujinet_webui_url(_session);
    _webUiLabel.stringValue = (smssession_fujinet_running(_session) && url && url[0])
        ? [NSString stringWithUTF8String:url] : @"(FujiNet is not running)";
}

- (void)settingChanged:(id)sender
{
    NSControl *control = sender;
    const char *key = [control.identifier UTF8String];
    int value;

    if ([control isKindOfClass:[NSPopUpButton class]])
        value = (int)((NSPopUpButton *)control).indexOfSelectedItem;
    else
        value = ((NSButton *)control).state == NSControlStateValueOn ? 1 : 0;

    if (!strcmp(key, "analog_joystick")) {
        smssession_set_analog(_session, value);
        return;
    }
    smssession_set_int(_session, key, value);
    _sessionDirty = YES;
    if (!strcmp(key, "console") || !strcmp(key, "fm_unit")) [self refreshMachineSettings];
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [self refreshMachineSettings];
        [self refreshWebUi];
        [self startSettingsTimer];
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    _consolePopup = [self popUpForKey:"console" fallback:SMS_CONSOLE_SMS1 names:sms_console_name];

    _biosPopup = [[NSPopUpButton alloc] init];
    _biosPopup.target = self;
    _biosPopup.action = @selector(biosChanged:);
    _biosImport = [NSButton buttonWithTitle:@"Import BIOS…" target:self action:@selector(importBios:)];
    NSStackView *biosRow = [NSStackView stackViewWithViews:@[_biosPopup, _biosImport]];
    biosRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    _fmUnitBox = [self checkBoxForKey:"fm_unit" title:@"FM Sound Unit fitted" fallback:1];
    _fmMutesBox = [self checkBoxForKey:"fm_unit_mutes_psg" title:@"FM mutes the PSG" fallback:0];
    _fmNote = [NSTextField labelWithString:@""];
    NSStackView *fmCol = [NSStackView stackViewWithViews:@[_fmUnitBox, _fmMutesBox, _fmNote]];
    fmCol.orientation = NSUserInterfaceLayoutOrientationVertical;
    fmCol.alignment = NSLayoutAttributeLeading;
    _ymNote = [self note:@""];

    _padList = [[NSPopUpButton alloc] init];
    _padList.target = self;
    _padList.action = @selector(padSelected:);
    _padPort = [[NSPopUpButton alloc] init];
    [_padPort addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2"]];
    _padPort.target = self;
    _padPort.action = @selector(padPortChanged:);

    _aspectPopup = [[NSPopUpButton alloc] init];
    [_aspectPopup addItemsWithTitles:@[@"TV (8:7 pixels, 1.39:1 on PAL)", @"Square pixels"]];
    [_aspectPopup selectItemAtIndex:(smssession_get_int(_session, "aspect", 0) ? 1 : 0)];
    _aspectPopup.target = self;
    _aspectPopup.action = @selector(aspectChanged:);

    NSSlider *volume = [NSSlider sliderWithValue:smssession_get_int(_session, "volume", 100)
                                        minValue:0 maxValue:100 target:self action:@selector(volumeChanged:)];
    [volume.widthAnchor constraintEqualToConstant:220].active = YES;

    NSStackView *padRow = [NSStackView stackViewWithViews:@[_padList, _padPort]];
    padRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    _webUiLabel = [NSTextField labelWithString:@""];
    _webUiLabel.selectable = YES;
    NSButton *webUiOpen = [NSButton buttonWithTitle:@"Open" target:self action:@selector(openWebUI:)];
    NSStackView *webUiRow = [NSStackView stackViewWithViews:@[_webUiLabel, webUiOpen]];
    webUiRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self sectionLabel:@"Machine"], [self note:@"console, BIOS and FM apply with a power cycle when this window closes"] ],
        @[ [NSTextField labelWithString:@"Console"], _consolePopup ],
        @[ [NSTextField labelWithString:@"BIOS"], biosRow ],
        @[ [NSTextField labelWithString:@"FM sound"], fmCol ],
        @[ [NSTextField labelWithString:@""], _ymNote ],
        @[ [NSTextField labelWithString:@"Picture"], _aspectPopup ],
        @[ [self sectionLabel:@"Audio"], [self note:@"the volume applies immediately; audio on/off restarts the session"] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_audio" title:@"Audio" fallback:1] ],
        @[ [NSTextField labelWithString:@"Volume"], volume ],
        @[ [self sectionLabel:@"Input"], [self note:@"applied immediately; gamepads on/off restarts the session"] ],
        @[ [NSTextField labelWithString:@""],
           [self checkBoxForKey:"analog_joystick" title:@"Analog sticks drive the D-pad" fallback:1] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_gamepad" title:@"Gamepads" fallback:1] ],
        @[ [NSTextField labelWithString:@"Gamepads"], padRow ],
        @[ [self sectionLabel:@"FujiNet"], [self note:@"applied by restarting the session when this window closes"] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_fujinet" title:@"Enable FujiNet" fallback:1] ],
        @[ [NSTextField labelWithString:@"Web UI"], webUiRow ],
    ];

    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSStackView *root = [NSStackView stackViewWithViews:@[grid]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);

    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 640, 660)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    _settingsWindow.contentView = root;
    [_settingsWindow center];

    [self refreshMachineSettings];
    [self refreshWebUi];
    [self startSettingsTimer];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

- (void)startSettingsTimer
{
    [_settingsTimer invalidate];
    _padGeneration = smssession_gamepad_generation(_session);
    [self refreshPadList];
    _settingsTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
        (void)t;
        const unsigned gen = smssession_gamepad_generation(self->_session);
        if (gen != self->_padGeneration) { self->_padGeneration = gen; [self refreshPadList]; }
    }];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
        return;
    }
    if (note.object != _settingsWindow) return;
    [_settingsTimer invalidate];
    _settingsTimer = nil;
    if (!_sessionDirty) return;
    _sessionDirty = NO;

    /* A power cycle for the machine options, a full restart if a host
     * option changed: the session knows which. */
    if (smssession_restart(_session) != 0)
        [self alert:@"Could not start" text:[self lastError]];
    [self updateTitle];
}

/* ---- FujiNet console log ------------------------------------------------------- */

- (void)refreshLog:(NSTimer *)timer
{
    (void)timer;
    static char buf[128 * 1024];
    const int n = smssession_fujinet_copy_log(_session, buf, sizeof buf);
    NSScrollView *scroll = (NSScrollView *)_logView.enclosingScrollView;
    const BOOL atEnd = !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                                   NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf] : nil;
    [_logView setString:(text ?: @"(no FujiNet output yet)")];
    if (atEnd) [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        if (!_logTimer)
            _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                                       selector:@selector(refreshLog:) userInfo:nil repeats:YES];
        return;
    }

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = NO;

    _logView = [[NSTextView alloc] initWithFrame:scroll.bounds];
    _logView.editable = NO;
    _logView.richText = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _logView.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = _logView;

    _logWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 860, 600)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;
    _logWindow.delegate = self;
    _logWindow.contentView = scroll;
    [_logWindow center];

    _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                               selector:@selector(refreshLog:) userInfo:nil repeats:YES];
    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
}

- (void)openWebUI:(id)sender
{
    (void)sender;
    if (!smssession_fujinet_running(_session)) {
        [self alert:@"FujiNet is not running" text:nil];
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:
        [NSURL URLWithString:[NSString stringWithUTF8String:smssession_fujinet_webui_url(_session)]]];
}
@end
