/*
 * hle_glx_posix.c - the X11/GLX seam on a 64-bit POSIX host (macOS, Android).
 *
 * hle_window.c does this for Windows, and the reasoning there holds here: the
 * game is an X11/GLX program, and the honest port is one real window with one
 * real GL context (host_sdl.c's), handed out behind the shapes Xlib and GLX
 * promise. What changes on a 64-bit host is where those shapes live. The guest
 * reads struct _XDisplay and Screen through macros compiled into it, so they
 * have to sit inside the guest's 4 GB window, not in host statics.
 *
 * Pbuffers are the one thing GLX gives that a single window does not. A 2005
 * game uses them for render-to-texture: make the pbuffer current, draw, copy
 * the result into a texture with glCopyTexImage2D, make the window current
 * again. In one context that is exactly a framebuffer object, so a pbuffer is
 * an FBO with a colour and a depth/stencil attachment, and glXMakeCurrent on a
 * pbuffer binds it. Every context the game asks for is the same context:
 * shared lists, which is what the game assumes, come for free.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_posix.h"
#include "stereo.h"

/* Tokens for things the game only hands back. Distinctive in a crash dump. */
#define TOK_WINDOW   0x58570001u      /* "XW" */
#define TOK_COLORMAP 0x58430001u      /* "XC" */
#define TOK_CONTEXT  0x47430001u      /* "GC" */
#define TOK_FBCONFIG 0x46430001u      /* "FC" */
#define TOK_CURSOR   0x58520001u
#define TOK_PBUFFER  0x50420000u      /* "PB" + index */

/* struct _XDisplay, Screen and Visual on i386: the fields the Xlib macros
 * read. See hle_window.c for how these offsets were established. */
#define DPY_SIZE 512
#define SCR_SIZE 128
#define VIS_SIZE 64
#define XVI_SIZE 40           /* XVisualInfo */

static uint32_t g_dpy, g_scr, g_vis, g_xvi;

static uint32_t galloc_zero(size_t n)
{
    void *p = guest_alloc(n, 16);
    memset(p, 0, n);
    return H2G(p);
}

static uint32_t build_display(void)
{
    if (g_dpy) return g_dpy;
    int w, h;
    lindbergh_window_size(&w, &h);
    g_dpy = galloc_zero(DPY_SIZE);
    g_scr = galloc_zero(SCR_SIZE);
    g_vis = galloc_zero(VIS_SIZE);
    g_xvi = galloc_zero(XVI_SIZE);

    wr32(g_vis + 4, 0x21);                    /* visualid */
    wr32(g_vis + 8, 4);                       /* TrueColor */
    wr32(g_vis + 12, 0x00FF0000); wr32(g_vis + 16, 0x0000FF00); wr32(g_vis + 20, 0x000000FF);
    wr32(g_vis + 24, 8); wr32(g_vis + 28, 256);

    wr32(g_scr + 4, g_dpy);   wr32(g_scr + 8, TOK_WINDOW);
    wr32(g_scr + 12, (uint32_t)w); wr32(g_scr + 16, (uint32_t)h);
    wr32(g_scr + 20, (uint32_t)(w / 4)); wr32(g_scr + 24, (uint32_t)(h / 4));
    wr32(g_scr + 36, 24); wr32(g_scr + 40, g_vis); wr32(g_scr + 48, TOK_COLORMAP);
    wr32(g_scr + 52, 0x00FFFFFF); wr32(g_scr + 56, 0);

    wr32(g_dpy + 16, 11); wr32(g_dpy + 24, guest_strdup_static("lindberghrecomp"));
    wr32(g_dpy + 52, 32); wr32(g_dpy + 56, 32); wr32(g_dpy + 72, 11);
    wr32(g_dpy + 116, 65535); wr32(g_dpy + 128, guest_strdup_static(":0.0"));
    wr32(g_dpy + 132, 0); wr32(g_dpy + 136, 1); wr32(g_dpy + 140, g_scr);

    /* XVisualInfo: visual, visualid, screen, depth, class, masks, ... */
    wr32(g_xvi + 0, g_vis); wr32(g_xvi + 4, 0x21); wr32(g_xvi + 8, 0);
    wr32(g_xvi + 12, 24); wr32(g_xvi + 16, 4);
    wr32(g_xvi + 20, 0x00FF0000); wr32(g_xvi + 24, 0x0000FF00); wr32(g_xvi + 28, 0x000000FF);
    wr32(g_xvi + 32, 256); wr32(g_xvi + 36, 8);
    return g_dpy;
}

