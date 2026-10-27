/*
 * host_sdl.c - the host window on a POSIX host, through SDL2.
 *
 * The Windows build has this in hle_window.c, around a Win32 window and WGL.
 * The contract is the same: one window, one GL context, an input queue the
 * GLUT seam drains, the cabinet's switches and gun, and host_swap() at the end
 * of every frame. Keys are reported as Win32 virtual-key codes so the GLUT
 * seam is shared unchanged.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>

/* A bare executable (not a .app) started from a script is not made the active
 * application, so its window shows but keystrokes keep going to the terminal.
 * Register as a regular app and take focus. */
void host_menu_install(void);    /* host_menu_mac.m */

static void mac_activate(void)
{
    id app = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSApplication"),
                                                sel_registerName("sharedApplication"));
    ((void (*)(id, SEL, long))objc_msgSend)(app, sel_registerName("setActivationPolicy:"), 0);
    ((void (*)(id, SEL, BOOL))objc_msgSend)(app, sel_registerName("activateIgnoringOtherApps:"), YES);
}
#endif

#include "gl_posix.h"
#include "host_settings.h"

#define DEFAULT_W 640
#define DEFAULT_H 480

unsigned g_frame;

static struct {
    SDL_Window   *win;
    SDL_GLContext gl;
    int           w, h;
} g_win;

static int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    return (v && *v) ? atoi(v) : dflt;
}

void lindbergh_window_size(int *w, int *h)
{
    if (w) *w = g_win.w ? g_win.w : env_int("LINDBERGH_WIDTH", DEFAULT_W);
    if (h) *h = g_win.h ? g_win.h : env_int("LINDBERGH_HEIGHT", DEFAULT_H);
}

/* ---- input queue ---- */
#define INQ 256
static HostInput g_inq[INQ];
static unsigned  g_in_head, g_in_tail;
static int       g_mouse_x, g_mouse_y, g_buttons;

static void in_push(int kind, int a, int b, int cc, int d)
{
    unsigned next = (g_in_head + 1) % INQ;
    if (next == g_in_tail) return;
    HostInput *e = &g_inq[g_in_head];
    e->kind = kind; e->a = a; e->b = b; e->c = cc; e->d = d;
    g_in_head = next;
}

int host_input_pop(HostInput *out)
{
    if (g_in_tail == g_in_head) return 0;
    *out = g_inq[g_in_tail];
    g_in_tail = (g_in_tail + 1) % INQ;
    return 1;
}

void host_mouse_state(int *x, int *y, int *buttons)
{
    if (x) *x = g_mouse_x;
    if (y) *y = g_mouse_y;
    if (buttons) *buttons = g_buttons;
}

/* ---- the cabinet ----
 * Every key and mouse button goes through the bindings in host_settings.c;
 * what each action does to the cabinet is here. */
static CabinetInput g_cab;
static unsigned g_held;       /* cabinet buttons held down right now */
/* Presses not yet seen by the I/O board. Events are drained once a frame and
 * the board is read once a frame, so a quick tap can go down and up inside
 * one drain; latching it until the next read means no tap is lost. */
static unsigned g_cab_latch;
static unsigned g_auto;       /* buttons driven by LINDBERGH_AUTOSTART/AUTOGUN */
static int      g_scripted;   /* the gun is scripted: real input is ignored */

static int g_capture = -1;    /* binding the next key or click, for this action */
static int g_calib;           /* aim calibration step, 0 = off */
static double g_calib_sx, g_calib_sy;     /* where the sight was clicked, step 1 */
static int g_pointer_in = 1;

static const unsigned CAB_OF[ACT_COUNT] = {
    [ACT_START1] = CAB_P1_START, [ACT_START2] = CAB_P2_START,
    [ACT_TRIGGER] = CAB_P1_TRIGGER, [ACT_RELOAD] = CAB_P1_RELOAD, [ACT_ACTION] = CAB_P1_ACTION,
    [ACT_FIREMODE] = CAB_P1_FIREMODE,
    [ACT_TEST] = CAB_TEST, [ACT_SERVICE] = CAB_SERVICE,
};

