/*
 * host_menu_mac.m - the macOS menu bar: Controls, Aim, Sound, Language.
 *
 * Added beside the application and Window menus SDL already made. Every item
 * reads and writes the player's settings (host_settings.c); remapping and aim
 * calibration hand over to host_sdl.c, which takes the next key, click or
 * sight position from the game window. Titles are rebuilt each time a menu
 * opens, so they always show the current bindings.
 */

#import <Cocoa/Cocoa.h>
#include <OpenGL/OpenGL.h>
#include <stdio.h>
#include <SDL.h>
#include <SDL_syswm.h>

#include "host_settings.h"

void host_audio_toggle_mute(void);

@interface LindberghMenu : NSObject <NSMenuDelegate>
@end

static LindberghMenu *g_target;
static NSMenu *g_controls, *g_aim, *g_sound, *g_lang;

static NSMenuItem *item(NSMenu *m, NSString *title, SEL sel, NSInteger tag)
{
    NSMenuItem *i = [[NSMenuItem alloc] initWithTitle:title action:sel keyEquivalent:@""];
    i.target = g_target;
    i.tag = tag;
    [m addItem:i];
    return i;
}

static NSString *binding_title(HostAction a)
{
    char nm[64];
    settings_binding_name(settings()->bind[a], nm, sizeof nm);
    return [NSString stringWithFormat:@"%s:  %s", settings_action_name(a), nm];
}

@implementation LindberghMenu

- (void)menuNeedsUpdate:(NSMenu *)menu
{
    HostSettings *st = settings();
    [menu removeAllItems];
    if (menu == g_controls) {
        for (int a = 0; a < ACT_COUNT; a++) {
            if (a == ACT_COIN2) [menu addItem:[NSMenuItem separatorItem]];
            item(menu, binding_title((HostAction)a), @selector(rebind:), a);
        }
        [menu addItem:[NSMenuItem separatorItem]];
        item(menu, @"Reload When the Pointer Leaves the Window", @selector(toggleLeaveReload:), 0).state =
            st->leave_reload ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:[NSMenuItem separatorItem]];
        item(menu, @"Reset Controls to Defaults", @selector(resetControls:), 0);
    } else if (menu == g_aim) {
        item(menu, @"Calibrate Aim…", @selector(calibrate:), 0);
        item(menu, @"Reset Aim Calibration", @selector(resetAim:), 0);
        NSMenuItem *info = item(menu, [NSString stringWithFormat:@"Current: x × %.3f %+.1f,  y × %.3f %+.1f",
                                       st->aim_sx, st->aim_ox, st->aim_sy, st->aim_oy], nil, 0);
        info.enabled = NO;
    } else if (menu == g_sound) {
        item(menu, @"Mute", @selector(toggleMute:), 0).state = st->muted ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:[NSMenuItem separatorItem]];
        for (int v = 25; v <= 100; v += 25)
            item(menu, [NSString stringWithFormat:@"Volume %d%%", v], @selector(setVolume:), v).state =
                st->volume == v ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (menu == g_lang) {
        item(menu, @"English", @selector(setLanguage:), 1).state = st->country ? NSControlStateValueOn : NSControlStateValueOff;
        item(menu, @"Japanese", @selector(setLanguage:), 0).state = st->country ? NSControlStateValueOff : NSControlStateValueOn;
        [menu addItem:[NSMenuItem separatorItem]];
        item(menu, @"Takes effect when the game next starts", nil, 0).enabled = NO;
    }
}

- (BOOL)validateMenuItem:(NSMenuItem *)i { return i.action != nil; }

- (void)rebind:(NSMenuItem *)i       { host_begin_capture((HostAction)i.tag); }
- (void)toggleLeaveReload:(id)s      { settings()->leave_reload = !settings()->leave_reload; settings_save(); }
- (void)resetControls:(id)s          { settings_reset_controls(); settings_save(); }
- (void)calibrate:(id)s              { host_begin_aim_calibration(); }
- (void)resetAim:(id)s               { settings_reset_aim(); settings_save(); }
- (void)toggleMute:(id)s             { host_audio_toggle_mute(); }
- (void)setVolume:(NSMenuItem *)i    { settings()->volume = (int)i.tag; settings_save(); }
- (void)setLanguage:(NSMenuItem *)i  { settings()->country = (int)i.tag; settings_save(); }

@end

static NSMenu *add_menu(NSString *title)
{
    NSMenu *m = [[NSMenu alloc] initWithTitle:title];
    m.delegate = g_target;
    m.autoenablesItems = YES;
    NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    top.submenu = m;
    NSMenu *bar = NSApp.mainMenu;
    /* before SDL's Window menu, which is last */
    NSInteger at = bar.numberOfItems > 1 ? bar.numberOfItems - 1 : bar.numberOfItems;
    [bar insertItem:top atIndex:at];
    return m;
}

/* SDL builds its own menu bar when Cocoa finishes launching, which can come
 * after the window is open and replaces whatever was there. So this is
 * called every so often and puts the menus back if they are gone. */
void host_menu_install(void)
{
    @autoreleasepool {
        if (!NSApp) return;
        if (!NSApp.mainMenu) NSApp.mainMenu = [[NSMenu alloc] init];
        if ([NSApp.mainMenu indexOfItemWithTitle:@"Controls"] >= 0) return;
        if (!g_target) g_target = [[LindberghMenu alloc] init];
        g_controls = add_menu(@"Controls");
        g_aim      = add_menu(@"Aim");
        g_sound    = add_menu(@"Sound");
        g_lang     = add_menu(@"Language");
        NSMutableArray *names = [NSMutableArray array];
        for (NSMenuItem *i in NSApp.mainMenu.itemArray) [names addObject:i.title.length ? i.title : @"(app)"];
        fprintf(stderr, "[menu] menu bar: %s\n", [[names componentsJoinedByString:@", "] UTF8String]);
    }
}

/* The game draws at its monitor's size whatever the window is: the current
 * GL context's drawable is pinned at w x h (a fixed surface backing size) and
 * the system scales it to the window as it presents. The window keeps the
 * game's shape when resized, so the picture is never stretched. */
void host_mac_fix_surface(SDL_Window *win, int w, int h)
{
    CGLContextObj ctx = CGLGetCurrentContext();
    if (ctx) {
        GLint size[2] = { w, h };
        CGLError e1 = CGLSetParameter(ctx, kCGLCPSurfaceBackingSize, size);
        CGLError e2 = CGLEnable(ctx, kCGLCESurfaceBackingSize);
        fprintf(stderr, "[window] drawable fixed at %dx%d%s\n", w, h, e1 || e2 ? " (refused)" : "");
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(win, &info) && info.subsystem == SDL_SYSWM_COCOA)
        [info.info.cocoa.window setContentAspectRatio:NSMakeSize(w, h)];
}
