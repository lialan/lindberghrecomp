/*
 * stereo.c - drawing each frame once per eye.
 *
 * Every GL import passes through stereo_gl_call() (hle_call's g_gl_hook). The
 * game draws its frame as usual - that is the first eye - and each call is
 * recorded as its handler plus a copy of its stack arguments. At the swap the
 * recording is replayed for the second eye. Both passes go through the same
 * three changes, and nothing else about the game's GL is touched:
 *
 *   - framebuffer 0 is the eye's image, not the window;
 *   - every viewport, scissor and render-target texture is scaled from the
 *     game's 640x480 to the eye's resolution;
 *   - a perspective projection the game loads is replaced by the eye's own,
 *     with the eye's offset from the game camera folded in.
 *
 * The game's frame is drawn entirely offscreen (scene, bloom, HUD composite)
 * and copied to framebuffer 0 with one quad, so redirecting framebuffer 0 is
 * enough to put the finished image in the eye.
 *
 * Not replayed: queries (the game already has its answer), object creation
 * and deletion, and texture uploads, whose source memory the game may have
 * freed by the time the second eye is drawn. Small vectors the game passes
 * by pointer - matrices, colours, constants - often live on its stack, so
 * those are copied at record time.
 *
 * LINDBERGH_STEREO=1 turns it on (the Quest host does so itself); on a
 * desktop the two eyes are shown side by side. LINDBERGH_FRAMEMAP=<frame>
 * lists that frame's GL calls with their first arguments.
 * LINDBERGH_MENU_DRAWS=<n> (120): a frame with fewer world draws looks like a
 * menu; LINDBERGH_SCENE_STATS=1 reports the count every 50 frames.
 * LINDBERGH_ZOOM_LENS=<x> (4): 3D drawn with a longer lens (projection x
 * scale; the game's usual is 1.7-2.4, its sniper scope 11.4) is a magnified
 * view, put on the screen like the HUD.
 */

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gl_posix.h"
#include "stereo.h"
#ifdef LINDBERGH_GL4ES
#include <EGL/egl.h>
#endif
#include "arbprog.h"
#ifdef LINDBERGH_GL4ES
/* gl4es' multiview (multiview.c in quest/gl4es.patch) */
int gl4es_multiview_enable(void);
void gl4es_multiview_set(const float m[3][16], int clip);
unsigned gl4es_multiview_array(unsigned texture);
int gl4es_multiview_read(unsigned texture, GLenum target, int layer, void *out);
extern int gl4es_mv_trace;
void gl4es_multiview_read_pixel(int x, int y, unsigned char out[4]);
#endif

extern unsigned g_frame;

#define GAME_W 640
#define GAME_H 480

/* ---- eyes ---- */
static int      g_on;
static int      g_mv;                   /* both eyes in one pass (GL_OVR_multiview2): no second eye to replay */
static float    g_mvR[3][16];           /* the views' matrices last given to GL, and back to the game's clip space */
static int      g_eye;                  /* the eye being drawn */
static int      g_ew = 1024, g_eh = 1024;
static float    g_sx, g_sy;             /* eye pixels per game pixel */
static GLuint   g_eye_fbo[2], g_eye_tex[2], g_eye_depth[2];
static int      g_external[2];          /* the host supplies the eye images */
static float    g_view[2][16];          /* game camera -> eye */
static float    g_tan[2][4];            /* left, right, down, up */

/* ---- what the game believes ---- */
static GLuint   g_game_fb;              /* framebuffer the game has bound */
static GLint    g_vp[4] = { 0, 0, GAME_W, GAME_H };
static GLenum   g_mode = GL_MODELVIEW;
static float    g_proj[32][16];         /* the game's projection stack */
static int      g_proj_top;
static float    g_last_persp[16] = { 1.732f, 0, 0, 0, 0, 2.309f, 0, 0, 0, 0, -1, -1, 0, 0, -0.02f, 0 };
                                        /* the game's last perspective (its usual one to start) */
static int      g_loaded_hud;           /* the projection GL has is the HUD's screen */
static GLuint   g_unit0_rect;           /* render target bound to texture unit 0 */
static GLenum   g_active_unit = GL_TEXTURE0;
static GLuint   g_rect_bound[8];        /* rectangle texture bound to each unit */
static unsigned g_rect_on;              /* units with GL_TEXTURE_RECTANGLE enabled */
static unsigned g_texmat_scaled;        /* units whose texture matrix fixed_function_rect() scaled */
static float    g_screen_dist = 3.0f;   /* the HUD's screen, in game units from the camera */
static GLuint   g_hud_fb, g_hud_fb_next = ~0u; /* where the HUD went, last frame and this one */
static int      g_scissor_on;           /* the game's scissor test */
static GLint    g_scissor[4];           /* the game's scissor box, in eye pixels */
static int      g_clipped;              /* a screen draw is clipped to the screen: undo after it */
static GLint    g_eye_vp[4];            /* the viewport GL has, in eye pixels */
static unsigned g_scene_draws;          /* 3D draws in the world this frame */
static float    g_lens_seen;            /* the longest lens (projection x scale) drawn with this frame */
static float    g_zoom_lens = 4.0f;     /* a longer one is a zoomed view (the game's own is 1.7-2.4) */
static int      g_menu;                 /* a menu: its 3D belongs on the screen too */
static int      g_tex2d_on;             /* GL_TEXTURE_2D enabled on unit 0 (frame map) */
static unsigned g_menu_frames;          /* frames in a row that looked like a menu */
static unsigned g_menu_draws = 120;
static unsigned g_zoom_draws, g_hudfb_draws;   /* this frame's 3D sent to the screen, for LINDBERGH_MODE_LOG */     /* fewer world draws than this looks like a menu */

/* ---- the recording ---- */
enum { OP_CALL, OP_UNMAP, OP_PTR };
typedef struct { unsigned char kind; HleId id; HleHandler fn; uint32_t va, n; GLenum target; } Op;
static Op      *g_ops;
static unsigned g_nops, g_cap;
static uint32_t g_arena;                /* guest memory for arguments and copies */
static uint32_t g_arena_used;
#define ARENA_SIZE (48u << 20)
#define ARGS 16                         /* dwords of stack copied per call */
static int      g_replaying;
static unsigned g_overflow;

static long g_map_frame = -1;

/* ---- matrices (column-major, as GL) ---- */
static void mat_identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void mat_mul(float *out, const float *a, const float *b)
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] +
                            a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
    memcpy(out, r, 64);
}
static void mat_ortho(float *m, double l, double r, double b, double t, double n, double f)
{
    mat_identity(m);
    m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n));
}
static void mat_frustum(float *m, double l, double r, double b, double t, double n, double f)
{
    memset(m, 0, 64);
    m[0] = (float)(2 * n / (r - l)); m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b));
    m[10] = (float)(-(f + n) / (f - n)); m[11] = -1; m[14] = (float)(-2 * f * n / (f - n));
}
static int is_perspective(const float *m) { return fabsf(m[11] + 1) < 1e-4f && fabsf(m[15]) < 1e-4f; }

/* What GL is given for the game's projection p.
 *
 * A perspective is the game's camera: it becomes the eye's frustum, seen from
 * the eye's place relative to that camera. An orthographic projection is
 * either a full-screen pass over a render target - already drawn in the eye's
 * view, so left alone - or the HUD. The HUD is put on a screen fixed in front
 * of the game camera, as wide as the game's own field of view, so its sight
 * lies on the ray the game fires along. 3D drawn where the HUD goes - menu
 * models, spent cases - is screen content too: flattened onto that screen
 * exactly where the arcade monitor showed it. */
static void effective_projection(const float *p, int hud, float *out)
{
    if (!g_on) { memcpy(out, p, 64); return; }
    const float *t = g_tan[g_eye];
    float e[16], ev[16];
    if (is_perspective(p) && !hud) {
        memcpy(g_last_persp, p, 64);
        float n = p[14] / (p[10] - 1), f = p[14] / (p[10] + 1);
        mat_frustum(e, -t[0] * n, t[1] * n, -t[2] * n, t[3] * n, n, f);
        mat_mul(out, e, g_view[g_eye]);
        return;
    }
    if (!hud) { memcpy(out, p, 64); return; }
    float d = g_screen_dist, sc[16];
    mat_identity(sc);                   /* game NDC -> a point on the screen */
    sc[0] = d / g_last_persp[0]; sc[5] = d / g_last_persp[5];
    sc[10] = -0.02f * d;                /* a sliver of depth keeps the game's depth order */
    sc[14] = -d;
    mat_frustum(e, -t[0] * 0.01f, t[1] * 0.01f, -t[2] * 0.01f, t[3] * 0.01f, 0.01f, 1000.0f);
    mat_mul(ev, e, g_view[g_eye]);
    mat_mul(ev, ev, sc);
    mat_mul(out, ev, p);
}

static int mat_invert(const float *m, float *out);

/* Multiview: GL is given the first eye's projection, as when the eyes are
 * drawn one at a time, so the first view is exactly that; the second view's
 * matrix takes the first eye's clip space to its own (the two frusta differ
 * by the eyes' offset, so it is near identity). Worked out in double: going
 * through the game's own projection instead loses the far plane to rounding,
 * as the game's is all but infinite. A third matrix takes the first eye's
 * clip space back to the game's, where the arcade screen is -1..1, for
 * clipping screen content (clip_to_screen). */
static int mat_invert(const float *m, float *out);

static void mat_mul_d(double *out, const double *a, const double *b)
{
    double r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            r[c * 4 + rr] = a[rr] * b[c * 4] + a[4 + rr] * b[c * 4 + 1] + a[8 + rr] * b[c * 4 + 2] + a[12 + rr] * b[c * 4 + 3];
    memcpy(out, r, sizeof r);
}

static int mat_invert_d(const double *m, double *o)
{
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[c * 4 + r] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[piv][c])) piv = r;
        if (fabs(a[piv][c]) < 1e-30) return 0;
        for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[piv][k]; a[piv][k] = t; }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[c * 4 + r] = a[r][c + 4];
    return 1;
}