static int ensure_window(void)
{
    build_display();
    return host_window_open("The House of the Dead 4") && host_gl_context();
}

/* ---- pbuffers ---- */
#define MAX_PB 32
static struct { GLuint fb, color, depth; int w, h, depth_tex; } g_pb[MAX_PB];

static int pb_index(uint32_t tok)
{
    if ((tok & 0xFFFF0000u) != TOK_PBUFFER) return -1;
    int i = (int)(tok & 0xFFFF);
    return i > 0 && i < MAX_PB && g_pb[i].fb ? i : -1;
}

static uint32_t glx_attrib(uint32_t list, uint32_t key, uint32_t dflt)
{
    if (!list) return dflt;
    for (int i = 0; i < 64; i += 2) {
        uint32_t k = rd32(list + 4u * i);
        if (!k) break;
        if (k == key) return rd32(list + 4u * (i + 1));
    }
    return dflt;
}

/* glXCreatePbuffer(dpy, config, attribs): GLX_PBUFFER_WIDTH 0x8041, HEIGHT 0x8040 */
static void h_glXCreatePbuffer(CPU *c)
{
    if (!ensure_window()) { RET(0); return; }
    int i = 1;
    while (i < MAX_PB && g_pb[i].fb) i++;
    if (i == MAX_PB) { fprintf(stderr, "[glX] out of pbuffers\n"); RET(0); return; }
    int w = (int)glx_attrib(A32(2), 0x8041, 256), h = (int)glx_attrib(A32(2), 0x8040, 256);

    GLint prev_fb = 0, prev_tex = 0, prev_rb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
    glGenFramebuffers(1, &g_pb[i].fb);
    glGenTextures(1, &g_pb[i].color);
    glBindTexture(GL_TEXTURE_2D, g_pb[i].color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, g_pb[i].fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_pb[i].color, 0);
#ifdef LINDBERGH_GL4ES
    /* In the headset gl4es draws both eyes at once into two-layer copies of
     * the textures a framebuffer has; a renderbuffer has one layer, and the
     * pair was incomplete. Depth and stencil are a texture too. */
    glGenTextures(1, &g_pb[i].depth);
    glBindTexture(GL_TEXTURE_2D, g_pb[i].depth);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_pb[i].depth, 0);
    g_pb[i].depth_tex = 1;
#else
    glGenRenderbuffers(1, &g_pb[i].depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_pb[i].depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_pb[i].depth);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_pb[i].depth);
#endif
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fb);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prev_rb);
    g_pb[i].w = w; g_pb[i].h = h;
    fprintf(stderr, "[glX] pbuffer %d: %dx%d%s\n", i, w, h,
            st == GL_FRAMEBUFFER_COMPLETE ? "" : " (incomplete framebuffer)");
    RET(TOK_PBUFFER | (uint32_t)i);
}

static void h_glXDestroyPbuffer(CPU *c)
{
    int i = pb_index(A32(1));
    if (i < 0) return;
    glDeleteFramebuffers(1, &g_pb[i].fb);
    glDeleteTextures(1, &g_pb[i].color);
    if (g_pb[i].depth_tex) glDeleteTextures(1, &g_pb[i].depth);
    else glDeleteRenderbuffers(1, &g_pb[i].depth);
    memset(&g_pb[i], 0, sizeof g_pb[i]);
}

/* ---- GLX ---- */
static int g_drawable_pb;            /* which pbuffer is current, 0 = the window */

static void h_glXQueryExtension(CPU *c)
{
    if (A32(1)) wr32(A32(1), 0);
    if (A32(2)) wr32(A32(2), 0);
    RET(1);
}
static void h_glXChooseVisual(CPU *c)  { if (!ensure_window()) { RET(0); return; } c->eax = g_xvi; }
static void h_glXGetVisualFromFBConfig(CPU *c) { if (!ensure_window()) { RET(0); return; } c->eax = g_xvi; }

/* glXChooseFBConfig(dpy, screen, attribs, int *nelements): one config fits
 * everything a 2005 game asks for. The array is the game's to XFree. */