static void set_title(const char *msg);
static int g_capture_or_calib(void);

static void cabinet_action(HostAction a, int down)
{
    switch (a) {
    case ACT_COIN1: if (down) g_cab.coins[0]++; return;
    case ACT_COIN2: if (down) g_cab.coins[1]++; return;
    case ACT_MUTE:  if (down) host_audio_toggle_mute(); return;
    default: break;
    }
    unsigned bit = CAB_OF[a];
    if (!bit) return;
    if (down) { g_held |= bit; g_cab_latch |= bit; } else g_held &= ~bit;
}

/* A key (SDL keycode) or mouse button (MOUSE_BIND) went down or up. Returns
 * whether something was bound to it. */
static int cabinet_input(int bind, int down)
{
    HostSettings *st = settings();
    int hit = 0;
    for (int a = 0; a < ACT_COUNT; a++)
        if (st->bind[a] == bind) { cabinet_action((HostAction)a, down); hit = 1; }
    return hit;
}

/* ---- remapping and calibration, started from the menus ---- */
void host_begin_capture(HostAction a)
{
    g_calib = 0;
    g_capture = (int)a;
    char t[160];
    snprintf(t, sizeof t, "Press a key or mouse button for \"%s\" (Esc cancels)", settings_action_name(a));
    set_title(t);
}

static void finish_capture(int bind)
{
    HostSettings *st = settings();
    if (bind) {
        for (int a = 0; a < ACT_COUNT; a++)       /* one action per input */
            if (st->bind[a] == bind) st->bind[a] = 0;
        st->bind[g_capture] = bind;
        settings_save();
        char nm[64]; settings_binding_name(bind, nm, sizeof nm);
        fprintf(stderr, "[settings] %s -> %s\n", settings_action_name((HostAction)g_capture), nm);
    }
    g_capture = -1;
    g_held = 0;
    set_title(NULL);
}

/* Aim calibration: the gun is held at two known points in turn and the player
 * clicks where the game draws its sight; the two pairs give scale and offset
 * per axis. Points are in window pixels, before the correction. */
#define CAL_AX 0.30
#define CAL_BX 0.70
#define CAL_AY 0.30
#define CAL_BY 0.70

void host_begin_aim_calibration(void)
{
    g_capture = -1;
    g_calib = 1;
    set_title("Aim calibration 1/2: click the centre of the game's sight (Esc cancels)");
}

static void calib_click(int x, int y)
{
    double w = g_win.w ? g_win.w : DEFAULT_W, h = g_win.h ? g_win.h : DEFAULT_H;
    if (g_calib == 1) {
        g_calib_sx = x; g_calib_sy = y; g_calib = 2;
        set_title("Aim calibration 2/2: click the centre of the game's sight again (Esc cancels)");
        return;
    }
    HostSettings *st = settings();
    double ax = CAL_AX * w, bx = CAL_BX * w, ay = CAL_AY * h, by = CAL_BY * h;
    double sx = (x - g_calib_sx) / (bx - ax), sy = (y - g_calib_sy) / (by - ay);
    if (sx > 0.5 && sx < 2.0 && sy > 0.5 && sy < 2.0) {
        st->aim_sx = sx; st->aim_ox = g_calib_sx - sx * ax;
        st->aim_sy = sy; st->aim_oy = g_calib_sy - sy * ay;
        settings_save();
        fprintf(stderr, "[settings] aim calibrated: %.3fx%+.1f, %.3fy%+.1f\n", st->aim_sx, st->aim_ox, st->aim_sy, st->aim_oy);
    } else {
        fprintf(stderr, "[settings] aim calibration rejected (scale %.2f, %.2f) - kept the old one\n", sx, sy);
    }
    g_calib = 0;
    set_title(NULL);
}

/* LINDBERGH_AUTOSTART=1: a coin at frame 240, then start pressed every two
 * seconds - gets past attract mode with nobody at the keyboard. */