static void view_matrices(const float *p, float *eye0)
{
    float eff1[16];
    g_eye = 1; effective_projection(p, g_loaded_hud, eff1);
    g_eye = 0; effective_projection(p, g_loaded_hud, eye0);
#ifdef LINDBERGH_GL4ES
    double e0[16], e1[16], pd[16], inv[16], r[16];
    for (int i = 0; i < 16; i++) { e0[i] = eye0[i]; e1[i] = eff1[i]; pd[i] = p[i]; }
    mat_identity(g_mvR[0]);
    mat_identity(g_mvR[1]);
    mat_identity(g_mvR[2]);
    if (mat_invert_d(e0, inv)) {
        mat_mul_d(r, e1, inv);
        for (int i = 0; i < 16; i++) g_mvR[1][i] = (float)r[i];
        mat_mul_d(r, pd, inv);
        for (int i = 0; i < 16; i++) g_mvR[2][i] = (float)r[i];
    }
    gl4es_multiview_set((const float (*)[16])g_mvR, g_clipped);
#endif
}

static void load_projection(void)
{
    float m[16];
    if (g_mv) view_matrices(g_proj[g_proj_top], m);
    else effective_projection(g_proj[g_proj_top], g_loaded_hud, m);
    if (g_mode != GL_PROJECTION) glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(m);
    if (g_mode != GL_PROJECTION) glMatrixMode(g_mode);
}

/* ---- render targets ---- */
#define MAX_SCALED 256
static GLuint g_scaled[MAX_SCALED];
static unsigned g_nscaled;

static int scaled(GLuint t)
{
    for (unsigned i = 0; i < g_nscaled; i++) if (g_scaled[i] == t) return 1;
    return 0;
}

/* A texture the game renders into is reallocated at the eye's resolution.
 * Sampling uses normalised coordinates, so nothing that reads it changes. */
