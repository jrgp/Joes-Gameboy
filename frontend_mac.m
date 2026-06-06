/* frontend_mac.m — Native macOS (AppKit) frontend for Joe's Gameboy
 *
 * Compiled as part of CGB.xcodeproj alongside gb.c, savestate.c, palette.c.
 * Provides its own main(); do NOT link main.c when building this target.
 *
 * Rendering:  CoreGraphics (CGImage drawn into NSView.drawRect:)
 * Game loop:  NSTimer at 1/60 s on the main run-loop
 * Input:      NSView keyDown/keyUp with macOS virtual-key codes
 * Menus:      programmatic AppKit (no NIB / storyboard)
 */

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb.h"
#include "savestate.h"
#include "palette.h"
#include "constants.h"

/* -----------------------------------------------------------------------
 * Globals normally defined in main.c
 * -------------------------------------------------------------------- */
volatile sig_atomic_t g_shutdown_requested = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

extern void cpu_close_debug_file(void);

@protocol ROMOpener <NSObject>
- (void)openROM:(NSString *)path;
@end

/* -----------------------------------------------------------------------
 * GBView — renders the 160×144 pixel buffer via CoreGraphics
 * -------------------------------------------------------------------- */

@interface GBView : NSView
@end

@implementation GBView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self)
        [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    return self;
}

- (BOOL)acceptsFirstResponder { return YES; }

/* -----------------------------------------------------------------------
 * Drag-and-drop — accept .gb / .gbc / .rom files
 * -------------------------------------------------------------------- */