static void cabinet_autostart(void)
{
    static int on = -1, coined;
    if (on < 0) on = env_int("LINDBERGH_AUTOSTART", 0) != 0;
    if (!on) return;
    if (!coined && g_frame > 240) {
        coined = 1;
        g_cab.coins[0]++;
        fprintf(stderr, "[cab] autostart: coin inserted at frame %u\n", g_frame);
    }
    if (coined) {
        if (g_frame % 120u < 20u) g_auto |= CAB_P1_START;
        else                      g_auto &= ~CAB_P1_START;
    }
    /* LINDBERGH_AUTOGUN=1 as well: the gun circles the screen and fires in
     * half-second bursts, to check aim and trigger without a mouse */
    static int gun = -1;
    if (gun < 0) gun = env_int("LINDBERGH_AUTOGUN", 0);
    if (gun && coined) {
        g_scripted = 1;
        double t = g_frame / 90.0;
        int w = g_win.w ? g_win.w : DEFAULT_W, h = g_win.h ? g_win.h : DEFAULT_H;
        int ax, ay;     /* LINDBERGH_AIM=x,y holds the gun still instead */
        const char *aim = getenv("LINDBERGH_AIM");
        if (aim && sscanf(aim, "%d,%d", &ax, &ay) == 2) { g_mouse_x = ax; g_mouse_y = ay; }
        else {
            g_mouse_x = (int)(w / 2 + w / 3 * cos(t));
            g_mouse_y = (int)(h / 2 + h / 3 * sin(t));
        }
        g_auto &= ~(CAB_P1_TRIGGER | CAB_P1_RELOAD);
        if (g_frame % 60u < 30u) g_auto |= CAB_P1_TRIGGER;
        /* LINDBERGH_AUTOPRESS=<cab bits, hex>: also press those for a quarter
         * second every two seconds - for finding what a button does */
        static int press = -1;
        if (press < 0) { const char *v = getenv("LINDBERGH_AUTOPRESS"); press = v ? (int)strtol(v, NULL, 16) : 0; }
        g_auto &= ~(unsigned)press;
        if (press && g_frame % 120u >= 60u && g_frame % 120u < 75u) g_auto |= (unsigned)press;
        /* LINDBERGH_AUTOGUN=2: one second in four off the screen, with a
         * press of the gun's screen-out signal - the arcade's reload */
        if (gun == 2 && g_frame % 240u < 60u) {
            g_mouse_x = -100000; g_mouse_y = -100000;
            g_auto &= ~CAB_P1_TRIGGER;
            if (g_frame % 240u >= 20u && g_frame % 240u < 30u) g_auto |= CAB_P1_RELOAD;
        }
    }
}

void host_cabinet_input(CabinetInput *out)
{
    cabinet_autostart();
    HostSettings *st = settings();
    double w = g_win.w ? g_win.w : DEFAULT_W, h = g_win.h ? g_win.h : DEFAULT_H;
    if (g_calib == 1)      { g_cab.gun_x = (int)(CAL_AX * w); g_cab.gun_y = (int)(CAL_AY * h); }
    else if (g_calib == 2) { g_cab.gun_x = (int)(CAL_BX * w); g_cab.gun_y = (int)(CAL_BY * h); }
    else {
        /* undo the game's own calibration, so the sight is under the pointer */
        g_cab.gun_x = (int)((g_mouse_x - st->aim_ox) / st->aim_sx);
        g_cab.gun_y = (int)((g_mouse_y - st->aim_oy) / st->aim_sy);
    }
    g_cab.screen_w = (int)w;
    g_cab.screen_h = (int)h;
    g_cab.buttons = g_auto | (g_scripted ? 0 : g_held);
    *out = g_cab;
    if (!g_scripted) out->buttons |= g_cab_latch;
    {   /* What the board reports, whenever it changes - and the gun, at most
         * once a second - so a report of "nothing registers" can be checked
         * against what the game was actually given. */
        static unsigned lb = ~0u, lc0 = ~0u, n; static int lx = -1, ly = -1;
        if (out->buttons != lb || out->coins[0] != lc0) {
            fprintf(stderr, "[cab] frame %u: buttons %#x, coins %u\n", g_frame, out->buttons, out->coins[0]);
            lb = out->buttons; lc0 = out->coins[0];
        }
        if (++n % 60 == 0 && (out->gun_x != lx || out->gun_y != ly)) {
            fprintf(stderr, "[cab] frame %u: gun %d,%d (pointer %d,%d)\n", g_frame, out->gun_x, out->gun_y, g_mouse_x, g_mouse_y);
            lx = out->gun_x; ly = out->gun_y;
        }
    }
}