static void scale_target(GLenum textarget, GLuint tex)
{
    if (!tex || scaled(tex) || g_nscaled >= MAX_SCALED ||
        (textarget != GL_TEXTURE_2D && textarget != GL_TEXTURE_RECTANGLE_ARB)) return;
    g_scaled[g_nscaled++] = tex;
    GLenum tg = textarget;
    GLint prev = 0, w = 0, h = 0, ifmt = 0;
    glGetIntegerv(tg == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_RECTANGLE_ARB, &prev);
    glBindTexture(tg, tex);
    glGetTexLevelParameteriv(tg, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(tg, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTexLevelParameteriv(tg, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
    GLenum fmt = GL_RGBA, type = GL_UNSIGNED_BYTE;
    if (ifmt == GL_DEPTH_COMPONENT || ifmt == GL_DEPTH_COMPONENT16 || ifmt == GL_DEPTH_COMPONENT24 ||
        ifmt == GL_DEPTH_COMPONENT32) { fmt = GL_DEPTH_COMPONENT; type = GL_UNSIGNED_INT; }
    else if (ifmt == GL_DEPTH24_STENCIL8 || ifmt == GL_DEPTH_STENCIL) { fmt = GL_DEPTH_STENCIL; type = GL_UNSIGNED_INT_24_8; }
    else if (ifmt == GL_RGB || ifmt == GL_RGB8) fmt = GL_RGB;
    int nw = (int)lroundf(w * g_sx), nh = (int)lroundf(h * g_sy);
    glTexImage2D(tg, 0, ifmt, nw, nh, 0, fmt, type, NULL);
    fprintf(stderr, "[stereo] render target %u%s: %dx%d -> %dx%d (format %#x)\n", tex,
            tg == GL_TEXTURE_2D ? "" : " (rect)", w, h, nw, nh, ifmt);
    glBindTexture(tg, (GLuint)prev);
}

static void bind_framebuffer(GLenum target, GLuint fb)
{
    g_game_fb = fb;
    glBindFramebuffer(target, fb ? fb : g_eye_fbo[g_eye]);
}

/* Multiview, wanted unless LINDBERGH_MULTIVIEW=0: one framebuffer for both
 * eyes, its colour and depth rectangle textures that gl4es keeps as two
 * layers. Turned on before the game makes any program or render target. */
static void eye_targets_multiview(void)
{
#ifdef LINDBERGH_GL4ES
    static int tried;
    if (!tried) {
        tried = 1;
        const char *v = getenv("LINDBERGH_MULTIVIEW");
        g_mv = !(v && *v == '0') && gl4es_multiview_enable();
        fprintf(stderr, "[stereo] %s\n", g_mv ? "multiview: both eyes in one pass" : "second eye replayed");
    }
    if (!g_mv || g_eye_fbo[0]) return;
    glGenTextures(2, g_eye_tex);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, g_eye_tex[0]);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA, g_ew, g_eh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, g_eye_tex[1]);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_DEPTH_COMPONENT, g_ew, g_eh, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);
    glGenFramebuffers(1, &g_eye_fbo[0]);
    glBindFramebuffer(GL_FRAMEBUFFER, g_eye_fbo[0]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE_ARB, g_eye_tex[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_RECTANGLE_ARB, g_eye_tex[1], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[stereo] multiview eye framebuffer incomplete\n");
    g_eye_fbo[1] = g_eye_fbo[0];
#endif
}

static void eye_targets(void)
{
    eye_targets_multiview();
    if (g_mv) return;
    for (int i = 0; i < 2; i++) {
        if (g_eye_fbo[i] || g_external[i]) continue;
        glGenTextures(1, &g_eye_tex[i]);
        glBindTexture(GL_TEXTURE_2D, g_eye_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_ew, g_eh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenRenderbuffers(1, &g_eye_depth[i]);
        glBindRenderbuffer(GL_RENDERBUFFER, g_eye_depth[i]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, g_ew, g_eh);
        glGenFramebuffers(1, &g_eye_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, g_eye_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_eye_tex[i], 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_eye_depth[i]);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_eye_depth[i]);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "[stereo] eye %d framebuffer incomplete\n", i);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* ---- calls with something to change ---- */
static float argf(CPU *c, int i) { uint32_t b = A32(i); float f; memcpy(&f, &b, 4); return f; }

/* ---- client-side vertex arrays ----
 * The game builds many draws in one scratch buffer of its own memory and
 * reuses it within the frame, so by the second eye that memory holds the
 * last draw's vertices. Each draw's slice is copied at record time and the
 * arrays re-pointed at the copy for the replay. */
enum { ARR_VERTEX, ARR_NORMAL, ARR_COLOR, ARR_TEX0, ARR_ATTR0 = ARR_TEX0 + 8, ARR_COUNT = ARR_ATTR0 + 16 };
typedef struct { int on, size, stride, norm; GLenum type; uint32_t ptr; GLuint vbo; } Arr;
static Arr g_arr[ARR_COUNT];
static int g_client_unit;
static Arr *g_ptr_fmt;                  /* each copy's array format, as recorded */
static unsigned g_nfmt, g_fmtcap;

/* Does a 2D draw cover the whole viewport? A pass over a render target does;
 * a render target drawn as a sprite (a menu's 3D model, drawn offscreen
 * first) does not, and belongs on the screen with the rest of the 2D.
 * Immediate-mode draws are all passes in this game. */
/* The game-NDC box of a small 2D draw's vertices; 0 if it cannot tell. */
static int ndc_box(CPU *c, const char *n, float box[4])
{
    int elements = !strcmp(n, "glDrawElements");
    if (!elements && strcmp(n, "glDrawArrays")) return 0;
    Arr *a = &g_arr[ARR_VERTEX];
    if (!a->on || a->size < 2 || a->type != GL_FLOAT) return 0;
    GLsizei count = elements ? AI32(1) : AI32(2);
    if (count <= 0 || count > 64) return 0;
    uint32_t stride = a->stride ? (uint32_t)a->stride : (uint32_t)a->size * 4;
    uint32_t first = elements ? 0 : A32(1);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    const float *m = g_proj[g_proj_top];
    for (GLsizei i = 0; i < count; i++) {
        uint32_t v = first + (uint32_t)i;
        if (elements) {
            if (gl_bound_buffer(GL_ELEMENT_ARRAY_BUFFER)) return 0;
            GLenum t = A32(2);
            const void *idx = GP(A32(3));
            v = t == GL_UNSIGNED_SHORT ? ((const uint16_t *)idx)[i] : t == GL_UNSIGNED_INT ? ((const uint32_t *)idx)[i]
                                       : ((const uint8_t *)idx)[i];
        }
        float p[2];
        if (a->vbo) {
            GLint was = 0;
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &was);
            glBindBuffer(GL_ARRAY_BUFFER, a->vbo);
            glGetBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(a->ptr + v * stride), 8, p);
            glBindBuffer(GL_ARRAY_BUFFER, (GLuint)was);
        } else memcpy(p, GP(a->ptr + v * stride), 8);
        /* to NDC, through the game's own 2D projection (w is 1) */
        float x = m[0] * p[0] + m[4] * p[1] + m[12], y = m[1] * p[0] + m[5] * p[1] + m[13];
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    box[0] = x0; box[1] = y0; box[2] = x1; box[3] = y1;
    return 1;
}

static int covers_viewport(CPU *c, const char *n)
{
    float b[4];
    if (!ndc_box(c, n, b)) return 1;
    return b[0] <= -0.95f && b[2] >= 0.95f && b[1] <= -0.95f && b[3] >= 0.95f;
}

/* A cinema bar: a band across the whole width along the top or bottom edge,
 * no more than a fifth of the height. In a headset it would float in front
 * of the scene, so outside menus it is not drawn. */
static int letterbox_bar(CPU *c, const char *n)
{
    float b[4];
    if (g_menu || g_unit0_rect || !ndc_box(c, n, b)) return 0;
    return b[0] <= -0.98f && b[2] >= 0.98f && b[3] - b[1] <= 0.4f && (b[3] >= 0.98f || b[1] <= -0.98f);
}

/* The arcade monitor's edge clipped whatever the game drew past it: on the
 * screen in front of the player, the scissor box around that screen does. */
static void clip_to_screen(void)
{
#ifdef LINDBERGH_GL4ES
    if (g_mv) {     /* the screen is -1..1 in the game's clip space: the views' clip distances */
        g_clipped = 1;
        gl4es_multiview_set((const float (*)[16])g_mvR, 1);
        return;
    }
#endif
    float m[16], e[16], d = g_screen_dist;
    const float *t = g_tan[g_eye];
    mat_frustum(e, -t[0] * 0.01f, t[1] * 0.01f, -t[2] * 0.01f, t[3] * 0.01f, 0.01f, 1000.0f);
    mat_mul(m, e, g_view[g_eye]);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int k = 0; k < 4; k++) {
        float p[3] = { (k & 1 ? 1 : -1) * d / g_last_persp[0], (k & 2 ? 1 : -1) * d / g_last_persp[5], -d };
        float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
        float cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
        float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        if (cw <= 1e-4f) return;                    /* behind the eye: no clipping */
        float x = g_eye_vp[0] + (cx / cw * 0.5f + 0.5f) * g_eye_vp[2];
        float y = g_eye_vp[1] + (cy / cw * 0.5f + 0.5f) * g_eye_vp[3];
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    GLint X0 = (GLint)floorf(x0), Y0 = (GLint)floorf(y0), X1 = (GLint)ceilf(x1), Y1 = (GLint)ceilf(y1);
    if (g_scissor_on) {                             /* within the game's own box too */
        if (g_scissor[0] > X0) X0 = g_scissor[0];
        if (g_scissor[1] > Y0) Y0 = g_scissor[1];
        if (g_scissor[0] + g_scissor[2] < X1) X1 = g_scissor[0] + g_scissor[2];
        if (g_scissor[1] + g_scissor[3] < Y1) Y1 = g_scissor[1] + g_scissor[3];
    }
    if (X1 < X0) X1 = X0;
    if (Y1 < Y0) Y1 = Y0;
    glScissor(X0, Y0, X1 - X0, Y1 - Y0);
    if (!g_scissor_on) glEnable(GL_SCISSOR_TEST);
    g_clipped = 1;
}

static void clip_done(void)
{
    if (!g_clipped) return;
    g_clipped = 0;
#ifdef LINDBERGH_GL4ES
    if (g_mv) { gl4es_multiview_set((const float (*)[16])g_mvR, 0); return; }
#endif
    if (g_scissor_on) glScissor(g_scissor[0], g_scissor[1], g_scissor[2], g_scissor[3]);
    else glDisable(GL_SCISSOR_TEST);
}

/* A fixed-function draw samples a rectangle texture in the game's texels,
 * but its render targets are drawn larger: scale that unit's texture matrix
 * for the draw. (A program's lookups are scaled in arbprog.c.) Undone by
 * fixed_function_rect_done() after the draw - after glEnd for glBegin. */
static void fixed_function_rect(void)
{
    int fp = 0;
    if (!arb_is_enabled(GL_FRAGMENT_PROGRAM_ARB, &fp)) fp = 0;
    unsigned want = 0;
    if (!fp)
        for (unsigned u = 0; u < 8; u++)
            if ((g_rect_on & (1u << u)) && scaled(g_rect_bound[u])) want |= 1u << u;
    if (!want) return;
    GLint mode = 0;
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glMatrixMode(GL_TEXTURE);
    for (unsigned u = 0; u < 8; u++) {
        if (!(want & (1u << u))) continue;
        glActiveTexture(GL_TEXTURE0 + u);
        float t[16], sc[16];
        glGetFloatv(GL_TEXTURE_MATRIX, t);
        glPushMatrix();
        mat_identity(sc); sc[0] = g_sx; sc[5] = g_sy;
        mat_mul(t, sc, t);
        glLoadMatrixf(t);
    }
    glActiveTexture(g_active_unit);
    glMatrixMode((GLenum)mode);
    g_texmat_scaled = want;
}

static void fixed_function_rect_done(void)
{
    if (!g_texmat_scaled) return;
    GLint mode = 0;
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glMatrixMode(GL_TEXTURE);
    for (unsigned u = 0; u < 8; u++)
        if (g_texmat_scaled & (1u << u)) { glActiveTexture(GL_TEXTURE0 + u); glPopMatrix(); }
    glActiveTexture(g_active_unit);
    glMatrixMode((GLenum)mode);
    g_texmat_scaled = 0;
}

/* ---- what each GL import is, worked out once ----
 * The game makes 20-30 thousand GL calls a frame; telling them apart by name
 * on every call cost more than the calls. */
enum {
    K_OTHER, K_BINDFB, K_VIEWPORT, K_SCISSOR, K_FBTEX2D, K_ACTIVETEX, K_BINDTEX, K_ENABLE, K_DISABLE,
    K_DRAWARRAYS, K_DRAWELEMENTS, K_DRAWOTHER, K_BEGIN, K_END, K_MATRIXMODE,
    K_LOADID, K_LOADMATRIX, K_MULTMATRIX, K_ORTHO, K_ORTHO2D, K_FRUSTUM, K_PERSPECTIVE,
    K_PUSHMATRIX, K_POPMATRIX, K_TRANSLATE, K_SCALE, K_GETINTEGERV, K_GETFLOATV, K_GETOTHER, K_UNMAP,
};
typedef struct {
    uint8_t ok, kind, live, track;          /* live: run, never recorded; track: client array state */
    int8_t pidx;                            /* an argument passed by pointer that is copied */
    uint8_t pkind;                          /* 0: pbytes; 1: glBufferData's size; 2: glBufferSubData's */
    uint32_t pbytes;
} Cls;
static Cls g_cls[HLE_COUNT];

static int live_only(const char *n);
static void pointer_arg(const char *n, int *idx, uint32_t *bytes, uint8_t *kind);

static const Cls *cls(HleId id)
{
    Cls *k = &g_cls[id];
    if (k->ok) return k;
    const char *n = hle_name(id);
    static const struct { const char *name; uint8_t kind; } T[] = {
        { "glBindFramebufferEXT", K_BINDFB }, { "glBindFramebuffer", K_BINDFB },
        { "glViewport", K_VIEWPORT }, { "glScissor", K_SCISSOR },
        { "glFramebufferTexture2DEXT", K_FBTEX2D }, { "glFramebufferTexture2D", K_FBTEX2D },
        { "glActiveTexture", K_ACTIVETEX }, { "glActiveTextureARB", K_ACTIVETEX }, { "glBindTexture", K_BINDTEX },
        { "glEnable", K_ENABLE }, { "glDisable", K_DISABLE },
        { "glDrawArrays", K_DRAWARRAYS }, { "glDrawElements", K_DRAWELEMENTS },
        { "glBegin", K_BEGIN }, { "glEnd", K_END }, { "glMatrixMode", K_MATRIXMODE },
        { "glLoadIdentity", K_LOADID }, { "glLoadMatrixf", K_LOADMATRIX }, { "glMultMatrixf", K_MULTMATRIX },
        { "glOrtho", K_ORTHO }, { "gluOrtho2D", K_ORTHO2D }, { "glFrustum", K_FRUSTUM }, { "gluPerspective", K_PERSPECTIVE },
        { "glPushMatrix", K_PUSHMATRIX }, { "glPopMatrix", K_POPMATRIX },
        { "glTranslatef", K_TRANSLATE }, { "glScalef", K_SCALE },
        { "glGetIntegerv", K_GETINTEGERV }, { "glGetFloatv", K_GETFLOATV },
        { "glUnmapBufferARB", K_UNMAP }, { "glUnmapBuffer", K_UNMAP },
    };
    k->kind = K_OTHER;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(n, T[i].name)) { k->kind = T[i].kind; break; }
    if (k->kind == K_OTHER && !strncmp(n, "glDraw", 6)) k->kind = K_DRAWOTHER;
    if (k->kind == K_OTHER && !strncmp(n, "glGet", 5)) k->kind = K_GETOTHER;
    k->live = (uint8_t)live_only(n);
    k->track = !strcmp(n, "glEnableClientState") || !strcmp(n, "glDisableClientState") ||
               !strcmp(n, "glClientActiveTexture") || !strcmp(n, "glClientActiveTextureARB") ||
               !strcmp(n, "glEnableVertexAttribArrayARB") || !strcmp(n, "glDisableVertexAttribArrayARB") ||
               !strcmp(n, "glVertexPointer") || !strcmp(n, "glNormalPointer") || !strcmp(n, "glColorPointer") ||
               !strcmp(n, "glTexCoordPointer") || !strcmp(n, "glVertexAttribPointerARB");
    int idx; uint32_t bytes; uint8_t pk;
    pointer_arg(n, &idx, &bytes, &pk);
    k->pidx = (int8_t)idx; k->pbytes = bytes; k->pkind = pk;
    k->ok = 1;
    return k;
}

static int is_draw(int kind) { return kind == K_DRAWARRAYS || kind == K_DRAWELEMENTS || kind == K_DRAWOTHER; }

/* Returns 1 when it did the call's work itself. */
static int transform_call(CPU *c, HleId id, const char *n)
{
    int k = cls(id)->kind;
    if (k == K_OTHER) return 0;
    if (k == K_BINDFB) {
        bind_framebuffer(A32(0), A32(1));
        return 1;
    }
    if (k == K_VIEWPORT || k == K_SCISSOR) {
        GLint x = AI32(0), y = AI32(1), w = AI32(2), h = AI32(3);
        if (n[2] == 'V') { g_vp[0] = x; g_vp[1] = y; g_vp[2] = w; g_vp[3] = h; }
        GLint X = (GLint)lroundf(x * g_sx), Y = (GLint)lroundf(y * g_sy);
        GLint W = (GLint)lroundf((x + w) * g_sx) - X, H = (GLint)lroundf((y + h) * g_sy) - Y;
        if (n[2] == 'V') { glViewport(X, Y, W, H); g_eye_vp[0] = X; g_eye_vp[1] = Y; g_eye_vp[2] = W; g_eye_vp[3] = H; }
        else { glScissor(X, Y, W, H); g_scissor[0] = X; g_scissor[1] = Y; g_scissor[2] = W; g_scissor[3] = H; }
        return 1;
    }
    if (k == K_FBTEX2D) {
        scale_target(A32(2), A32(3));
        return 0;
    }
    if (k == K_ACTIVETEX) { g_active_unit = A32(0); return 0; }
    if (k == K_BINDTEX) {
        unsigned u = g_active_unit - GL_TEXTURE0;
        if (A32(0) == GL_TEXTURE_RECTANGLE_ARB) {
            if (u == 0) g_unit0_rect = A32(1);
            if (u < 8) g_rect_bound[u] = A32(1);
        }
        return 0;
    }
    if ((k == K_ENABLE || k == K_DISABLE) && A32(0) == GL_TEXTURE_RECTANGLE_ARB) {
        unsigned bit = 1u << ((g_active_unit - GL_TEXTURE0) & 31);
        if (n[2] == 'E') g_rect_on |= bit; else g_rect_on &= ~bit;
        return 0;
    }
    if (k == K_ENABLE || k == K_DISABLE) {
        if (A32(0) == GL_SCISSOR_TEST) g_scissor_on = n[2] == 'E';
        if (A32(0) == GL_TEXTURE_2D && g_active_unit == GL_TEXTURE0) g_tex2d_on = n[2] == 'E';
        return 0;
    }
    if (g_on && (is_draw(k) || k == K_BEGIN)) {
        /* Screen content or not, decided per draw: 2D that is not a pass over
         * a render target, and 3D drawn into the HUD's target. */
        int persp = is_perspective(g_proj[g_proj_top]);
        if (persp && g_game_fb != g_hud_fb && !g_replaying) g_scene_draws++;
        if (persp && g_proj[g_proj_top][0] > g_lens_seen) g_lens_seen = g_proj[g_proj_top][0];
        /* A long lens - the sniper scope, a cutscene's close-up - is a
         * magnified view the eye's own frustum would undo: it goes on the
         * screen too, framed by the scope the game draws there. */
        int zoom = persp && g_proj[g_proj_top][0] > g_zoom_lens;
        if (persp && !g_replaying) { if (zoom) g_zoom_draws++; if (g_game_fb == g_hud_fb) g_hudfb_draws++; }
        int hud = persp ? g_game_fb == g_hud_fb || g_menu || zoom
                        : !g_unit0_rect || !covers_viewport(c, n);
        if (hud && !is_perspective(g_proj[g_proj_top]) && !g_replaying) g_hud_fb_next = g_game_fb;
        if (hud && !persp && letterbox_bar(c, n)) return 1;
        if (hud != g_loaded_hud) { g_loaded_hud = hud; load_projection(); }
        if (hud) clip_to_screen();
        fixed_function_rect();
        return 0;
    }
    if (k == K_MATRIXMODE) { g_mode = A32(0); return 0; }
    if (g_mode != GL_PROJECTION) return 0;

    float *top = g_proj[g_proj_top], m[16];
    if (k == K_LOADID) mat_identity(top);
    else if (k == K_LOADMATRIX) { const float *p = (const float *)APTR(0); if (p) memcpy(top, p, 64); }
    else if (k == K_MULTMATRIX) { const float *p = (const float *)APTR(0); if (p) mat_mul(top, top, p); }
    else if (k == K_ORTHO) { mat_ortho(m, gl_arg_d(c, 0), gl_arg_d(c, 2), gl_arg_d(c, 4), gl_arg_d(c, 6), gl_arg_d(c, 8), gl_arg_d(c, 10)); mat_mul(top, top, m); }
    else if (k == K_ORTHO2D) { mat_ortho(m, gl_arg_d(c, 0), gl_arg_d(c, 2), gl_arg_d(c, 4), gl_arg_d(c, 6), -1, 1); mat_mul(top, top, m); }
    else if (k == K_FRUSTUM) { mat_frustum(m, gl_arg_d(c, 0), gl_arg_d(c, 2), gl_arg_d(c, 4), gl_arg_d(c, 6), gl_arg_d(c, 8), gl_arg_d(c, 10)); mat_mul(top, top, m); }
    else if (k == K_PERSPECTIVE) {
        double zn = gl_arg_d(c, 4), t = zn * tan(gl_arg_d(c, 0) * M_PI / 360), r = t * gl_arg_d(c, 2);
        mat_frustum(m, -r, r, -t, t, zn, gl_arg_d(c, 6)); mat_mul(top, top, m);
    }
    else if (k == K_PUSHMATRIX) { if (g_proj_top < 31) { memcpy(g_proj[g_proj_top + 1], top, 64); g_proj_top++; } glPushMatrix(); return 1; }
    else if (k == K_POPMATRIX) { if (g_proj_top > 0) g_proj_top--; glPopMatrix(); load_projection(); return 1; }
    else if (k == K_TRANSLATE) { mat_identity(m); m[12] = argf(c, 0); m[13] = argf(c, 1); m[14] = argf(c, 2); mat_mul(top, top, m); }
    else if (k == K_SCALE) { mat_identity(m); m[0] = argf(c, 0); m[5] = argf(c, 1); m[10] = argf(c, 2); mat_mul(top, top, m); }
    else return 0;
    load_projection();
    return 1;
}

/* Answers the game must get in its own terms. */
static void fix_query(CPU *c, int k)
{
    if (k == K_GETINTEGERV && A32(0) == GL_VIEWPORT) {
        GLint *o = (GLint *)APTR(1); if (o) memcpy(o, g_vp, sizeof g_vp);
    } else if (k == K_GETINTEGERV && (A32(0) == GL_FRAMEBUFFER_BINDING)) {
        GLint *o = (GLint *)APTR(1); if (o) *o = (GLint)g_game_fb;
    } else if (k == K_GETFLOATV && A32(0) == GL_PROJECTION_MATRIX) {
        float *o = (float *)APTR(1); if (o) memcpy(o, g_proj[g_proj_top], 64);
    } else if (k == K_GETFLOATV && A32(0) == GL_VIEWPORT) {
        float *o = (float *)APTR(1); if (o) for (int i = 0; i < 4; i++) o[i] = (float)g_vp[i];
    }
}

static void exec(CPU *c, HleId id, HleHandler fn)
{
    int k = cls(id)->kind;
    if (!transform_call(c, id, hle_name(id))) fn(c);
    if (is_draw(k) || k == K_END) { fixed_function_rect_done(); clip_done(); }
    if (!g_replaying && (k == K_GETINTEGERV || k == K_GETFLOATV)) fix_query(c, k);
}

/* ---- recording ---- */
static uint32_t arena_take(uint32_t n)
{
    n = (n + 15u) & ~15u;
    if (g_arena_used + n > ARENA_SIZE) return 0;
    uint32_t va = g_arena + g_arena_used;
    g_arena_used += n;
    return va;
}

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

/* Calls whose effect outlives the frame, or whose answer the game already has. */
static int live_only(const char *n)
{
    return starts(n, "glGet") || starts(n, "glIs") || starts(n, "glGen") || starts(n, "glDelete") ||
           starts(n, "glCheckFramebufferStatus") || starts(n, "glTexImage") || starts(n, "glTexSubImage") ||
           starts(n, "glCompressedTex") || starts(n, "glMapBuffer") || starts(n, "glProgramString") ||
           starts(n, "glShaderSource") || starts(n, "glCompileShader") || starts(n, "glLinkProgram") ||
           starts(n, "glCreate") || starts(n, "glAttach") || starts(n, "glFramebufferTexture") ||
           starts(n, "glRenderbufferStorage") || starts(n, "glReadPixels") || starts(n, "glFinish") ||
           starts(n, "glFlush");
}

/* Arguments the game passes by pointer that are small enough to copy:
 * (argument index, bytes). Bytes 0: not a pointer worth copying. */
static void pointer_arg(const char *n, int *idx, uint32_t *bytes, uint8_t *kind)
{
    static const struct { const char *name; int idx; uint32_t bytes; } T[] = {
        { "glLoadMatrixf", 0, 64 }, { "glMultMatrixf", 0, 64 },
        { "glTexEnvfv", 2, 16 }, { "glTexEnviv", 2, 16 }, { "glFogfv", 1, 16 }, { "glFogiv", 1, 16 },
        { "glColor4ubv", 0, 4 }, { "glColor3ubv", 0, 3 }, { "glColor4fv", 0, 16 }, { "glColor3fv", 0, 12 },
        { "glTexParameterfv", 2, 16 }, { "glTexParameteriv", 2, 16 },
        { "glProgramEnvParameter4fvARB", 2, 16 }, { "glProgramLocalParameter4fvARB", 2, 16 },
        { "glLightfv", 2, 16 }, { "glLightModelfv", 1, 16 }, { "glMaterialfv", 2, 16 },
        { "glPointParameterfvARB", 1, 12 }, { "glNormal3fv", 0, 12 }, { "glVertex3fv", 0, 12 },
        { "glVertex2fv", 0, 8 }, { "glTexCoord2fv", 0, 8 }, { "glMultiTexCoord2fvARB", 1, 8 },
        { "glMultiTexCoord2fv", 1, 8 }, { "glVertexAttrib4fvARB", 1, 16 }, { "glClipPlane", 1, 32 },
    };
    *idx = -1; *bytes = 0; *kind = 0;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(n, T[i].name)) { *idx = T[i].idx; *bytes = T[i].bytes; return; }
    if (!strcmp(n, "glBufferDataARB") || !strcmp(n, "glBufferData")) { *idx = 2; *kind = 1; }
    else if (!strcmp(n, "glBufferSubDataARB") || !strcmp(n, "glBufferSubData")) { *idx = 3; *kind = 2; }
}