static void h_glXChooseFBConfig(CPU *c)
{
    if (!ensure_window()) { RET(0); return; }
    uint32_t arr = galloc_zero(8);
    wr32(arr, TOK_FBCONFIG);
    if (A32(3)) wr32(A32(3), 1);
    c->eax = arr;
}

static void h_glXCreateContext(CPU *c) { c->eax = ensure_window() ? TOK_CONTEXT : 0; }
static void h_glXDestroyContext(CPU *c) { (void)c; }

/* glXMakeCurrent(dpy, drawable, ctx). The context is always host_sdl's; what
 * changes is the drawable, which picks the framebuffer. */
static void h_glXMakeCurrent(CPU *c)
{
    {   /* LINDBERGH_FRAMEMAP: which drawable the frame's draws go to */
        static long map = -2;
        if (map == -2) { const char *v = getenv("LINDBERGH_FRAMEMAP"); map = v && *v ? atol(v) : -1; }
        extern unsigned g_frame;
        if ((long)g_frame == map || (long)g_frame == map + 1)
            fprintf(stderr, "[map] frame %u glXMakeCurrent drawable %#x context %#x\n", g_frame, A32(1), A32(2));
    }
    if (!A32(2)) { RET(1); return; }
    if (!ensure_window()) { RET(0); return; }
    int i = pb_index(A32(1));
    g_drawable_pb = i > 0 ? i : 0;
    /* In the headset a pbuffer is the eyes too. The House of the Dead 4
     * draws its whole 3D scene into one, copies it into a texture and draws
     * that over the window under its HUD: drawn into the eyes directly, the
     * scene is seen from each of them, and stereo.c leaves out the copy laid
     * back over it. */
    if (stereo_enabled()) stereo_bind_window();
    else glBindFramebuffer(GL_FRAMEBUFFER, i > 0 ? g_pb[i].fb : 0);
    {
        static int said;
        if (!said++) fprintf(stderr, "[glX] context current\n");
    }
    RET(1);
}

/* Swapping a pbuffer is a no-op in GLX; only the window presents. */
static void h_glXSwapBuffers(CPU *c)
{
    if (pb_index(A32(1)) > 0) { RET(0); return; }
    host_swap();
    host_pump();
    RET(0);
}

static void h_glXGetProcAddressARB(CPU *c)
{
    const char *name = ASTR(0);
    uint32_t va = hle_plt_address(name);
    if (!va) va = gl_token_for(name);
    RET(va);
}

/* ---- Xlib ---- */
static void h_XOpenDisplay(CPU *c) { c->eax = ensure_window() ? g_dpy : 0; }
static void h_XCreateWindow(CPU *c) { ensure_window(); RET(TOK_WINDOW); }
static void h_XCreateColormap(CPU *c) { RET(TOK_COLORMAP); }
static void h_XMapWindow(CPU *c) { host_pump(); RET(0); }
static void h_XSetStandardProperties(CPU *c)
{
    if (A32(2)) host_window_open(ASTR(2));
    RET(0);
}
static void h_XPending(CPU *c) { host_pump(); RET(0); }
static void h_XFlush(CPU *c)   { host_pump(); RET(0); }
static void h_XSync(CPU *c)    { host_pump(); RET(0); }

/* The cabinet's controls arrive over JVS, so the X event queue is empty by
 * design; it must still pump the host's, or the window stops responding. */
static void h_XNextEvent(CPU *c)
{
    host_pump();
    if (A32(1)) memset(APTR(1), 0, 96);       /* sizeof(XEvent) on i386 */
    RET(0);
}

static void h_XQueryPointer(CPU *c)
{
    int x = 0, y = 0, b = 0;
    host_mouse_state(&x, &y, &b);
    if (A32(2)) wr32(A32(2), TOK_WINDOW);
    if (A32(3)) wr32(A32(3), TOK_WINDOW);
    if (A32(4)) wr32(A32(4), (uint32_t)x);
    if (A32(5)) wr32(A32(5), (uint32_t)y);
    if (A32(6)) wr32(A32(6), (uint32_t)x);
    if (A32(7)) wr32(A32(7), (uint32_t)y);
    if (A32(8)) wr32(A32(8), (b & 1 ? 1u << 8 : 0) | (b & 4 ? 1u << 10 : 0));
    RET(1);
}