/* The board has reported the switches: latched taps have been seen. */
void host_cabinet_switches_read(void)
{
    g_cab_latch = 0;
}

/* SDL keysym -> Win32 virtual key: letters upper case, digits as themselves. */
static int vk_of(SDL_Keycode k)
{
    if (k >= 'a' && k <= 'z') return k - 'a' + 'A';
    if (k >= ' ' && k < 0x7F) return k;
    if (k >= SDLK_F1 && k <= SDLK_F12) return VK_F1 + (k - SDLK_F1);
    switch (k) {
    case SDLK_LEFT:  return VK_LEFT;
    case SDLK_RIGHT: return VK_RIGHT;
    case SDLK_UP:    return VK_UP;
    case SDLK_DOWN:  return VK_DOWN;
    case SDLK_ESCAPE: return VK_ESCAPE;
    case SDLK_RETURN: return '\r';
    }
    return 0;
}

static int button_of(Uint8 b)
{
    return b == SDL_BUTTON_LEFT ? 0 : b == SDL_BUTTON_MIDDLE ? 1 : 2;
}

/* LINDBERGH_KEYS="frame:key,frame:key...": push real SDL key events - a
 * press at that frame, the release in the same drain (a fast tap) - so the
 * keyboard path is tested exactly as a person drives it. "!" is the pointer
 * leaving the window and coming back. */
static void fake_keys(void)
{
    static const char *v = (const char *)1;
    static unsigned done = ~0u;              /* events drain more than once a frame */
    if (v == (const char *)1) v = getenv("LINDBERGH_KEYS");
    if (!v || done == g_frame) return;
    done = g_frame;
    for (const char *p = v; p && *p; ) {
        unsigned f = (unsigned)strtoul(p, (char **)&p, 10);
        if (*p++ != ':' || !*p) break;
        char k = *p++;
        if (f == g_frame) {
            SDL_Event e; memset(&e, 0, sizeof e);
            if (k == '!') {                  /* the pointer leaves the window */
                e.type = SDL_WINDOWEVENT; e.window.event = SDL_WINDOWEVENT_LEAVE;
                SDL_PushEvent(&e);
                e.window.event = SDL_WINDOWEVENT_ENTER; SDL_PushEvent(&e);
                fprintf(stderr, "[keys] pointer out and back at frame %u\n", f);
                while (*p == ',') p++;
                continue;
            }
            e.type = SDL_KEYDOWN; e.key.keysym.sym = (SDL_Keycode)(k >= 'A' && k <= 'Z' ? k + 32 : k);
            SDL_PushEvent(&e);
            e.type = SDL_KEYUP; SDL_PushEvent(&e);
            fprintf(stderr, "[keys] tapped %c at frame %u\n", k, f);
        }
        while (*p == ',') p++;
    }
}

static int g_capture_or_calib(void) { return g_capture >= 0 || g_calib; }

static char g_title[128] = "lindberghrecomp";
static void set_title(const char *msg)
{
    if (g_win.win) SDL_SetWindowTitle(g_win.win, msg ? msg : g_title);
}