static int type_bytes(GLenum t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_DOUBLE: return 8;
    default: return 4;
    }
}

static int client_array(GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY: return ARR_VERTEX;
    case GL_NORMAL_ARRAY: return ARR_NORMAL;
    case GL_COLOR_ARRAY: return ARR_COLOR;
    case GL_TEXTURE_COORD_ARRAY: return ARR_TEX0 + (g_client_unit & 7);
    }
    return -1;
}

/* Keeps g_arr in step with the game's calls (recording only). */
static void track_arrays(CPU *c, const char *n)
{
    Arr *a = NULL;
    if (!strcmp(n, "glEnableClientState") || !strcmp(n, "glDisableClientState")) {
        int i = client_array(A32(0)); if (i >= 0) g_arr[i].on = n[2] == 'E';
    } else if (!strcmp(n, "glClientActiveTexture") || !strcmp(n, "glClientActiveTextureARB")) {
        g_client_unit = (int)(A32(0) - GL_TEXTURE0);
    } else if (!strcmp(n, "glEnableVertexAttribArrayARB") || !strcmp(n, "glDisableVertexAttribArrayARB")) {
        if (A32(0) < 16) g_arr[ARR_ATTR0 + A32(0)].on = n[2] == 'E';
    } else if (!strcmp(n, "glVertexPointer")) { a = &g_arr[ARR_VERTEX]; a->size = A32(0); a->type = A32(1); a->stride = AI32(2); a->ptr = A32(3); }
    else if (!strcmp(n, "glNormalPointer")) { a = &g_arr[ARR_NORMAL]; a->size = 3; a->type = A32(0); a->stride = AI32(1); a->ptr = A32(2); }
    else if (!strcmp(n, "glColorPointer")) { a = &g_arr[ARR_COLOR]; a->size = A32(0); a->type = A32(1); a->stride = AI32(2); a->ptr = A32(3); }
    else if (!strcmp(n, "glTexCoordPointer")) { a = &g_arr[ARR_TEX0 + (g_client_unit & 7)]; a->size = A32(0); a->type = A32(1); a->stride = AI32(2); a->ptr = A32(3); }
    else if (!strcmp(n, "glVertexAttribPointerARB")) {
        if (A32(0) >= 16) return;
        a = &g_arr[ARR_ATTR0 + A32(0)]; a->size = A32(1); a->type = A32(2); a->norm = A32(3); a->stride = AI32(4); a->ptr = A32(5);
    }
    if (a) a->vbo = gl_bound_buffer(GL_ARRAY_BUFFER);
}