static void h_XTranslateCoordinates(CPU *c)
{
    if (A32(5)) wr32(A32(5), A32(3));
    if (A32(6)) wr32(A32(6), A32(4));
    if (A32(7)) wr32(A32(7), TOK_WINDOW);
    RET(1);
}

static void h_XMissingExtension(CPU *c)
{
    fprintf(stderr, "[X] missing extension: %s\n", A32(1) ? ASTR(1) : "(null)");
    RET(0);
}

/* XKeysymToKeycode: any non-zero keycode; X key events never arrive. */
static void h_XKeysymToKeycode(CPU *c) { RET((A32(1) & 0xFF) | 8); }

/* Grabs, focus, cursors, properties: arranged already, Success. */
static void h_x_zero(CPU *c)   { RET(0); }
static void h_x_one(CPU *c)    { RET(1); }
static void h_x_cursor(CPU *c) { RET(TOK_CURSOR); }

/* The raw protocol (_XReply, _XSend, ...) is what a statically linked X
 * extension library (Xxf86vm and friends) speaks underneath. hle_vidmode
 * overrides those libraries at their entry points, so these are only reached
 * if an extension call slips past it: report it once and fail the request. */
static void h_x_protocol(CPU *c)
{
    static int said;
    if (!said++) fprintf(stderr, "[X] raw protocol call reached; answering failure\n");
    RET(0);
}

void hle_register_glx_posix(void)
{
    hle_bind("glXQueryExtension", h_glXQueryExtension);
    hle_bind("glXChooseVisual", h_glXChooseVisual);
    hle_bind("glXChooseFBConfig", h_glXChooseFBConfig);
    hle_bind("glXGetVisualFromFBConfig", h_glXGetVisualFromFBConfig);
    hle_bind("glXCreateContext", h_glXCreateContext);
    hle_bind("glXDestroyContext", h_glXDestroyContext);
    hle_bind("glXMakeCurrent", h_glXMakeCurrent);
    hle_bind("glXSwapBuffers", h_glXSwapBuffers);
    hle_bind("glXCreatePbuffer", h_glXCreatePbuffer);
    hle_bind("glXDestroyPbuffer", h_glXDestroyPbuffer);
    hle_bind("glXGetProcAddressARB", h_glXGetProcAddressARB);

    hle_bind("XOpenDisplay", h_XOpenDisplay);
    hle_bind("XCreateWindow", h_XCreateWindow);
    hle_bind("XCreateColormap", h_XCreateColormap);
    hle_bind("XMapWindow", h_XMapWindow);
    hle_bind("XSetStandardProperties", h_XSetStandardProperties);
    hle_bind("XPending", h_XPending);
    hle_bind("XFlush", h_XFlush);
    hle_bind("XSync", h_XSync);
    hle_bind("XNextEvent", h_XNextEvent);
    hle_bind("XQueryPointer", h_XQueryPointer);
    hle_bind("XTranslateCoordinates", h_XTranslateCoordinates);
    hle_bind("XMissingExtension", h_XMissingExtension);
    hle_bind("XKeysymToKeycode", h_XKeysymToKeycode);
    hle_bind("XInitThreads", h_x_one);

    static const char *zero[] = {
        "XFree", "XFreePixmap", "XMoveWindow", "XWarpPointer", "XGrabPointer",
        "XSetInputFocus", "XDefineCursor", "XAutoRepeatOff", "XAutoRepeatOn",
        "XLookupString", "XStoreName", "XCloseDisplay", "XDestroyWindow",
        "XextAddDisplay", "XextRemoveDisplay", "XextFindDisplay", "XextCreateExtension",
        "XUngrabPointer", "XSetErrorHandler",
    };
    for (unsigned i = 0; i < sizeof zero / sizeof *zero; i++) hle_bind(zero[i], h_x_zero);
    hle_bind("XCreateBitmapFromData", h_x_cursor);
    hle_bind("XCreatePixmapCursor", h_x_cursor);
    hle_bind("XCreateFontCursor", h_x_cursor);

    static const char *proto[] = { "_XReply", "_XSend", "_XRead", "_XReadPad", "_XEatData", "_XFlush" };
    for (unsigned i = 0; i < sizeof proto / sizeof *proto; i++) hle_bind(proto[i], h_x_protocol);
}