static void pump(void)
{
    SDL_Event e;
    fake_keys();
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            fprintf(stderr, "[window] closed by the user\n");
            exit(0);
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) fprintf(stderr, "[window] focus gained\n");
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)   fprintf(stderr, "[window] focus lost\n");
            if (e.window.event == SDL_WINDOWEVENT_ENTER) { fprintf(stderr, "[window] pointer entered\n"); g_pointer_in = 1; }
            if (e.window.event == SDL_WINDOWEVENT_LEAVE) {
                fprintf(stderr, "[window] pointer left\n");
                /* Pointing a real gun off the screen is the reload; the
                 * nearest thing a mouse can do is leave the window. */
                if (g_pointer_in && settings()->leave_reload && !g_capture_or_calib()) g_cab_latch |= CAB_P1_RELOAD;
                g_pointer_in = 0;
            }
            break;
        case SDL_MOUSEMOTION:
            g_mouse_x = e.motion.x;
            g_mouse_y = e.motion.y;
            in_push(HOST_IN_MOTION, g_mouse_x, g_mouse_y, g_buttons, 0);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int b = button_of(e.button.button), down = e.type == SDL_MOUSEBUTTONDOWN;
            g_mouse_x = e.button.x;
            g_mouse_y = e.button.y;
            if (down && g_capture >= 0) { finish_capture(MOUSE_BIND(e.button.button)); break; }
            if (down && g_calib) { calib_click(e.button.x, e.button.y); break; }
            if (down) g_buttons |= 1 << b; else g_buttons &= ~(1 << b);
            cabinet_input(MOUSE_BIND(e.button.button), down);
            in_push(HOST_IN_MOUSE, b, down ? 0 : 1, g_mouse_x, g_mouse_y);
            break;
        }
        case SDL_KEYDOWN: {
            if (e.key.repeat) break;
            int vk = vk_of(e.key.keysym.sym);
            fprintf(stderr, "[key] down %s (vk %#x)\n", SDL_GetKeyName(e.key.keysym.sym), vk);
            if (g_capture >= 0 || g_calib) {
                if (e.key.keysym.sym == SDLK_ESCAPE) { g_capture = -1; g_calib = 0; set_title(NULL); }
                else if (g_capture >= 0) finish_capture((int)e.key.keysym.sym);
                break;
            }
            if (vk == VK_ESCAPE) {
                fprintf(stderr, "[window] Escape pressed - quitting\n");
                exit(0);
            }
            cabinet_input((int)e.key.keysym.sym, 1);
            if (!vk) break;
            if (e.key.keysym.sym < 0x80)
                in_push(HOST_IN_KEY, (int)e.key.keysym.sym, 1, g_mouse_x, g_mouse_y);
            else
                in_push(HOST_IN_SPECIAL, vk, 1, g_mouse_x, g_mouse_y);
            break;
        }
        case SDL_KEYUP: {
            int vk = vk_of(e.key.keysym.sym);
            cabinet_input((int)e.key.keysym.sym, 0);
            if (!vk) break;
            in_push(HOST_IN_KEYUP, vk, 0, g_mouse_x, g_mouse_y);
            break;
        }
        }
    }
}

static int g_hint_w, g_hint_h;

void host_window_hint(int w, int h)
{
    if (w > 0 && h > 0 && !g_win.win) { g_hint_w = w; g_hint_h = h; }
}