static Op *op_new(void);

/* Before a draw is recorded: copy the client memory it reads. */
static int snapshot_draw(CPU *c, int kind, uint32_t *args_va)
{
    int elements = kind == K_DRAWELEMENTS;
    if (!elements && kind != K_DRAWARRAYS) return 1;
    uint32_t lo, hi;
    if (!elements) {
        if (AI32(2) <= 0) return 1;
        lo = A32(1); hi = lo + A32(2) - 1;
    } else {
        GLsizei count = AI32(1); GLenum type = A32(2);
        if (count <= 0) return 1;
        int ib = type_bytes(type);
        const void *idx; void *tmp = NULL;
        if (gl_bound_buffer(GL_ELEMENT_ARRAY_BUFFER)) {
            tmp = malloc((size_t)count * ib);
            if (!tmp) return 0;
            glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)A32(3), (GLsizeiptr)count * ib, tmp);
            idx = tmp;
        } else {
            uint32_t copy = arena_take((uint32_t)count * (uint32_t)ib);
            if (!copy) return 0;
            memcpy(G2H(copy), GP(A32(3)), (size_t)count * ib);
            wr32(*args_va + 12, copy);             /* the draw reads the copy */
            idx = G2H(copy);
        }
        lo = 0xFFFFFFFFu; hi = 0;
        for (GLsizei i = 0; i < count; i++) {
            uint32_t v = ib == 1 ? ((const uint8_t *)idx)[i] : ib == 2 ? ((const uint16_t *)idx)[i] : ((const uint32_t *)idx)[i];
            if (v < lo) lo = v; if (v > hi) hi = v;
        }
        free(tmp);
    }
    for (int i = 0; i < ARR_COUNT; i++) {
        Arr *a = &g_arr[i];
        if (!a->on || a->vbo || !a->ptr) continue;
        uint32_t elem = (uint32_t)(a->size * type_bytes(a->type));
        uint32_t stride = a->stride ? (uint32_t)a->stride : elem;
        uint32_t bytes = (hi - lo) * stride + elem;
        uint32_t copy = arena_take(bytes);
        if (!copy) return 0;
        if (g_nfmt == g_fmtcap) {
            unsigned cap = g_fmtcap ? g_fmtcap * 2 : 4096;
            Arr *f = (Arr *)realloc(g_ptr_fmt, cap * sizeof *f);
            if (!f) return 0;
            g_ptr_fmt = f; g_fmtcap = cap;
        }
        Op *o = op_new();
        if (!o) return 0;
        memcpy(G2H(copy), GP(a->ptr + lo * stride), bytes);
        g_ptr_fmt[g_nfmt] = *a;
        o->kind = OP_PTR; o->target = (GLenum)i; o->va = copy - lo * stride; o->n = g_nfmt++;
        o->id = (HleId)0; o->fn = NULL;
    }
    return 1;
}

/* Replay: point array i at a copy. */
static void replay_pointer(int i, uint32_t va)
{
    Arr *a = &g_arr[i];
    GLint vbo = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
    if (vbo) glBindBuffer(GL_ARRAY_BUFFER, 0);
    const void *p = G2H(va);
    if (i == ARR_VERTEX) glVertexPointer(a->size, a->type, a->stride, p);
    else if (i == ARR_NORMAL) glNormalPointer(a->type, a->stride, p);
    else if (i == ARR_COLOR) glColorPointer(a->size, a->type, a->stride, p);
    else if (i < ARR_ATTR0) {
        GLint cur = 0; glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &cur);
        glClientActiveTexture(GL_TEXTURE0 + (i - ARR_TEX0));
        glTexCoordPointer(a->size, a->type, a->stride, p);
        glClientActiveTexture((GLenum)cur);
    } else glVertexAttribPointerARB((GLuint)(i - ARR_ATTR0), a->size, a->type, (GLboolean)a->norm, a->stride, p);
    if (vbo) glBindBuffer(GL_ARRAY_BUFFER, (GLuint)vbo);
}

static Op *op_new(void)
{
    if (g_nops == g_cap) {
        unsigned cap = g_cap ? g_cap * 2 : 32768;
        Op *o = (Op *)realloc(g_ops, cap * sizeof *o);
        if (!o) return NULL;
        g_ops = o; g_cap = cap;
    }
    return &g_ops[g_nops++];
}

static void record(CPU *c, HleId id, HleHandler fn, const char *n)
{
    const Cls *k = cls(id);
    if (k->kind == K_UNMAP) {
        /* The data the game wrote into the mapping, as it stood at unmap. */
        void *shadow; int size;
        if (!gl_map_shadow(A32(0), &shadow, &size) || size <= 0) return;
        uint32_t va = arena_take((uint32_t)size);
        Op *o = va ? op_new() : NULL;
        if (!o) { g_overflow++; return; }
        memcpy(G2H(va), shadow, (size_t)size);
        o->kind = OP_UNMAP; o->target = A32(0); o->va = va; o->n = (uint32_t)size;
        return;
    }
    if (k->live) return;
    if (k->track) track_arrays(c, n);
    uint32_t va = arena_take(ARGS * 4);
    if (!va) { g_overflow++; return; }
    memcpy(G2H(va), G2H(c->esp), ARGS * 4);
    /* the copies of the draw's vertices go in before the draw itself */
    if (!snapshot_draw(c, k->kind, &va)) { g_overflow++; return; }
    Op *o = op_new();
    if (!o) { g_overflow++; return; }
    int idx = k->pidx;
    uint32_t bytes = k->pkind == 1 ? A32(1) : k->pkind == 2 ? A32(2) : k->pbytes;
    if (idx >= 0 && bytes && A32(idx)) {
        if (bytes > (4u << 20)) { g_nops--; return; }       /* a load-time upload: not per frame */
        uint32_t copy = arena_take(bytes);
        if (!copy) { g_nops--; g_overflow++; return; }
        memcpy(G2H(copy), GP(A32(idx)), bytes);
        wr32(va + 4u * (uint32_t)idx, copy);
    }
    o->kind = OP_CALL; o->id = id; o->fn = fn; o->va = va;
}

/* With LINDBERGH_RT_DUMP: on that frame, the centre pixel of the target
 * after every draw, to compare one GL's sequence with another's. */