static BOOL urlIsROM(NSURL *url) {
    NSString *ext = url.pathExtension.lowercaseString;
    return [@[@"gb", @"gbc", @"rom"] containsObject:ext];
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    NSArray *urls = [sender.draggingPasteboard
        readObjectsForClasses:@[[NSURL class]]
        options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    for (NSURL *u in urls)
        if (urlIsROM(u)) return NSDragOperationCopy;
    return NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray *urls = [sender.draggingPasteboard
        readObjectsForClasses:@[[NSURL class]]
        options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    for (NSURL *u in urls) {
        if (urlIsROM(u)) {
            [(id<ROMOpener>)[NSApp delegate] openROM:u.path];
            return YES;
        }
    }
    return NO;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];

    if (!pixels) {
        CGContextSetGrayFillColor(ctx, 0.05, 1.0);
        CGContextFillRect(ctx, NSRectToCGRect(self.bounds));
        return;
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();

    /*
     * Pixel memory layout: bytes [R, G, B, A] per pixel.
     * (pallette encoding: uint32 = A<<24|B<<16|G<<8|R → LE bytes: R,G,B,A)
     * Use big-endian interpretation so component[0]=R and skip last (A=0xFF).
     */
    CGBitmapInfo bitmapInfo =
        (CGBitmapInfo)kCGBitmapByteOrder32Big | kCGImageAlphaNoneSkipLast;

    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL,
        pixels,
        (size_t)(VIEWPORT_WIDTH * VIEWPORT_HEIGHT) * sizeof(uint32_t),
        NULL   /* pixels lives for the app lifetime; no release callback needed */
    );

    CGImageRef img = CGImageCreate(
        (size_t)VIEWPORT_WIDTH, (size_t)VIEWPORT_HEIGHT,
        8, 32, (size_t)VIEWPORT_WIDTH * 4,
        cs, bitmapInfo,
        provider, NULL, false, kCGRenderingIntentDefault
    );

    CGColorSpaceRelease(cs);
    CGDataProviderRelease(provider);

    if (img) {
        NSRect bounds = self.bounds;
        CGFloat vw = NSWidth(bounds);
        CGFloat vh = NSHeight(bounds);

        /* Compute the largest VIEWPORT_WIDTH:VIEWPORT_HEIGHT rect that fits. */
        CGFloat aspect = (CGFloat)VIEWPORT_WIDTH / (CGFloat)VIEWPORT_HEIGHT;
        CGFloat fitW = vw;
        CGFloat fitH = vw / aspect;
        if (fitH > vh) { fitH = vh; fitW = vh * aspect; }

        CGFloat ox = (vw - fitW) * 0.5;
        CGFloat oy = (vh - fitH) * 0.5;
        CGRect gameRect = CGRectMake(ox, oy, fitW, fitH);

        /* Black bars in the margins. */
        CGContextSetGrayFillColor(ctx, 0.0, 1.0);
        CGContextFillRect(ctx, NSRectToCGRect(bounds));

        CGContextDrawImage(ctx, gameRect, img);
        CGImageRelease(img);
    }
}

/* -----------------------------------------------------------------------
 * Key input
 * macOS virtual-key codes (Carbon HIToolbox/Events.h)
 * GB joypad bits: 0=RIGHT 1=LEFT 2=UP 3=DOWN 4=A 5=B 6=SELECT 7=START
 * -------------------------------------------------------------------- */
static int keycode_to_gb_bit(unsigned short kc) {
    switch (kc) {
        case 124: return 0;   /* →  RIGHT  */
        case 123: return 1;   /* ←  LEFT   */
        case 126: return 2;   /* ↑  UP     */
        case 125: return 3;   /* ↓  DOWN   */
        case   0: return 4;   /* A  button */
        case   1: return 5;   /* S  button */
        case  36: return 7;   /* ↵  START  */
        case  56: return 6;   /* ⇧  SELECT */
        default:  return -1;
    }
}

- (void)keyDown:(NSEvent *)event {
    int bit = keycode_to_gb_bit(event.keyCode);
    if (bit >= 0) gb_set_button(bit, true);
}

- (void)keyUp:(NSEvent *)event {
    int bit = keycode_to_gb_bit(event.keyCode);
    if (bit >= 0) gb_set_button(bit, false);
}

@end  /* GBView */


/* -----------------------------------------------------------------------
 * GBWindowController — window owner and emulation timer
 * -------------------------------------------------------------------- */

@interface GBWindowController : NSWindowController <NSWindowDelegate>

@property (nonatomic, strong) GBView   *gbView;
@property (nonatomic, strong) NSTimer  *frameTimer;
@property (nonatomic, assign) BOOL      romLoaded;
@property (nonatomic, assign) BOOL      paused;
@property (nonatomic, copy)   NSString *currentROMPath;

- (void)openROM:(NSString *)path;
- (void)saveState;
- (void)loadState;
- (void)resetEmulator;
- (void)togglePause;

@end

@implementation GBWindowController

- (instancetype)init {
    NSUInteger style =
        NSWindowStyleMaskTitled       |
        NSWindowStyleMaskClosable     |
        NSWindowStyleMaskMiniaturizable |
        NSWindowStyleMaskResizable;

    NSRect frame = NSMakeRect(0, 0,
                              VIEWPORT_WIDTH  * 3,
                              VIEWPORT_HEIGHT * 3);

    NSWindow *win = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:style
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    self = [super initWithWindow:win];
    if (!self) return nil;

    win.title    = @"Joe's Gameboy";
    win.delegate = self;
    win.minSize  = NSMakeSize(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    [win setContentAspectRatio:NSMakeSize(VIEWPORT_WIDTH, VIEWPORT_HEIGHT)];

    self.gbView = [[GBView alloc] initWithFrame:frame];
    self.gbView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    win.contentView = self.gbView;

    [win center];
    return self;
}

/* --------------------------------------------------------------------- */

- (void)openROM:(NSString *)path {
    /* Save state + battery for whatever is currently running. */
    if (self.romLoaded && self.currentROMPath) {
        const char *old = self.currentROMPath.fileSystemRepresentation;
        if (ext_ram_dirty) sav_save(old);
        char ss[4096];
        savestate_default_path(old, ss, sizeof(ss));
        save_state(ss);
    }

    [self stopEmulation];

    const char *cpath = path.fileSystemRepresentation;

    /* Auto-restore from save state if one exists for this ROM. */
    char ss[4096];
    savestate_default_path(cpath, ss, sizeof(ss));
    BOOL restored = NO;
    if ([[NSFileManager defaultManager]
            fileExistsAtPath:[NSString stringWithUTF8String:ss]]) {
        pixels_init();
        restored = (BOOL)load_state(ss);
    }

    if (!restored) {
        cart_load((char *)cpath);
        mem_init();
        sav_load(cpath);
        gpu_init();
        cpu_fake_init();
        pixels_init();
        joypad_init();
    }

    self.currentROMPath = path;
    self.window.title = path.lastPathComponent.stringByDeletingPathExtension;

    self.romLoaded = YES;
    self.paused    = NO;
    [self syncPauseTitle];

    [[NSUserDefaults standardUserDefaults] setObject:path forKey:@"LastROMPath"];

    [self startEmulation];
}

- (void)startEmulation {
    if (self.frameTimer) return;
    self.frameTimer =
        [NSTimer timerWithTimeInterval:(1.0 / 60.0)
                                target:self
                              selector:@selector(frameStep)
                              userInfo:nil
                               repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:self.frameTimer
                              forMode:NSRunLoopCommonModes];
}

- (void)stopEmulation {
    [self.frameTimer invalidate];
    self.frameTimer = nil;
}

- (void)frameStep {
    if (!self.romLoaded || self.paused) return;
    int iters = g_fast_mode ? 4 : 1;
    for (int i = 0; i < iters; i++)
        frame_headless();
    [self.gbView setNeedsDisplay:YES];
}

/* --------------------------------------------------------------------- */

- (void)saveState {
    if (!self.romLoaded || !self.currentROMPath) return;
    char ss[4096];
    savestate_default_path(self.currentROMPath.fileSystemRepresentation,
                           ss, sizeof(ss));
    if (save_state(ss))
        NSLog(@"[savestate] saved → %s", ss);
    else
        NSLog(@"[savestate] save FAILED");
}

- (void)loadState {
    if (!self.currentROMPath) return;
    char ss[4096];
    savestate_default_path(self.currentROMPath.fileSystemRepresentation,
                           ss, sizeof(ss));
    if (load_state(ss)) {
        NSLog(@"[savestate] loaded ← %s", ss);
        if (savestate_rom_path[0] != '\0')
            self.currentROMPath =
                [NSString stringWithUTF8String:savestate_rom_path];
    } else {
        NSLog(@"[savestate] load FAILED (no state file?)");
    }
}

- (void)resetEmulator {
    if (!self.romLoaded) return;
    gb_reset();
}

- (void)togglePause {
    self.paused = !self.paused;
    [self syncPauseTitle];
}

- (void)syncPauseTitle {
    NSMenu *emul = [[[NSApp mainMenu] itemWithTitle:@"Emulation"] submenu];
    NSMenuItem *item = [emul itemWithTag:100];
    if (item) item.title = self.paused ? @"Resume" : @"Pause";
}

/* --------------------------------------------------------------------- */
/* NSWindowDelegate                                                        */
/* --------------------------------------------------------------------- */

- (void)windowWillClose:(NSNotification *)note {
    (void)note;
    [self stopEmulation];

    if (self.romLoaded && self.currentROMPath) {
        const char *cpath = self.currentROMPath.fileSystemRepresentation;
        if (ext_ram_dirty) sav_save(cpath);
        /* Auto-save emulator state on window close */
        char ss[4096];
        savestate_default_path(cpath, ss, sizeof(ss));
        save_state(ss);
    }

    cpu_close_debug_file();
    [NSApp terminate:nil];
}

@end  /* GBWindowController */


/* -----------------------------------------------------------------------
 * AppDelegate
 * -------------------------------------------------------------------- */

@interface AppDelegate : NSObject <NSApplicationDelegate, ROMOpener>
@property (nonatomic, strong) GBWindowController *wc;
@property (nonatomic, strong) NSMenu *openRecentMenu;
@end

@implementation AppDelegate

/* Central ROM-open: updates emulator + recent list. */
- (void)openROM:(NSString *)path {
    [self.wc openROM:path];
    [self trackRecentROM:path];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    [self buildMenuBar];

    self.wc = [[GBWindowController alloc] init];
    [self.wc showWindow:nil];
    [self.wc.window makeKeyAndOrderFront:nil];

    /* ROM from command line (e.g. "open CGB.app --args game.gb") */
    NSArray<NSString *> *args = [NSProcessInfo processInfo].arguments;
    for (NSUInteger i = 1; i < args.count; i++) {
        NSString *a = args[i];
        if (![a hasPrefix:@"-"]) {
            [self openROM:a];
            return;
        }
    }

    /* Restore last-opened ROM */
    NSString *last = [[NSUserDefaults standardUserDefaults]
                          stringForKey:@"LastROMPath"];
    if (last && [[NSFileManager defaultManager] fileExistsAtPath:last])
        [self openROM:last];
    /* else: empty window — user can File > Open ROM… */
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    (void)app;
    return YES;
}

/* Allows opening .gb files by double-clicking in Finder */
- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename {
    (void)app;
    [self openROM:filename];
    return YES;
}

/* -----------------------------------------------------------------------
 * Menu actions
 * -------------------------------------------------------------------- */

- (IBAction)openROMDialog:(id)sender {
    (void)sender;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.title                  = @"Open ROM";
    p.allowedFileTypes       = @[@"gb", @"gbc", @"rom"];
    p.canChooseFiles         = YES;
    p.canChooseDirectories   = NO;
    p.allowsMultipleSelection = NO;
    if ([p runModal] == NSModalResponseOK && p.URL)
        [self openROM:p.URL.path];
}

- (IBAction)saveStateAction:(id)sender { (void)sender; [self.wc saveState];       }
- (IBAction)loadStateAction:(id)sender { (void)sender; [self.wc loadState];       }
- (IBAction)resetAction:(id)sender     { (void)sender; [self.wc resetEmulator];   }
- (IBAction)togglePauseAction:(id)sender {
    (void)sender;
    [self.wc togglePause];
}

- (IBAction)fastForwardAction:(id)sender {
    (void)sender;
    g_fast_mode = !g_fast_mode;
    NSMenu *em = [[[NSApp mainMenu] itemWithTitle:@"Emulation"] submenu];
    [em itemWithTag:101].state =
        g_fast_mode ? NSControlStateValueOn : NSControlStateValueOff;
}

- (IBAction)openRecentROM:(id)sender {
    NSString *path = [(NSMenuItem *)sender representedObject];
    if ([[NSFileManager defaultManager] fileExistsAtPath:path])
        [self openROM:path];
    else
        NSLog(@"Recent ROM not found: %@", path);
}

- (IBAction)clearRecentROMs:(id)sender {
    (void)sender;
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"RecentROMs"];
    [self rebuildOpenRecentMenu];
}

- (void)trackRecentROM:(NSString *)path {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    NSArray<NSString *> *existing = [ud objectForKey:@"RecentROMs"] ?: @[];
    NSMutableArray *list = [existing mutableCopy];
    [list removeObject:path];
    [list insertObject:path atIndex:0];
    if (list.count > 10)
        [list removeObjectsInRange:NSMakeRange(10, list.count - 10)];
    [ud setObject:list forKey:@"RecentROMs"];
    [self rebuildOpenRecentMenu];
}

- (void)rebuildOpenRecentMenu {
    [self.openRecentMenu removeAllItems];
    NSArray<NSString *> *list =
        [[NSUserDefaults standardUserDefaults] objectForKey:@"RecentROMs"] ?: @[];
    if (list.count == 0) {
        NSMenuItem *empty = [[NSMenuItem alloc]
            initWithTitle:@"No Recent Items" action:nil keyEquivalent:@""];
        empty.enabled = NO;
        [self.openRecentMenu addItem:empty];
    } else {
        for (NSString *p in list) {
            NSString *name = p.lastPathComponent.stringByDeletingPathExtension;
            NSMenuItem *item = [[NSMenuItem alloc]
                initWithTitle:name
                       action:@selector(openRecentROM:)
                keyEquivalent:@""];
            item.representedObject = p;
            item.toolTip = p;
            [self.openRecentMenu addItem:item];
        }
        [self.openRecentMenu addItem:[NSMenuItem separatorItem]];
        [self.openRecentMenu addItemWithTitle:@"Clear Menu"
                                       action:@selector(clearRecentROMs:)
                                keyEquivalent:@""];
    }
}

- (IBAction)selectPaletteAction:(id)sender {
    NSMenuItem *item = (NSMenuItem *)sender;
    NSInteger idx = item.tag;

    NSMenu *pm = [[[NSApp mainMenu] itemWithTitle:@"Palette"] submenu];
    for (NSMenuItem *mi in pm.itemArray)
        mi.state = NSControlStateValueOff;

    palette_set((int)idx);
    item.state = NSControlStateValueOn;
    [[NSUserDefaults standardUserDefaults] setInteger:idx
                                               forKey:@"LastPaletteIndex"];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    SEL a = item.action;
    if (a == @selector(saveStateAction:) ||
        a == @selector(loadStateAction:) ||
        a == @selector(resetAction:)     ||
        a == @selector(togglePauseAction:) ||
        a == @selector(fastForwardAction:))
        return self.wc.romLoaded;
    return YES;
}

/* -----------------------------------------------------------------------
 * Menu construction (no NIB/storyboard)
 * -------------------------------------------------------------------- */

- (void)buildMenuBar {
    NSMenu *bar = [[NSMenu alloc] init];
    NSApp.mainMenu = bar;

    /* ---- ① Application menu ---------------------------------------- */
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"App"];
    appItem.submenu = appMenu;

    [appMenu addItemWithTitle:@"About Joe's Gameboy"
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *hide = [[NSMenuItem alloc]
        initWithTitle:@"Hide Joe's Gameboy"
               action:@selector(hide:)
        keyEquivalent:@"h"];
    [appMenu addItem:hide];

    NSMenuItem *hideOthers = [[NSMenuItem alloc]
        initWithTitle:@"Hide Others"
               action:@selector(hideOtherApplications:)
        keyEquivalent:@"h"];
    hideOthers.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [appMenu addItem:hideOthers];

    [appMenu addItemWithTitle:@"Show All"
                       action:@selector(unhideAllApplications:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit Joe's Gameboy"
                       action:@selector(terminate:)
                keyEquivalent:@"q"];

    /* ---- ② File menu ----------------------------------------------- */
    NSMenuItem *fileItem = [[NSMenuItem alloc]
        initWithTitle:@"File" action:nil keyEquivalent:@""];
    [bar addItem:fileItem];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    fileItem.submenu = fileMenu;

    [fileMenu addItemWithTitle:@"Open ROM\u2026"
                        action:@selector(openROMDialog:)
                 keyEquivalent:@"o"];

    /* Open Recent submenu */
    self.openRecentMenu = [[NSMenu alloc] initWithTitle:@"Open Recent"];
    NSMenuItem *recentItem = [[NSMenuItem alloc]
        initWithTitle:@"Open Recent" action:nil keyEquivalent:@""];
    recentItem.submenu = self.openRecentMenu;
    [fileMenu addItem:recentItem];
    [self rebuildOpenRecentMenu];

    [fileMenu addItem:[NSMenuItem separatorItem]];

    [fileMenu addItemWithTitle:@"Save State"
                        action:@selector(saveStateAction:)
                 keyEquivalent:@"s"];
    [fileMenu addItemWithTitle:@"Load State"
                        action:@selector(loadStateAction:)
                 keyEquivalent:@"l"];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [fileMenu addItemWithTitle:@"Reset"
                        action:@selector(resetAction:)
                 keyEquivalent:@"r"];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [fileMenu addItemWithTitle:@"Quit"
                        action:@selector(terminate:)
                 keyEquivalent:@"q"];

    /* ---- ③ Emulation menu ------------------------------------------ */
    NSMenuItem *emulItem = [[NSMenuItem alloc]
        initWithTitle:@"Emulation" action:nil keyEquivalent:@""];
    [bar addItem:emulItem];
    NSMenu *emulMenu = [[NSMenu alloc] initWithTitle:@"Emulation"];
    emulItem.submenu = emulMenu;

    NSMenuItem *pauseItem = [[NSMenuItem alloc]
        initWithTitle:@"Pause"
               action:@selector(togglePauseAction:)
        keyEquivalent:@"p"];
    pauseItem.tag = 100;
    [emulMenu addItem:pauseItem];

    NSMenuItem *ffItem = [[NSMenuItem alloc]
        initWithTitle:@"Fast Forward"
               action:@selector(fastForwardAction:)
        keyEquivalent:@"f"];
    ffItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    ffItem.tag = 101;
    [emulMenu addItem:ffItem];

    /* ---- ④ View menu ----------------------------------------------- */
    NSMenuItem *viewItem = [[NSMenuItem alloc]
        initWithTitle:@"View" action:nil keyEquivalent:@""];
    [bar addItem:viewItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    viewItem.submenu = viewMenu;

    NSMenuItem *fsItem = [[NSMenuItem alloc]
        initWithTitle:@"Enter Full Screen"
               action:@selector(toggleFullScreen:)
        keyEquivalent:@"f"];
    fsItem.keyEquivalentModifierMask =
        NSEventModifierFlagControl | NSEventModifierFlagCommand;
    [viewMenu addItem:fsItem];

    /* ---- ⑤ Palette menu -------------------------------------------- */
    NSMenuItem *palItem = [[NSMenuItem alloc]
        initWithTitle:@"Palette" action:nil keyEquivalent:@""];
    [bar addItem:palItem];
    NSMenu *palMenu = [[NSMenu alloc] initWithTitle:@"Palette"];
    palItem.submenu = palMenu;

    /* Restore last-used palette */
    NSInteger savedIdx = [[NSUserDefaults standardUserDefaults]
                              integerForKey:@"LastPaletteIndex"];
    if (savedIdx >= 0 && savedIdx < (NSInteger)GB_PALETTE_COUNT)
        palette_set((int)savedIdx);
    int current = palette_get();

    for (int i = 0; i < GB_PALETTE_COUNT; i++) {
        NSString *name = [NSString stringWithUTF8String:GB_PALETTES[i].name];
        NSMenuItem *pi = [[NSMenuItem alloc]
            initWithTitle:name
                   action:@selector(selectPaletteAction:)
            keyEquivalent:@""];
        pi.tag   = (NSInteger)i;
        pi.state = (i == current) ? NSControlStateValueOn
                                  : NSControlStateValueOff;
        [palMenu addItem:pi];
    }

    /* ---- ⑥ Window menu --------------------------------------------- */
    NSMenuItem *winItem = [[NSMenuItem alloc]
        initWithTitle:@"Window" action:nil keyEquivalent:@""];
    [bar addItem:winItem];
    NSMenu *winMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    winItem.submenu = winMenu;
    [NSApp setWindowsMenu:winMenu];

    [winMenu addItemWithTitle:@"Minimize"
                       action:@selector(performMiniaturize:)
                keyEquivalent:@"m"];
    [winMenu addItemWithTitle:@"Zoom"
                       action:@selector(performZoom:)
                keyEquivalent:@""];
    [winMenu addItem:[NSMenuItem separatorItem]];
    [winMenu addItemWithTitle:@"Bring All to Front"
                       action:@selector(arrangeInFront:)
                keyEquivalent:@""];
}

@end  /* AppDelegate */


/* -----------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------- */
int main(int argc, const char *argv[]) {
    (void)argc; (void)argv;
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
