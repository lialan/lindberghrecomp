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
static void mac_activate(void)
{
    id app = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSApplication"),
                                                sel_registerName("sharedApplication"));
    ((void (*)(id, SEL, long))objc_msgSend)(app, sel_registerName("setActivationPolicy:"), 0);
    ((void (*)(id, SEL, BOOL))objc_msgSend)(app, sel_registerName("activateIgnoringOtherApps:"), YES);
}
#endif

#include "gl_posix.h"

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

/* ---- the cabinet ---- */
static CabinetInput g_cab;
/* Presses not yet seen by the I/O board. Events are drained once a frame and
 * the board is read once a frame, so a quick tap can go down and up inside
 * one drain; latching it until the next read means no tap is lost. */
static unsigned g_cab_latch;
static int      g_mouse_latch;

static void cabinet_key(int vk, int down)
{
    unsigned bit = 0;
    switch (vk) {
    case '5': if (down) g_cab.coins[0]++; return;
    case '6': if (down) g_cab.coins[1]++; return;
    case '1': bit = CAB_P1_START;   break;
    case '2': bit = CAB_P2_START;   break;
    case 'T': bit = CAB_TEST;       break;
    case 'S': bit = CAB_SERVICE;    break;
    default: return;
    }
    if (down) { g_cab.buttons |= bit; g_cab_latch |= bit; } else g_cab.buttons &= ~bit;
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
        if (g_frame % 120u < 20u) g_cab.buttons |= CAB_P1_START;
        else                      g_cab.buttons &= ~CAB_P1_START;
    }
    /* LINDBERGH_AUTOGUN=1 as well: the gun circles the screen and fires in
     * half-second bursts, to check aim and trigger without a mouse */
    static int gun = -1;
    if (gun < 0) gun = env_int("LINDBERGH_AUTOGUN", 0) != 0;
    if (gun && coined) {
        double t = g_frame / 90.0;
        int w = g_win.w ? g_win.w : DEFAULT_W, h = g_win.h ? g_win.h : DEFAULT_H;
        int ax, ay;     /* LINDBERGH_AIM=x,y holds the gun still instead */
        const char *aim = getenv("LINDBERGH_AIM");
        if (aim && sscanf(aim, "%d,%d", &ax, &ay) == 2) { g_mouse_x = ax; g_mouse_y = ay; }
        else {
            g_mouse_x = (int)(w / 2 + w / 3 * cos(t));
            g_mouse_y = (int)(h / 2 + h / 3 * sin(t));
        }
        if (g_frame % 60u < 30u) g_buttons |= 1; else g_buttons &= ~1;
    }
}

void host_cabinet_input(CabinetInput *out)
{
    cabinet_autostart();
    /* The game maps the gun through its own calibration, which stretches the
     * raw range: measured, the sight lands at 1.14x + 21, 1.06y + 17. Undo
     * that so the sight is where the pointer is. LINDBERGH_GUN_CAL=sx,ox,sy,oy
     * overrides the measured values (1,0,1,0 sends the pointer unchanged). */
    static double sx = 1.139, ox = 20.6, sy = 1.056, oy = 16.7;
    static int cal = -1;
    if (cal < 0) { const char *v = getenv("LINDBERGH_GUN_CAL"); cal = 1;
                   if (v) sscanf(v, "%lf,%lf,%lf,%lf", &sx, &ox, &sy, &oy); }
    g_cab.gun_x = (int)((g_mouse_x - ox) / sx);
    g_cab.gun_y = (int)((g_mouse_y - oy) / sy);
    g_cab.screen_w = g_win.w ? g_win.w : 1;
    g_cab.screen_h = g_win.h ? g_win.h : 1;
    g_cab.buttons &= ~(CAB_P1_TRIGGER | CAB_P1_RELOAD);
    if (g_buttons & 1) g_cab.buttons |= CAB_P1_TRIGGER;
    if (g_buttons & 4) g_cab.buttons |= CAB_P1_RELOAD;
    if (g_mouse_latch & 1) g_cab.buttons |= CAB_P1_TRIGGER;
    if (g_mouse_latch & 4) g_cab.buttons |= CAB_P1_RELOAD;
    *out = g_cab;
    out->buttons |= g_cab_latch;
}

/* The board has reported the switches: latched taps have been seen. */
void host_cabinet_switches_read(void)
{
    g_cab_latch = 0;
    g_mouse_latch = 0;
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
 * keyboard path is tested exactly as a person drives it. */
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
            e.type = SDL_KEYDOWN; e.key.keysym.sym = (SDL_Keycode)(k >= 'A' && k <= 'Z' ? k + 32 : k);
            SDL_PushEvent(&e);
            e.type = SDL_KEYUP; SDL_PushEvent(&e);
            fprintf(stderr, "[keys] tapped %c at frame %u\n", k, f);
        }
        while (*p == ',') p++;
    }
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
            if (down) { g_buttons |= 1 << b; g_mouse_latch |= 1 << b; } else g_buttons &= ~(1 << b);
            in_push(HOST_IN_MOUSE, b, down ? 0 : 1, g_mouse_x, g_mouse_y);
            break;
        }
        case SDL_KEYDOWN: {
            if (e.key.repeat) break;
            int vk = vk_of(e.key.keysym.sym);
            fprintf(stderr, "[key] down %s (vk %#x)\n", SDL_GetKeyName(e.key.keysym.sym), vk);
            if (!vk) break;
            if (vk == VK_ESCAPE) {
                fprintf(stderr, "[window] Escape pressed - quitting\n");
                exit(0);
            }
            cabinet_key(vk, 1);
            if (e.key.keysym.sym < 0x80)
                in_push(HOST_IN_KEY, (int)e.key.keysym.sym, 1, g_mouse_x, g_mouse_y);
            else
                in_push(HOST_IN_SPECIAL, vk, 1, g_mouse_x, g_mouse_y);
            break;
        }
        case SDL_KEYUP: {
            int vk = vk_of(e.key.keysym.sym);
            if (!vk) break;
            cabinet_key(vk, 0);
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
    pump();
}

/* This game opens its window through GLUT; the X11/GLX seam that a GLX title
 * needs lives in hle_window.c and is Windows-only so far. */
void hle_register_window(void) { }