static void trace_draw(const char *n)
{
    static long at = -2;
    if (at == -2) { const char *v = getenv("LINDBERGH_RT_DUMP"); at = v && *v ? atol(v) : -1; }
#ifdef LINDBERGH_GL4ES
    gl4es_mv_trace = (long)g_frame == at && getenv("LIBGL_MV_CHECK");
#endif
    if ((long)g_frame != at || (strncmp(n, "glDraw", 6) && strcmp(n, "glEnd"))) return;
    static unsigned k;
    unsigned char px[4] = { 0, 0, 0, 0 };
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    /* LINDBERGH_PX_AT=x,y: where, as fractions of the viewport (the centre) */
    static float fx = -1, fy;
    if (fx < 0) { const char *a = getenv("LINDBERGH_PX_AT"); fx = 0.5f; fy = 0.5f; if (a) sscanf(a, "%f,%f", &fx, &fy); }
    GLint qx = vp[0] + (GLint)(vp[2] * fx), qy = vp[1] + (GLint)(vp[3] * fy);
#ifdef LINDBERGH_GL4ES
    if (g_mv) gl4es_multiview_read_pixel(qx, qy, px);
    else
#endif
    glReadPixels(qx, qy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLint dfn = 0; GLboolean dwm = 0;
    glGetIntegerv(GL_DEPTH_FUNC, &dfn); glGetBooleanv(GL_DEPTH_WRITEMASK, &dwm);
    int von = 0, fon = 0;
    arb_is_enabled(GL_VERTEX_PROGRAM_ARB, &von); arb_is_enabled(GL_FRAGMENT_PROGRAM_ARB, &fon);
    GLuint vpid = von ? arb_bound(GL_VERTEX_PROGRAM_ARB) : 0, fpid = fon ? arb_bound(GL_FRAGMENT_PROGRAM_ARB) : 0;
    fprintf(stderr, "[px] %u fb %u %s rect %u hud %d -> %02x%02x%02x%02x  depth %d %#x %d cull %d vp %u fp %u\n", k++, g_game_fb, n,
            g_unit0_rect, g_loaded_hud, px[0], px[1], px[2], px[3], glIsEnabled(GL_DEPTH_TEST), dfn, dwm, glIsEnabled(GL_CULL_FACE), vpid, fpid);
    static unsigned char seen[4096];
    if (vpid && vpid < 4096 && !seen[vpid]) {
        seen[vpid] = 1;
        const char *src = arb_get_source(GL_VERTEX_PROGRAM_ARB, vpid);
        fprintf(stderr, "[px] vertex program %u:\n%s\n[px] end\n", vpid, src ? src : "(none)");
    }
    static unsigned char fseen[4096];
    if (fpid && fpid < 4096 && !fseen[fpid]) {
        fseen[fpid] = 1;
        const char *src = arb_get_source(GL_FRAGMENT_PROGRAM_ARB, fpid);
        fprintf(stderr, "[px] fragment program %u:\n%s\n[px] end\n", fpid, src ? src : "(none)");
    }
    if (!g_game_fb) {
        GLint sb[4] = { 0 };
        glGetIntegerv(GL_SCISSOR_BOX, sb);
        fprintf(stderr, "[px]   eye %d viewport %d,%d %dx%d scissor %d %d,%d %dx%d tan %.2f %.2f %.2f %.2f persp %.3f %.3f screen %.1f m\n",
                g_eye, vp[0], vp[1], vp[2], vp[3], glIsEnabled(GL_SCISSOR_TEST), sb[0], sb[1], sb[2], sb[3],
                g_tan[g_eye][0], g_tan[g_eye][1], g_tan[g_eye][2], g_tan[g_eye][3],
                g_last_persp[0], g_last_persp[5], g_screen_dist);
        GLint df = 0, bs = 0, bd = 0, af = 0; GLboolean dm = 0, cm[4] = { 0 };
        glGetIntegerv(GL_DEPTH_FUNC, &df); glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
        glGetIntegerv(GL_BLEND_SRC, &bs); glGetIntegerv(GL_BLEND_DST, &bd);
        glGetBooleanv(GL_COLOR_WRITEMASK, cm); glGetIntegerv(GL_ALPHA_TEST_FUNC, &af);
        fprintf(stderr, "[px]   depth %d func %#x mask %d  blend %d %#x %#x  alpha test %d %#x  colour mask %d%d%d%d  cull %d stencil %d\n",
                glIsEnabled(GL_DEPTH_TEST), df, dm, glIsEnabled(GL_BLEND), bs, bd, glIsEnabled(GL_ALPHA_TEST), af,
                cm[0], cm[1], cm[2], cm[3], glIsEnabled(GL_CULL_FACE), glIsEnabled(GL_STENCIL_TEST));
#ifdef LINDBERGH_GL4ES
        {   /* the texture as the GLES driver sees it */
            typedef void (*Iv)(GLenum, GLint *);
            typedef void (*TIv)(GLenum, GLenum, GLint *);
            typedef void (*TLIv)(GLenum, GLint, GLenum, GLint *);
            typedef void (*Act)(GLenum);
            Iv get = (Iv)eglGetProcAddress("glGetIntegerv");
            TIv tp = (TIv)eglGetProcAddress("glGetTexParameteriv");
            TLIv tl = (TLIv)eglGetProcAddress("glGetTexLevelParameteriv");
            Act act = (Act)eglGetProcAddress("glActiveTexture");
            GLint au = 0; get(GL_ACTIVE_TEXTURE, &au); act(GL_TEXTURE0);
            GLint b = 0, mn = 0, mg = 0, ws = 0, wt = 0, w = 0, h = 0, w1 = 0, base = 0, maxl = 0, prog = 0;
            get(GL_TEXTURE_BINDING_2D, &b); get(GL_CURRENT_PROGRAM, &prog);
            tp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mn); tp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mg);
            tp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &ws); tp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wt);
            tp(GL_TEXTURE_2D, 0x813C /* BASE_LEVEL */, &base); tp(GL_TEXTURE_2D, 0x813D /* MAX_LEVEL */, &maxl);
            if (tl) { tl(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w); tl(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h); tl(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &w1); }
            act((GLenum)au);
            fprintf(stderr, "[px]   GLES unit 0: texture %d %dx%d (level 1 %d) min %#x mag %#x wrap %#x %#x levels %d..%d, program %d\n",
                    b, w, h, w1, mn, mg, ws, wt, base, maxl, prog);
            if (g_mv) {     /* the arrays each unit has, and where the program's samplers look */
                typedef GLint (*Loc)(GLuint, const char *);
                typedef void (*GU)(GLuint, GLint, GLint *);
                /* the driver's own: eglGetProcAddress may hand back gl4es' */
                void *lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_NOLOAD);
                if (!lib) lib = dlopen("libGLESv3.so", RTLD_NOW);
                Loc loc = (Loc)dlsym(lib, "glGetUniformLocation");
                GU gu = (GU)dlsym(lib, "glGetUniformiv");
                act = (Act)dlsym(lib, "glActiveTexture");
                get = (Iv)dlsym(lib, "glGetIntegerv");
                tp = (TIv)dlsym(lib, "glGetTexParameteriv");
                tl = (TLIv)dlsym(lib, "glGetTexLevelParameteriv");
                get(GL_ACTIVE_TEXTURE, &au);
                get(GL_CURRENT_PROGRAM, &prog);
                for (int u = 0; u < 4; u++) {
                    GLint a = 0, smin = 0, l = 0, v = -1, aw = 0, ah = 0, ad = 0, af = 0;
                    char nm[8];
                    act(GL_TEXTURE0 + u); get(0x8C1D /*GL_TEXTURE_BINDING_2D_ARRAY*/, &a);
                    if (a) {
                        tp(0x8C1A, GL_TEXTURE_MIN_FILTER, &smin);
                        tl(0x8C1A, 0, GL_TEXTURE_WIDTH, &aw); tl(0x8C1A, 0, GL_TEXTURE_HEIGHT, &ah);
                        tl(0x8C1A, 0, 0x8071 /*GL_TEXTURE_DEPTH*/, &ad); tl(0x8C1A, 0, GL_TEXTURE_INTERNAL_FORMAT, &af);
                    }
                    snprintf(nm, sizeof nm, "sr_%d", u);
                    l = loc((GLuint)prog, nm);
                    if (l >= 0) gu((GLuint)prog, l, &v);
                    fprintf(stderr, "[px]   unit %d: array %d %dx%dx%d format %#x min %#x; sampler sr_%d at %d = %d\n", u, a, aw, ah, ad, af, smin, u, l, v);
                }
                typedef void (*GUf)(GLuint, GLint, GLfloat *);
                GUf guf = (GUf)dlsym(lib, "glGetUniformfv");
                for (int u = 0; u < 4; u++) {
                    char nm[24]; snprintf(nm, sizeof nm, "u_rect_norm[%d]", u);
                    GLint l = loc((GLuint)prog, nm); GLfloat v[2] = { -1, -1 };
                    if (l >= 0) guf((GLuint)prog, l, v);
                    fprintf(stderr, "[px]   %s at %d = %g %g\n", nm, l, v[0], v[1]);
                }
                act((GLenum)au);
            }
        }
#endif
    }
    Arr *col = &g_arr[ARR_COLOR], *tc = &g_arr[ARR_TEX0];
    if (g_unit0_rect && col->on && col->ptr && !col->vbo && col->type == GL_UNSIGNED_BYTE) {
        const unsigned char *b = (const unsigned char *)GP(col->ptr);
        const float *t = tc->on && tc->ptr ? (const float *)GP(tc->ptr) : NULL;
        int ts = tc->stride ? tc->stride / 4 : 2;
        GLint active = 0, env = 0, bound = 0, w = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        int on_rect = glIsEnabled(GL_TEXTURE_RECTANGLE_ARB), on_2d = glIsEnabled(GL_TEXTURE_2D);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
        glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB, &bound);
        glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE_ARB, 0, GL_TEXTURE_WIDTH, &w);
        glActiveTexture((GLenum)active);
        fprintf(stderr, "[px]   colours %02x%02x%02x%02x %02x%02x%02x%02x  uv %g,%g %g,%g  unit0: rect %d 2d %d env %#x bound %d w %d\n",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                t ? t[0] : -1, t ? t[1] : -1, t ? t[2 * ts] : -1, t ? t[2 * ts + 1] : -1, on_rect, on_2d, env, bound, w);
    }
}

/* LINDBERGH_GLPROF: the time each GL import takes, game's calls only (the
 * second eye's replay is timed by the host), summed and printed every 300
 * frames with the costliest few. */
static int g_glprof = -1;
static double g_glprof_ns[HLE_COUNT];
static unsigned g_glprof_n[HLE_COUNT];
static unsigned g_glprof_f0;
static double g_glprof_rec_ns;          /* of which recording for the second eye */
static double g_frame_ns[HLE_COUNT];    /* this frame's, to name what a slow frame spent */
static double g_frame_t0;
static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