int host_window_open(const char *title)
{
    if (title && *title) snprintf(g_title, sizeof g_title, "%s", title);
    if (g_win.win) {
        if (title && *title) SDL_SetWindowTitle(g_win.win, title);
        return 1;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[window] SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    g_win.w = env_int("LINDBERGH_WIDTH", g_hint_w ? g_hint_w : DEFAULT_W);
    g_win.h = env_int("LINDBERGH_HEIGHT", g_hint_h ? g_hint_h : DEFAULT_H);

    /* The game is a 2007 fixed-function + ARB-program GL program: a 2.1
     * compatibility context is what it was written for. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    g_win.win = SDL_CreateWindow(title && *title ? title : "lindberghrecomp",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 g_win.w, g_win.h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!g_win.win) {
        fprintf(stderr, "[window] SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }
#if defined(__APPLE__)
    mac_activate();
    host_menu_install();
#endif
    SDL_RaiseWindow(g_win.win);
    fprintf(stderr, "[window] %dx%d\n", g_win.w, g_win.h);
    return 1;
}

int host_gl_context(void)
{
    if (!g_win.win && !host_window_open(NULL)) return 0;
    if (g_win.gl) return 1;
    g_win.gl = SDL_GL_CreateContext(g_win.win);
    if (!g_win.gl) {
        fprintf(stderr, "[window] SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_SetSwapInterval(env_int("LINDBERGH_VSYNC", 0));   /* the game paces itself; macOS stalls vsync swaps for hidden windows */
    fprintf(stderr, "[window] GL context ready: %s / %s\n",
            (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    return 1;
}

void host_pump(void) { pump(); }

/* LINDBERGH_SHOT=file.ppm writes the frame numbered LINDBERGH_SHOT_FRAME (or
 * every LINDBERGH_SHOT_EVERY frames, numbered); LINDBERGH_EXIT_AFTER=N exits
 * cleanly after N frames. Both exist to check a run without watching it. */
/* LINDBERGH_PEEK=addr,len[,every]: hex dump of guest memory each `every`
 * frames (default 30), with the cabinet's buttons - for finding where the
 * game keeps what it made of its inputs. */
static void maybe_peek(void)
{
    static int on = -1; static uint32_t addr, len, every;
    if (on < 0) {
        const char *v = getenv("LINDBERGH_PEEK");
        on = v && *v;
        if (on) { char *e; addr = (uint32_t)strtoul(v, &e, 16); len = *e ? (uint32_t)strtoul(e + 1, &e, 0) : 16;
                  every = *e ? (uint32_t)strtoul(e + 1, NULL, 0) : 30; if (len > 256) len = 256; if (!every) every = 30; }
    }
    if (!on || g_frame % every) return;
    char line[1024]; int at = snprintf(line, sizeof line, "[peek] f%u btn %#x:", g_frame, g_cab.buttons);
    for (uint32_t i = 0; i < len && at < 1000; i++)
        at += snprintf(line + at, sizeof line - at, "%s%02x", i % 4 ? "" : " ", ((unsigned char *)G2H(addr))[i]);
    fprintf(stderr, "%s\n", line);
}

static void maybe_screenshot(void)
{
    const char *path = getenv("LINDBERGH_SHOT");
    if (!path || !*path) return;
    int every = env_int("LINDBERGH_SHOT_EVERY", 0);
    int at = env_int("LINDBERGH_SHOT_FRAME", 600);
    char name[512];
    if (every > 0) {
        if (g_frame == 0 || g_frame % (unsigned)every) return;
        snprintf(name, sizeof name, "%s.%05u.ppm", path, g_frame);
    } else {
        if (g_frame != (unsigned)at) return;
        snprintf(name, sizeof name, "%s", path);
    }
    int w = g_win.w, h = g_win.h;
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 3);
    if (!px) return;
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
    FILE *f = fopen(name, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fclose(f);
        fprintf(stderr, "[shot] frame %u -> %s\n", g_frame, name);
    }
    free(px);
}

void host_swap(void)
{
    maybe_screenshot();
    maybe_peek();
    g_frame++;
    if (g_win.win) {
        /* Present the window's own framebuffer whatever the game left bound:
         * it ends a frame with one of its offscreen FBOs current, which the
         * Lindbergh's driver ignored at swap and Apple's does not. */
        GLint fb = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
        if (fb) glBindFramebuffer(GL_FRAMEBUFFER, 0);
        SDL_GL_SwapWindow(g_win.win);
        if (fb) glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
    }
    {
        static int after = -1;
        if (after < 0) after = env_int("LINDBERGH_EXIT_AFTER", 0);
        if (after && g_frame >= (unsigned)after) {
            fprintf(stderr, "[window] LINDBERGH_EXIT_AFTER: %u frames done\n", g_frame);
            exit(0);
        }
    }
    {   /* frame rate, every 5 seconds */
        static Uint32 t0; static unsigned f0;
        Uint32 now = SDL_GetTicks();
        if (!t0) { t0 = now; f0 = g_frame; }
        else if (now - t0 >= 5000) {
            fprintf(stderr, "[perf] frame %u, %.1f fps\n", g_frame,
                    (g_frame - f0) * 1000.0 / (now - t0));
            t0 = now; f0 = g_frame;
        }
    }
#if defined(__APPLE__)
    if (g_frame % 60u == 0) host_menu_install();
#endif
    pump();
}

/* This game opens its window through GLUT; the X11/GLX seam that a GLX title
 * needs lives in hle_window.c and is Windows-only so far. */
void hle_register_window(void) { }