static void glprof_frame(void)
{
    double now = now_ns(), all = g_frame_t0 ? now - g_frame_t0 : 0, gl = 0;
    unsigned best = 0;
    for (unsigned i = 0; i < HLE_COUNT; i++) { gl += g_frame_ns[i]; if (g_frame_ns[i] > g_frame_ns[best]) best = i; }
    if (all > 40e6)
        fprintf(stderr, "[glprof] frame %u took %.0f ms, GL %.0f ms of it, most in %s (%.0f ms)\n", g_frame,
                all / 1e6, gl / 1e6, hle_name((HleId)best), g_frame_ns[best] / 1e6);
#ifdef LINDBERGH_GL4ES
    {   /* buffer uploads: the driver's, and gl4es' own copy */
        extern double gl4es_drawprof[4], gl4es_bufprof_copy, gl4es_bufprof_bytes;
        if (all > 40e6 && gl4es_bufprof_bytes > 0)
            fprintf(stderr, "[glprof]   buffers: %.1f MB, driver %.0f ms, gl4es' copy %.0f ms\n",
                    gl4es_bufprof_bytes / 1e6, gl4es_drawprof[3] / 1e6, gl4es_bufprof_copy / 1e6);
        gl4es_bufprof_bytes = gl4es_bufprof_copy = gl4es_drawprof[3] = 0;
    }
#endif
    memset(g_frame_ns, 0, sizeof g_frame_ns);
    g_frame_t0 = now;
}

static void glprof_report(void)
{
    if (g_glprof > 0) glprof_frame();
    if (g_glprof <= 0 || g_frame - g_glprof_f0 < 300) return;
    double frames = g_frame - g_glprof_f0, total = 0, calls = 0;
    for (unsigned i = 0; i < HLE_COUNT; i++) total += g_glprof_ns[i], calls += g_glprof_n[i];
    extern double g_arb_draw_ns;
    fprintf(stderr, "[glprof] frames %u..%u: GL %.1f ms (recording %.1f, ARB programs %.1f), %.0f calls per frame; costliest:",
            g_glprof_f0, g_frame, total / frames / 1e6, g_glprof_rec_ns / frames / 1e6, g_arb_draw_ns / frames / 1e6, calls / frames);
    g_glprof_rec_ns = g_arb_draw_ns = 0;
    for (int k = 0; k < 10; k++) {
        unsigned best = 0;
        for (unsigned i = 1; i < HLE_COUNT; i++) if (g_glprof_ns[i] > g_glprof_ns[best]) best = i;
        if (g_glprof_ns[best] <= 0) break;
        fprintf(stderr, " %s %.2f ms/%.0f", hle_name((HleId)best), g_glprof_ns[best] / frames / 1e6, g_glprof_n[best] / frames);
        g_glprof_ns[best] = -g_glprof_ns[best];
    }
    fputc('\n', stderr);
#ifdef LINDBERGH_GL4ES
    {   /* inside gl4es' draws */
        extern double gl4es_drawprof[4];
        fprintf(stderr, "[glprof]   in gl4es' draws, ms per frame: textures %.2f, program and uniforms %.2f, the driver's draw %.2f\n",
                gl4es_drawprof[0] / frames / 1e6, gl4es_drawprof[1] / frames / 1e6, gl4es_drawprof[2] / frames / 1e6);
        memset(gl4es_drawprof, 0, 4 * sizeof(double));
    }
#endif
    memset(g_glprof_ns, 0, sizeof g_glprof_ns);
    memset(g_glprof_n, 0, sizeof g_glprof_n);
    g_glprof_f0 = g_frame;
}

int stereo_glprof_on(void)
{
    if (g_glprof < 0) {
        const char *v = getenv("LINDBERGH_GLPROF"); g_glprof = v && *v && *v != '0';
#ifdef LINDBERGH_GL4ES
        extern int gl4es_drawprof_on;
        gl4es_drawprof_on = g_glprof;
#endif
    }
    return g_glprof;
}

static void stereo_gl_call_inner(CPU *c, HleId id, HleHandler fn);
static void stereo_gl_call(CPU *c, HleId id, HleHandler fn)
{
    stereo_glprof_on();
    if (!g_glprof) { stereo_gl_call_inner(c, id, fn); return; }
    double t0 = now_ns();
    stereo_gl_call_inner(c, id, fn);
    double dt = now_ns() - t0;
    g_glprof_ns[id] += dt;
    g_frame_ns[id] += dt;
    g_glprof_n[id]++;
}

static void stereo_gl_call_inner(CPU *c, HleId id, HleHandler fn)
{
    if ((long)g_frame == g_map_frame) {
        uint32_t a[6]; for (int i = 0; i < 6; i++) a[i] = A32(i);
        const char *nm = hle_name(id);
        if (!strncmp(nm, "glDraw", 6) || !strcmp(nm, "glBegin")) {
            /* what a draw has bound, to tell a full-screen pass from the HUD */
            GLint t2 = 0, tr = 0, fp = 0, vp = 0, unit = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &unit); glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &t2); glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB, &tr);
            glActiveTexture((GLenum)unit);
            glGetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB, &fp);
            glGetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB, &vp);
            int fpon = 0, vpon = 0; arb_is_enabled(GL_FRAGMENT_PROGRAM_ARB, &fpon); arb_is_enabled(GL_VERTEX_PROGRAM_ARB, &vpon);
            float b[4] = { 0, 0, 0, 0 };
            int boxed = !is_perspective(g_proj[g_proj_top]) && ndc_box(c, nm, b);
            fprintf(stderr, "[map] %s %#x %#x %#x | fb %u ortho %d tex2d %d%s rect %d fp %d%s vp %d%s box %.2f,%.2f..%.2f,%.2f%s\n", nm, a[0], a[1], a[2],
                    g_game_fb, !is_perspective(g_proj[g_proj_top]), t2, g_tex2d_on ? "" : "(off)", tr, fp, fpon ? "" : "(off)", vp, vpon ? "" : "(off)",
                    b[0], b[1], b[2], b[3], boxed ? "" : "?");
        } else
            fprintf(stderr, "[map] %s %#x %#x %#x %#x %#x %#x\n", nm, a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    if (!g_on) { fn(c); return; }
    const char *n = hle_name(id);
#ifdef LINDBERGH_GL4ES
    if (gl4es_mv_trace) {   /* the dump frame with LIBGL_MV_CHECK: which call fails */
        static GLenum (*err)(void);
        if (!err) { void *lib = dlopen("libGLESv3.so", RTLD_NOW); err = (GLenum (*)(void))dlsym(lib, "glGetError"); }
        while (err() != GL_NO_ERROR) ;
        if (!g_mv) record(c, id, fn, n);
        exec(c, id, fn);
        GLenum e = err();
        if (e) fprintf(stderr, "[mv] %s %#x %#x %#x -> GL error %#x\n", n, A32(0), A32(1), A32(2), e);
        trace_draw(n);
        return;
    }
#endif
    if (!g_mv) {
        if (g_glprof > 0) {
            double t0 = now_ns();
            record(c, id, fn, n);
            g_glprof_rec_ns += now_ns() - t0;
        } else record(c, id, fn, n);
    }
    exec(c, id, fn);
    trace_draw(n);
}

/* ---- per frame ---- */
static void replay(int eye)
{
    g_eye = eye;
    g_replaying = 1;
    Arr live[ARR_COUNT];
    memcpy(live, g_arr, sizeof live);
    CPU cpu;
    for (unsigned i = 0; i < g_nops; i++) {
        Op *o = &g_ops[i];
        if (o->kind == OP_UNMAP) { glBufferSubData(o->target, 0, (GLsizeiptr)o->n, G2H(o->va)); continue; }
        if (o->kind == OP_PTR) {
            g_arr[o->target] = g_ptr_fmt[o->n];
            replay_pointer((int)o->target, o->va);
            g_arr[o->target].ptr = o->va;       /* what the draw reads now */
            continue;
        }
        memset(&cpu, 0, sizeof cpu);
        cpu.esp = o->va;
        exec(&cpu, o->id, o->fn);
    }
    memcpy(g_arr, live, sizeof live);
    g_replaying = 0;
}

/* LINDBERGH_RT_DUMP=<frame>: every render target and the first eye's image,
 * as the first eye left them that frame, to rt_<texture>.ppm - for finding
 * which pass of a frame goes wrong on a GL that cannot be watched. */
static void write_ppm(const char *name, int w, int h, const unsigned char *rgba)
{
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)
        for (int x = 0; x < w; x++) fwrite(rgba + ((size_t)y * w + x) * 4, 1, 3, f);
    fclose(f);
}

static void dump_targets(void)
{
    static long at = -2;
    if (at == -2) { const char *v = getenv("LINDBERGH_RT_DUMP"); at = v && *v ? atol(v) : -1; }
    if ((long)g_frame != at) return;
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    for (unsigned i = 0; i <= g_nscaled; i++) {
        GLuint tex = i < g_nscaled ? g_scaled[i] : g_eye_tex[0];
        GLenum tg = GL_TEXTURE_2D;
        GLint w = 0, h = 0, ifmt = 0, was = 0;
        for (int k = 0; k < 2 && !w; k++) {
            tg = k ? GL_TEXTURE_RECTANGLE_ARB : GL_TEXTURE_2D;
            glGetIntegerv(k ? GL_TEXTURE_BINDING_RECTANGLE_ARB : GL_TEXTURE_BINDING_2D, &was);
            glBindTexture(tg, tex);
            glGetTexLevelParameteriv(tg, 0, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(tg, 0, GL_TEXTURE_HEIGHT, &h);
            glGetTexLevelParameteriv(tg, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
            glBindTexture(tg, (GLuint)was);
        }
        if (w <= 0 || h <= 0 || ifmt == GL_DEPTH_COMPONENT || ifmt == GL_DEPTH_COMPONENT24 ||
            ifmt == GL_DEPTH_COMPONENT16 || ifmt == GL_DEPTH_COMPONENT32 || ifmt == GL_DEPTH24_STENCIL8) continue;
        unsigned char *px = malloc((size_t)w * h * 4);
        if (!px) continue;
#ifdef LINDBERGH_GL4ES
        if (g_mv) {     /* the left eye's layer */
            if (!gl4es_multiview_read(tex, tg, 0, px)) { free(px); continue; }
        } else
#endif
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tg, tex, 0);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        }
        char name[64];
        snprintf(name, sizeof name, i < g_nscaled ? "rt_%u.ppm" : "rt_eye.ppm", tex);
        write_ppm(name, w, h, px);
        free(px);
        fprintf(stderr, "[stereo] frame %u: wrote %s (%dx%d%s, status %#x)\n", g_frame, name, w, h,
                tg == GL_TEXTURE_2D ? "" : " rect", glCheckFramebufferStatus(GL_FRAMEBUFFER));
    }
#ifdef LINDBERGH_GL4ES
    /* LINDBERGH_DUMP_TEX=a,b,...: rectangle textures that are not render
     * targets too, as their first layer holds them */
    const char *xt = getenv("LINDBERGH_DUMP_TEX");
    for (const char *q = xt; g_mv && q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : NULL) {
        GLuint tex = (GLuint)atoi(q);
        GLint w = 0, h = 0, was = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB, &was);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
        glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE_ARB, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE_ARB, 0, GL_TEXTURE_HEIGHT, &h);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, (GLuint)was);
        unsigned char *px = w > 0 && h > 0 ? malloc((size_t)w * h * 4) : NULL;
        if (px && gl4es_multiview_read(tex, GL_TEXTURE_RECTANGLE_ARB, 0, px)) {
            char name[64];
            snprintf(name, sizeof name, "tex_%u.ppm", tex);
            write_ppm(name, w, h, px);
            fprintf(stderr, "[stereo] frame %u: wrote %s (%dx%d), first texels %d %d %d, middle %d %d %d\n", g_frame, name, w, h,
                    px[0], px[1], px[2], px[(w / 2) * 4], px[(w / 2) * 4 + 1], px[(w / 2) * 4 + 2]);
        }
        free(px);
    }
#endif
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
    glDeleteFramebuffers(1, &fb);
}

void stereo_frame_end(void)
{
    if (!g_on) return;
    dump_targets();
    if (!g_mv) replay(1);
    if (g_overflow) { fprintf(stderr, "[stereo] frame %u: recording overflowed %u times\n", g_frame, g_overflow); g_overflow = 0; }
}

void stereo_frame_begin(void)
{
    glprof_report();
    if (!g_on) return;
    g_nops = 0;
    g_nfmt = 0;
    g_arena_used = 0;
    /* Menus put a few 3D models among 2D panels and the arcade monitor's edge
     * framed them; the game world takes far more draws. Half a second of
     * menu-sized frames turns the 3D into screen content, a quarter second
     * of world-sized ones turns it back (one busy frame used to flip it). */
    static unsigned world_frames;
    if (g_scene_draws >= g_menu_draws) {
        g_menu_frames = 0;
        if (++world_frames >= 15) g_menu = 0;
    } else {
        world_frames = 0;
        if (++g_menu_frames >= 30) g_menu = 1;
    }
    if (getenv("LINDBERGH_SCENE_STATS") && g_frame % 50 == 0)
        fprintf(stderr, "[stereo] frame %u: %u world draws%s, lens %.2f\n", g_frame, g_scene_draws, g_menu ? " (menu)" : "", g_lens_seen);
    static int mode_log = -1, last_sig = -1;
    if (mode_log < 0) { const char *v = getenv("LINDBERGH_MODE_LOG"); mode_log = v && *v && *v != '0'; }
    if (mode_log) {
        int sig = g_menu | (g_zoom_draws ? 2 : 0) | (g_hudfb_draws ? 4 : 0);
        if (sig != last_sig)
            fprintf(stderr, "[mode] frame %u: menu %d, zoom draws %u, 3D into HUD target %u, world draws %u, lens %.2f\n",
                    g_frame, g_menu, g_zoom_draws, g_hudfb_draws, g_scene_draws, g_lens_seen);
        last_sig = sig;
    }
    g_zoom_draws = g_hudfb_draws = 0;
    g_lens_seen = 0;
    g_scene_draws = 0;
    g_hud_fb = g_hud_fb_next;           /* 3D drawn there this frame is screen content */
    g_hud_fb_next = ~0u;
    g_eye = 0;
    /* The game's state carries over from the end of the frame; only the
     * framebuffer and projection depend on the eye. */
    eye_targets();
    glBindFramebuffer(GL_FRAMEBUFFER, g_game_fb ? g_game_fb : g_eye_fbo[0]);
    GLenum mode = g_mode;
    g_mode = GL_MODELVIEW;                     /* load_projection restores this */
    glMatrixMode(GL_PROJECTION);
    g_mode = GL_PROJECTION;
    load_projection();
    g_mode = mode;
    glMatrixMode(mode);
}

/* ---- the host's side ---- */
int stereo_enabled(void) { return g_on; }

void stereo_eye_size(int w, int h)
{
    g_ew = w; g_eh = h;
    g_sx = (float)w / GAME_W; g_sy = (float)h / GAME_H;
}

void stereo_set_eye_target(int eye, unsigned fbo)
{
    g_external[eye] = 1;
    g_eye_fbo[eye] = fbo;
}

void stereo_set_eye(int eye, const float view[16], float tl, float tr, float td, float tu)
{
    memcpy(g_view[eye], view, 64);
    g_tan[eye][0] = tl; g_tan[eye][1] = tr; g_tan[eye][2] = td; g_tan[eye][3] = tu;
}

void stereo_game_camera(float *tan_x, float *tan_y)
{
    const float *p = g_last_persp;
    *tan_x = p[0] ? 1 / p[0] : 0.577f;
    *tan_y = p[5] ? 1 / p[5] : 0.433f;
}

static int mat_invert(const float *m, float *out)
{
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-20f) return 0;
    for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
    return 1;
}

/* For a vertex program with the game's projection baked into its constants:
 * clip space as the game made it -> clip space for this eye. The game's
 * projection is taken to be the one it has loaded. */
void stereo_reproject_view(int v, float out[16])
{
    int e = g_eye;
    g_eye = v;
    stereo_reproject(out);
    g_eye = e;
}

int stereo_multiview(void) { return g_mv; }

void stereo_reproject(float out[16])
{
    mat_identity(out);
    if (!g_on) return;
    const float *p = g_proj[g_proj_top];
    if (!is_perspective(p)) p = g_last_persp;
    float eff[16], inv[16];
    effective_projection(p, g_loaded_hud, eff);
    if (mat_invert(p, inv)) mat_mul(out, eff, inv);
}

void stereo_set_screen_distance(float d) { if (d > 0) g_screen_dist = d; }

/* Where a ray from the game camera's space crosses the HUD's screen, in the
 * game's 640x480 pixels. 0 when it points away from the screen. */
int stereo_ray_to_screen(const float o[3], const float d[3], float *x, float *y)
{
    if (d[2] > -1e-4f) return 0;
    float t = (-g_screen_dist - o[2]) / d[2];
    if (t <= 0) return 0;
    float hx = o[0] + t * d[0], hy = o[1] + t * d[1];
    float u = hx * g_last_persp[0] / g_screen_dist, v = hy * g_last_persp[5] / g_screen_dist;
    *x = GAME_W * 0.5f * (1 + u);
    *y = GAME_H * 0.5f * (1 - v);
    return 1;
}

void stereo_rect_scale(float *x, float *y)
{
    *x = g_on ? g_sx : 1; *y = g_on ? g_sy : 1;
}

unsigned stereo_eye_texture(int eye) { return g_eye_tex[eye]; }
#ifdef LINDBERGH_GL4ES
unsigned stereo_eye_array(void) { return g_mv ? gl4es_multiview_array(g_eye_tex[0]) : 0; }
#endif
unsigned stereo_eye_framebuffer(int eye) { return g_eye_fbo[eye]; }

/* Desktop check: both eyes side by side in the window. */
void stereo_present_desktop(int win_w, int win_h)
{
    if (!g_on) return;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0, 0, 0, 1);
    glDisable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    for (int i = 0; i < 2; i++) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_eye_fbo[i]);
        int half = win_w / 2, h = half * g_eh / g_ew;
        if (h > win_h) h = win_h;
        int y = (win_h - h) / 2;
        glBlitFramebuffer(0, 0, g_ew, g_eh, i * half, y, i * half + half, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_eye_fbo[0]);
}

/* Called once the GL handlers are bound. */
void stereo_init(void)
{
    const char *v = getenv("LINDBERGH_FRAMEMAP");
    if (v && *v) g_map_frame = atol(v);
    for (unsigned i = 0; i < HLE_COUNT; i++) {
        const char *n = hle_name((HleId)i);
        g_gl_ids[i] = n[0] == 'g' && n[1] == 'l' && strncmp(n, "glut", 4) != 0 && strncmp(n, "glX", 3) != 0;
    }
    g_gl_hook = stereo_gl_call;
    for (int i = 0; i < 32; i++) mat_identity(g_proj[i]);
    v = getenv("LINDBERGH_MENU_DRAWS");
    if (v && *v) g_menu_draws = (unsigned)atoi(v);
    v = getenv("LINDBERGH_ZOOM_LENS");
    if (v && *v) g_zoom_lens = (float)atof(v);
    v = getenv("LINDBERGH_STEREO");
    if (!v || !*v || *v == '0') { stereo_eye_size(GAME_W, GAME_H); return; }
    stereo_start();
}

/* Desktop defaults: a 90-degree square eye, 6.4 cm apart. A VR host
 * replaces all of it every frame. */
void stereo_start(void)
{
    if (g_on) return;
    void *a = guest_alloc(ARENA_SIZE, 64);
    if (!a) { fprintf(stderr, "[stereo] no guest memory for the recording - stereo off\n"); return; }
    g_arena = H2G(a);
    if (!g_sx) stereo_eye_size(1024, 1024);
    float ipd = 0.064f;
    const char *s = getenv("LINDBERGH_IPD");
    if (s && *s) ipd = (float)atof(s);
    for (int i = 0; i < 2; i++) {
        float m[16]; mat_identity(m);
        m[12] = i ? -ipd / 2 : ipd / 2;      /* view = inverse of the eye's offset */
        stereo_set_eye(i, m, 1, 1, 1, 1);
    }
    g_on = 1;
    fprintf(stderr, "[stereo] on: %dx%d per eye (x%.2f, x%.2f of the game's 640x480)\n", g_ew, g_eh, g_sx, g_sy);
}
