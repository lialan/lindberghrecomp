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
 * LINDBERGH_MENU_DRAWS=<n> (0, off): a frame with fewer world draws looks like a
 * menu; LINDBERGH_SCENE_STATS=1 reports the count every 50 frames.
 * LINDBERGH_ZOOM_LENS=<x> (8): 3D drawn with a longer lens, for 6 frames running, (projection x
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
int gl4es_multiview_copy(unsigned src, int layer, int sx0, int sy0, int sx1, int sy1,
                         unsigned dst, int level, int dx0, int dy0, int dx1, int dy1);
extern int gl4es_mv_trace;
void gl4es_multiview_read_pixel(int x, int y, unsigned char out[4]);
void gl4es_multiview_read_pixel_layer(int x, int y, int layer, unsigned char out[4]);
#endif

extern unsigned g_frame;

/* The game's picture: 640x480 unless its mode says otherwise (Let's Go
 * Jungle draws 1360x768), taken at stereo_init */
static int g_game_w = 640, g_game_h = 480;
#define GAME_W g_game_w
#define GAME_H g_game_h
/* A game drawing its world into a target of another size (Let's Go Jungle:
 * 800x600, tone mapped onto its 1360x768 screen), and whether its projection
 * lives in its vertex programs' constants (arb_env_projection) */
static int g_pic_w, g_pic_h, g_env_proj;
static unsigned g_swing_at;       /* the frame the game last swung its camera hard (track_camera_swing) */
void stereo_picture_size(int w, int h) { g_pic_w = w; g_pic_h = h; }
void stereo_env_projection(int on) { g_env_proj = on; }
static int picture_size(int w, int h) { return (w == GAME_W && h == GAME_H) || (w && w == g_pic_w && h == g_pic_h); }

/* ---- eyes ---- */
static int      g_on;
static int      g_mv;                   /* both eyes in one pass (GL_OVR_multiview2): no second eye to replay */
/* Widescreen (LINDBERGH_WIDE): one "eye", the game camera itself, its image
 * wider than the game's 4:3. The 3D keeps its vertical view and sees further
 * to the sides (Hor+); the 2D, laid out for 4:3, keeps its shape in the
 * middle, where it lines up with the 3D as it did on the arcade. Both are the
 * game's projection with its x row scaled by g_wide_k (4:3 over the image's
 * aspect). Nothing is recorded or replayed. */
static int      g_wide;
static float    g_wide_s;               /* this 2D draw's shift to its edge (game NDC of the image), 0 centred */
static int      g_wide_span;            /* this 2D draw spans the arcade screen: it spans the image */
static int g_flat;                      /* this frame is drawn flat: the arcade's picture, on the screen */
static float    g_wide_k = 1;
static float    g_mvR[3][16];           /* the views' matrices last given to GL, and back to the game's clip space */
static int      g_eye;                  /* the eye being drawn */
static int      g_ew = 1024, g_eh = 1024;
static float    g_sx, g_sy;             /* eye pixels per game pixel */
static GLuint   g_eye_fbo[2], g_eye_tex[2], g_eye_depth[2];
static int      g_external[2];          /* the host supplies the eye images */
static float    g_view[2][16];          /* game camera -> eye */
static float    g_hud_view[2][16];      /* head -> eye: the HUD kept in front of the face */
static int      g_hud_lock = -1;        /* the HUD follows the head (LINDBERGH_XR_HUD_LOCK, vr.hud_head_locked) */
static float    g_tan[2][4];            /* left, right, down, up */

/* ---- what the game believes ---- */
static GLuint   g_game_fb;              /* framebuffer the game has bound */
static GLint    g_vp[4] = { 0, 0, 640, 480 };       /* set to the game's size at stereo_init */
static GLenum   g_mode = GL_MODELVIEW;
static float    g_proj[32][16];         /* the game's projection stack */
static int      g_proj_top;
static float    g_last_persp[16] = { 1.732f, 0, 0, 0, 0, 2.309f, 0, 0, 0, 0, -1, -1, 0, 0, -0.02f, 0 };
                                        /* the game's last perspective (its usual one to start) */
static float    g_screen_lens[2] = { 1.732f, 2.309f };   /* the screen's lens: the game camera's, as last seen */
                                        /* the HUD's screen is sized by the game's usual lens, not
                                         * its current one: menus zoom the camera to 3.7 and back,
                                         * and the 2D shrank and grew with it */
static int      g_loaded_hud;           /* the projection GL has is the HUD's screen */
static int      g_hud_nowrite;          /* this 2D draw's depth writes are off, to be put back */
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
static int      g_side;                 /* inside a side pass */
static GLint    g_game_scissor[4];      /* the game's scissor box, in its pixels */
static unsigned g_scene_draws;          /* 3D draws in the world this frame */
static float    g_lens_seen;            /* the longest lens (projection x scale) drawn with this frame */
static float    g_zoom_lens = 8.0f;     /* a longer one is a zoomed view: the sniper scope is 11.4, the
                                         * game's own camera 1.7-2.4 and its zooms on a target up to ~6 */
static unsigned g_zoom_frames;          /* frames in a row drawn with such a lens */
/* The sniping sections (the game's qte_ctrl_snipe, reported by hle_sega.c)
 * zoom in steps, some well below 8: while one runs, any lens past this is
 * its scope. */
static float    g_snipe_lens = 3.2f;
static unsigned g_snipe_frame = ~0u;    /* the last frame a sniping event ran */
void stereo_snipe_running(void) { g_snipe_frame = g_frame; }
/* CQC (qte_ctrl_grapple): its markers are 2D on the screen, so it is aimed
 * on the screen's plane, not through the enemy a metre away */
static unsigned g_cqc_frame = ~0u;
void stereo_cqc_running(void) { g_cqc_frame = g_frame; }
int stereo_cqc_active(void) { return g_cqc_frame != ~0u && g_frame - g_cqc_frame <= 2; }
static float zoom_limit(void)
{
    return g_snipe_frame != ~0u && g_frame - g_snipe_frame <= 2 ? g_snipe_lens : g_zoom_lens;
}
static int      g_menu;                 /* a menu: its 3D belongs on the screen too */
static int      g_tex2d_on;             /* GL_TEXTURE_2D enabled on unit 0 (frame map) */
static unsigned g_menu_frames;          /* frames in a row that looked like a menu */
static unsigned g_menu_draws = 0;       /* fewer world draws than this looks like a menu; 0: off -
                                         * sparse gameplay scenes tripped it, flattening the world */
static unsigned g_zoom_draws, g_hudfb_draws;   /* this frame's 3D sent to the screen, for LINDBERGH_MODE_LOG */
static float    g_near_seen;            /* the nearest near plane of this frame's world draws */
static GLuint   g_world_fb = ~0u, g_world_fb_next = ~0u;   /* the target the world was drawn into last frame */
static unsigned g_hud2d, g_pass2d;      /* this frame's 2D draws put on the screen / left as passes (LINDBERGH_HUD_LOG) */
static float    g_hud_lens;
/* The game's own sight: the small 2D sprite drawn where the gun points.
 * Learnt as the texture such draws use, frame after frame, while the gun
 * moves; hidden on request (stereo_show_game_sight) by skipping draws of
 * that texture at the gun's place. */
static GLuint   g_tex2d_bound;          /* GL_TEXTURE_2D bound on unit 0 */
static float    g_gun_ndc[2] = { -9, -9 };   /* where the gun points, in the game's NDC */
static int      g_show_sight = 1;
static GLuint   g_sight_tex, g_sight_cand;
static unsigned g_sight_hits, g_sight_frame;
static float    g_sight_from[2];
static GLint    g_menu_vp[4];           /* this frame's first sub-viewport a screen 3D draw used (LINDBERGH_HUD_LOG) */             /* the lens the screen was sized by at this frame's first 2D draw on it */

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
/* The view the HUD's screen is seen through: the game camera's, so it stays
 * put in the world, or the head's, so it stays in front of the face */
static const float *hud_view(int eye)
{
    if (g_hud_lock < 0) { const char *v = getenv("LINDBERGH_XR_HUD_LOCK"); g_hud_lock = v && *v == '1'; }
    return g_hud_lock ? g_hud_view[eye] : g_view[eye];
}
void stereo_hud_head_locked(int on) { if (!getenv("LINDBERGH_XR_HUD_LOCK")) g_hud_lock = on; }

static void effective_projection(const float *p, int hud, float *out)
{
    if (!g_on || hud == 2) { memcpy(out, p, 64); return; }
    if (hud == 3) {     /* a 2D layer over the whole screen - a fade, a wipe: over the whole eye, in front */
        memcpy(out, p, 64);
        for (int col = 0; col < 4; col++) out[col * 4 + 2] = -0.999f * out[col * 4 + 3];
        return;
    }
    if (g_wide) {
        memcpy(out, p, 64);
        if (g_flat) return;                     /* the arcade's picture, shown 4:3 */
        int persp = is_perspective(p);
        if (persp && !hud) memcpy(g_last_persp, p, 64);
        if (persp || (hud && !g_wide_span))     /* the world wider; the 2D in the middle */
            for (int col = 0; col < 4; col++) out[col * 4] *= g_wide_k;
        if (hud && !persp && g_wide_s != 0)     /* or out at its edge */
            for (int col = 0; col < 4; col++) out[col * 4] += g_wide_s * out[col * 4 + 3];
        return;
    }
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
    sc[0] = d / g_screen_lens[0]; sc[5] = d / g_screen_lens[1];
    sc[10] = -0.02f * d;                /* a sliver of depth keeps the game's depth order */
    sc[14] = -d;
    /* near and far close around the screen: everything on it lies within
     * that sliver, and a 1 cm near plane left the depth buffer only ~1300
     * steps there - a scoped view's whole scene fought over them */
    float zn = 0.25f * d, zf = 4.0f * d;
    mat_frustum(e, -t[0] * zn, t[1] * zn, -t[2] * zn, t[3] * zn, zn, zf);
    mat_mul(ev, e, hud_view(g_eye));
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
    if ((long)g_frame == g_map_frame) {
        static int shown;
        if (shown++ < 3)
            fprintf(stderr, "[map] views: eye0 %.3f %.3f %.3f %.3f  mvR1 %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | tan1 %.2f %.2f %.2f %.2f\n",
                    eye0[0], eye0[5], eye0[10], eye0[14], g_mvR[1][0], g_mvR[1][5], g_mvR[1][10], g_mvR[1][15],
                    g_mvR[1][12], g_mvR[1][13], g_mvR[1][14], g_mvR[1][3], g_tan[1][0], g_tan[1][1], g_tan[1][2], g_tan[1][3]);
    }
    gl4es_multiview_set((const float (*)[16])g_mvR, g_clipped);
#endif
}

/* The projection GL has is loaded lazily, at the next draw: the game builds
 * one from several calls (identity, ortho, a multiply...), and each load is a
 * matrix for both eyes written to the GPU - pushes and pops alone cost
 * ~2 ms a frame when every call loaded it. */
static int g_proj_dirty;

static void load_projection(void)
{
    g_proj_dirty = 0;
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
static short  g_scaled_w[MAX_SCALED], g_scaled_h[MAX_SCALED];   /* their size as the game made them */
static float  g_scaled_k[MAX_SCALED][2];                        /* and how much bigger they are now */

/* The floating-point formats the game gave its textures, which gl4es reports
 * as RGBA (Let's Go Jungle draws its scene in half floats and tone maps it):
 * a render target made again at the eyes' size keeps it. Depth goes by where
 * the texture is attached (scale_target). */
#define MAX_TEXFMT 256
static struct { GLuint tex; GLenum ifmt, fmt, type; } g_texfmt[MAX_TEXFMT];
static unsigned g_ntexfmt;

static int texfmt_find(GLuint tex)
{
    for (unsigned i = 0; i < g_ntexfmt; i++) if (g_texfmt[i].tex == tex) return (int)i;
    return -1;
}

/* The framebuffer the game draws into, and its pixels per game pixel: the
 * eyes' scale for the window and the game's picture, a target's own for the
 * rest. The viewport and scissor box follow it. */
#define MAX_FB_K 64
static struct { GLuint fb; float k[2]; } g_fb_k[MAX_FB_K];
static unsigned g_nfb_k;
static float g_cur_k[2];
static struct { GLuint fb, tex; } g_fb_color[MAX_FB_K];  /* each framebuffer's colour texture */
static unsigned g_nfb_color;

/* Framebuffers drawn into a texture that is not the game's 640x480 picture -
 * the menu's soldier and rifle, drawn with their own camera into 256x256
 * textures shown as sprites: what is drawn there is no view from the eyes,
 * and keeps the game's projection as it is. */
#define MAX_FBS 64
static struct { GLuint fb; int offscreen; } g_fbs[MAX_FBS];
static unsigned g_nfbs;

static int offscreen_fb(GLuint fb)
{
    for (unsigned i = 0; i < g_nfbs; i++) if (g_fbs[i].fb == fb) return g_fbs[i].offscreen;
    return 0;
}

static struct { GLuint fb, depth; } g_fb_depth[MAX_FBS];   /* each framebuffer's depth texture */
static unsigned g_nfb_depth;

static void note_fb_depth(GLuint fb, GLuint tex)
{
    for (unsigned i = 0; i < g_nfb_depth; i++) if (g_fb_depth[i].fb == fb) { g_fb_depth[i].depth = tex; return; }
    if (g_nfb_depth < MAX_FBS) { g_fb_depth[g_nfb_depth].fb = fb; g_fb_depth[g_nfb_depth++].depth = tex; }
}

static void note_fb_target(GLuint fb, GLuint tex)
{
    int w = 0, h = 0;
    for (unsigned i = 0; i < g_nscaled; i++) if (g_scaled[i] == tex) { w = g_scaled_w[i]; h = g_scaled_h[i]; }
    if (!w || !fb) return;
    int off = !picture_size(w, h);
    unsigned i = 0;
    while (i < g_nfbs && g_fbs[i].fb != fb) i++;
    int added = i == g_nfbs;
    if (added) { if (g_nfbs >= MAX_FBS) return; g_nfbs++; g_fbs[i].fb = fb; }
    if (g_fbs[i].offscreen != off || added)
        fprintf(stderr, "[stereo] framebuffer %u draws into texture %u, %dx%d%s\n", fb, tex, w, h, off ? ": offscreen, the game's own projection" : "");
    g_fbs[i].offscreen = off;
}

static int scaled(GLuint t)
{
    for (unsigned i = 0; i < g_nscaled; i++) if (g_scaled[i] == t) return 1;
    return 0;
}

/* a 2D texture the game draws its world into (stereo_picture_size) */
static int picture_2d(GLuint tex)
{
    for (unsigned i = 0; tex && g_pic_w && i < g_nscaled; i++)
        if (g_scaled[i] == tex) return g_scaled_w[i] == g_pic_w && g_scaled_h[i] == g_pic_h;
    return 0;
}

int stereo_is_picture_texture(unsigned tex) { return g_on && picture_2d(tex); }

/* a framebuffer drawing into such a texture: a full-screen pass there
 * (a background, a haze) is part of the world's picture, not the HUD */
static int picture_fb(GLuint fb)
{
    for (unsigned i = 0; fb && i < g_nfb_color; i++)
        if (g_fb_color[i].fb == fb) return picture_2d(g_fb_color[i].tex);
    return 0;
}

/* framebuffer fb draws into tex: it takes tex's scale */
static void note_fb_scale(GLuint fb, GLuint tex)
{
    unsigned i = 0;
    while (i < g_nscaled && g_scaled[i] != tex) i++;
    if (!fb || i == g_nscaled) return;
    unsigned j = 0;
    while (j < g_nfb_k && g_fb_k[j].fb != fb) j++;
    if (j == g_nfb_k) { if (g_nfb_k >= MAX_FB_K) return; g_nfb_k++; g_fb_k[j].fb = fb; }
    g_fb_k[j].k[0] = g_scaled_k[i][0]; g_fb_k[j].k[1] = g_scaled_k[i][1];
}

static void fb_scale(GLuint fb, float k[2])
{
    k[0] = g_sx; k[1] = g_sy;
    for (unsigned j = 0; fb && j < g_nfb_k; j++)
        if (g_fb_k[j].fb == fb) { k[0] = g_fb_k[j].k[0]; k[1] = g_fb_k[j].k[1]; }
}

/* A texture the game renders into is reallocated at the eye's resolution.
 * Sampling uses normalised coordinates, so nothing that reads it changes. */
static void scale_target(GLenum textarget, GLuint tex, GLenum attachment)
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
    int known = texfmt_find(tex);
    if (known >= 0) { ifmt = (GLint)g_texfmt[known].ifmt; fmt = g_texfmt[known].fmt; type = g_texfmt[known].type; }
    else if (ifmt == GL_DEPTH_COMPONENT || ifmt == GL_DEPTH_COMPONENT16 || ifmt == GL_DEPTH_COMPONENT24 ||
        ifmt == GL_DEPTH_COMPONENT32) { fmt = GL_DEPTH_COMPONENT; type = GL_UNSIGNED_INT; }
    else if (ifmt == GL_DEPTH24_STENCIL8 || ifmt == GL_DEPTH_STENCIL) { fmt = GL_DEPTH_STENCIL; type = GL_UNSIGNED_INT_24_8; }
    /* gl4es reports a depth texture's format as RGBA; where it is attached
     * says what it is (Let's Go Jungle's depth textures) */
    else if (attachment == GL_DEPTH_ATTACHMENT) { ifmt = GL_DEPTH_COMPONENT; fmt = GL_DEPTH_COMPONENT; type = GL_UNSIGNED_INT; }
    else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT || attachment == GL_STENCIL_ATTACHMENT) {
        ifmt = GL_DEPTH_STENCIL; fmt = GL_DEPTH_STENCIL; type = GL_UNSIGNED_INT_24_8;
    }
    else if (ifmt == GL_RGB || ifmt == GL_RGB8) fmt = GL_RGB;
    /* The game's picture is drawn at the eyes' resolution. Its other
     * targets - shadow maps, glow, the menu's sprites - at most twice their
     * size and 1024 pixels across: at the eyes' scale Let's Go Jungle's
     * 1024x1024 shadow map was 1550x3040, and the frame took 200 ms. A
     * rectangle texture is sampled in pixels, so it keeps the eyes' scale. */
    float kx = g_sx, ky = g_sy;
    if (w > 0 && h > 0 && w == g_pic_w && h == g_pic_h) { kx = (float)g_ew / w; ky = (float)g_eh / h; }   /* the eyes' size */
    else if (tg == GL_TEXTURE_2D && !(w == GAME_W && h == GAME_H) && w > 0 && h > 0) {
        float k = fminf(fminf(g_sx, g_sy), 2.0f);
        k = fminf(k, 1024.0f / (float)(w > h ? w : h));
        kx = ky = k < 1.0f ? 1.0f : k;
    }
    int nw = (int)lroundf(w * kx), nh = (int)lroundf(h * ky);
    g_scaled_w[g_nscaled - 1] = (short)w; g_scaled_h[g_nscaled - 1] = (short)h;
    g_scaled_k[g_nscaled - 1][0] = kx; g_scaled_k[g_nscaled - 1][1] = ky;
    glTexImage2D(tg, 0, ifmt, nw, nh, 0, fmt, type, NULL);
    fprintf(stderr, "[stereo] render target %u%s: %dx%d -> %dx%d (format %#x)\n", tex,
            tg == GL_TEXTURE_2D ? "" : " (rect)", w, h, nw, nh, ifmt);
    glBindTexture(tg, (GLuint)prev);
}

static void (*g_aim_hook)(void);
void stereo_set_aim_hook(void (*fn)(void)) { g_aim_hook = fn; }

/* the game's viewport (vp) or scissor box, in the bound target's pixels */
static void apply_rect(int vp)
{
    const GLint *r = vp ? g_vp : g_game_scissor;
    float sx = g_side ? 1 : g_cur_k[0], sy = g_side ? 1 : g_cur_k[1];
    GLint X = (GLint)lroundf(r[0] * sx), Y = (GLint)lroundf(r[1] * sy);
    GLint W = (GLint)lroundf((r[0] + r[2]) * sx) - X, H = (GLint)lroundf((r[1] + r[3]) * sy) - Y;
    if (vp) { glViewport(X, Y, W, H); g_eye_vp[0] = X; g_eye_vp[1] = Y; g_eye_vp[2] = W; g_eye_vp[3] = H; }
    else { glScissor(X, Y, W, H); g_scissor[0] = X; g_scissor[1] = Y; g_scissor[2] = W; g_scissor[3] = H; }
}

/* The times the world's target is left in a frame, and how many world draws
 * were done by each; the one to read its depth at (last frame's first after
 * which no more world was drawn), -1 none: the frame's end. */
static unsigned g_leaves, g_leave_draws[16];
static int      g_read_at_leave;
static int      g_depth_read;           /* LG_DEPTH_* (game_profile.h): which leaving that is */
void stereo_depth_read(int mode) { g_depth_read = mode; }

static void bind_framebuffer(GLenum target, GLuint fb)
{
    /* Leaving the world's target with the whole scene drawn: its depth is
     * the world's now - later full-screen passes into it write a flat 0.5
     * over it (Ghost Squad) - so the aim reads it here. Some games leave it
     * before they are done (Let's Go Jungle, for its shadow and reflection
     * targets): the leaving that counts is the one after the last world
     * draw, as the previous frame had it. */
    if (g_aim_hook && g_mv && !g_flat && g_scene_draws > 0 && g_game_fb == g_world_fb_next && fb != g_game_fb) {
        if (g_leaves < 16) g_leave_draws[g_leaves] = g_scene_draws;
        if ((int)g_leaves == g_read_at_leave) g_aim_hook();
        g_leaves++;
    }
    g_game_fb = fb;
    glBindFramebuffer(target, fb ? fb : g_eye_fbo[g_eye]);
    float k[2];
    fb_scale(fb, k);
    if (!g_side && (k[0] != g_cur_k[0] || k[1] != g_cur_k[1])) {
        g_cur_k[0] = k[0]; g_cur_k[1] = k[1];
        if (g_vp[2] > 0) apply_rect(1);
        if (g_game_scissor[2] > 0) apply_rect(0);
    }
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
    if (g_wide && (g_flat || g_wide_span)) return;   /* the whole image is the arcade's / the layer spans it */
    if (g_wide) {   /* the arcade screen is the middle of the image (moved with a piece at an edge) */
        float sh = g_wide_s * g_eye_vp[2] / 2;
        GLint X0 = g_eye_vp[0] + (GLint)floorf(g_eye_vp[2] * (1 - g_wide_k) / 2 + sh);
        GLint X1 = g_eye_vp[0] + (GLint)ceilf(g_eye_vp[2] * (1 + g_wide_k) / 2 + sh);
        GLint Y0 = g_eye_vp[1], Y1 = g_eye_vp[1] + g_eye_vp[3];
        if (g_scissor_on) {
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
        return;
    }
    float m[16], e[16], d = g_screen_dist;
    const float *t = g_tan[g_eye];
    mat_frustum(e, -t[0] * 0.01f, t[1] * 0.01f, -t[2] * 0.01f, t[3] * 0.01f, 0.01f, 1000.0f);
    mat_mul(m, e, hud_view(g_eye));
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int k = 0; k < 4; k++) {
        float p[3] = { (k & 1 ? 1 : -1) * d / g_screen_lens[0], (k & 2 ? 1 : -1) * d / g_screen_lens[1], -d };
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
    K_DRAWARRAYS, K_DRAWELEMENTS, K_DRAWOTHER, K_BEGIN, K_END, K_MATRIXMODE, K_VERTEX,
    K_LOADID, K_LOADMATRIX, K_MULTMATRIX, K_ORTHO, K_ORTHO2D, K_FRUSTUM, K_PERSPECTIVE,
    K_PUSHMATRIX, K_POPMATRIX, K_TRANSLATE, K_SCALE, K_GETINTEGERV, K_GETFLOATV, K_GETOTHER, K_UNMAP, K_CLEAR, K_COPYTEXSUB, K_COPYTEX,
    K_TEXIMAGE, K_FBSTATUS,
};
typedef struct {
    uint8_t ok, kind, live, track;          /* live: run, never recorded; track: client array state */
    int8_t pidx;                            /* an argument passed by pointer that is copied */
    uint8_t pkind;                          /* 0: pbytes; 1: glBufferData's size; 2: glBufferSubData's */
    uint32_t pbytes;
} Cls;
/* Cls.track: the client-array calls, in cls()'s order */
enum { T_ENABLECS = 1, T_DISABLECS, T_CLIENTTEX, T_CLIENTTEXARB, T_ENABLEVA, T_DISABLEVA,
       T_VERTEXPTR, T_NORMALPTR, T_COLORPTR, T_TEXCOORDPTR, T_ATTRIBPTR };
static Cls g_cls[HLE_ID_MAX];

/* ---- the game's menu screens ----
 * The menus are 3D: panels, the soldier and his rifle, all drawn with the
 * game camera, which swings and zooms between screens. In the room they
 * tilted and slid past the edge of view; on the screen, where the arcade
 * monitor showed them, they hold still. Neither draw counts nor render
 * targets tell a menu from a quiet moment of play, but the game itself does:
 * in game mode it runs one state function a frame, GameLoop while playing,
 * GameInstruction, GameWeaponSelect and so on while a menu is up. */
uint32_t stereo_state_lo = ~0u, stereo_state_hi;
static uint32_t g_state_va[48];
static unsigned g_nstate, g_nmenu;      /* the first g_nmenu are menus */
static unsigned g_state_frame = ~0u;    /* the frame a menu state last ran */
static uint32_t g_state_last;
static long g_rt_dump_at = -2;          /* LINDBERGH_RT_DUMP_MENU=N: dump N frames after the first menu state */

static void watch_states(void)
{
    static const char *menus[] = {
        "Init", "Calibration", "Instruction", "Card", "Custom", "MatchUpSelect", "CharacterSelect", "WeaponSelect",
        "Information", "Entry", "StartWait", "StageSelect", "WaitCard", "NameEntry", "Ranking", "Record",
        "Password", "Result", "Start",
    };
    static const char *play[] = {   /* reported in LINDBERGH_HUD_LOG only */
        "Opening", "RoundStart", "Loop", "RoundResult", "Replay", "SceneClear", "ChallengerComes",
        "TimeUp", "Continue", "Close", "Ending", "CleanUp", "Pause", "Exit", "Over", "StageClear",
    };
    if (g_nstate) return;
    for (int pass = 0; pass < 2; pass++) {
        const char **l = pass ? play : menus;
        int n = pass ? (int)(sizeof play / sizeof *play) : (int)(sizeof menus / sizeof *menus);
        for (int i = 0; i < n && g_nstate < 48; i++) {
            char name[64];
            snprintf(name, sizeof name, "_Z%zuGame%sP8GAMEWORK", strlen(l[i]) + 4, l[i]);
            uint32_t va = guest_symbol(name);
            if (!va) continue;
            g_state_va[g_nstate++] = va;
            if (va < stereo_state_lo) stereo_state_lo = va;
            if (va > stereo_state_hi) stereo_state_hi = va;
        }
        if (!pass) g_nmenu = g_nstate;
    }
    fprintf(stderr, "[stereo] %u menu states of %u found\n", g_nmenu, g_nstate);
}

void stereo_guest_state(uint32_t va)
{
    for (unsigned i = 0; i < g_nstate; i++) {
        if (g_state_va[i] != va) continue;
        if (i < g_nmenu) {
            if (g_state_frame == ~0u && g_rt_dump_at == -2) {
                const char *v = getenv("LINDBERGH_RT_DUMP_MENU");
                g_rt_dump_at = v && *v ? (long)g_frame + atol(v) : -1;
            }
            g_state_frame = g_frame;
        }
        else g_state_frame = ~0u;
        if (va != g_state_last && getenv("LINDBERGH_HUD_LOG"))
            fprintf(stderr, "[state] frame %u: %#x (%s)\n", g_frame, va, i < g_nmenu ? "menu" : "play");
        g_state_last = va;
        return;
    }
}

/* a menu state ran this frame or the last (outside game mode, none runs) */
static int menu_state(void)
{
    return g_state_frame != ~0u && g_frame - g_state_frame <= 1;
}

/* A menu frame is drawn flat - the arcade's own picture, every pass and
 * sprite as the game made it - and the host puts that picture on the screen.
 * Its sprites sample their render targets where the game drew into them,
 * which drawing the menu's 3D from the eyes broke (a black window where the
 * soldier stands, buttons that changed every frame). Decided per frame:
 * the state the game ran last frame. */
static int g_flat_shown;
int stereo_flat_frame(void) { return g_flat_shown; }
void stereo_screen(float *half_w, float *half_h, float *dist)
{
    *half_w = g_screen_dist / g_screen_lens[0];
    *half_h = g_screen_dist / g_screen_lens[1];
    *dist = g_screen_dist;
}

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
        { "glVertex2f", K_VERTEX }, { "glVertex3f", K_VERTEX }, { "glVertex2i", K_VERTEX },
        { "glLoadIdentity", K_LOADID }, { "glLoadMatrixf", K_LOADMATRIX }, { "glMultMatrixf", K_MULTMATRIX },
        { "glOrtho", K_ORTHO }, { "gluOrtho2D", K_ORTHO2D }, { "glFrustum", K_FRUSTUM }, { "gluPerspective", K_PERSPECTIVE },
        { "glPushMatrix", K_PUSHMATRIX }, { "glPopMatrix", K_POPMATRIX },
        { "glTranslatef", K_TRANSLATE }, { "glScalef", K_SCALE },
        { "glGetIntegerv", K_GETINTEGERV }, { "glGetFloatv", K_GETFLOATV },
        { "glUnmapBufferARB", K_UNMAP }, { "glUnmapBuffer", K_UNMAP }, { "glClear", K_CLEAR },
        { "glCopyTexSubImage2D", K_COPYTEXSUB }, { "glCopyTexImage2D", K_COPYTEX }, { "glTexImage2D", K_TEXIMAGE },
        { "glCheckFramebufferStatusEXT", K_FBSTATUS }, { "glCheckFramebufferStatus", K_FBSTATUS },
    };
    k->kind = K_OTHER;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(n, T[i].name)) { k->kind = T[i].kind; break; }
    if (k->kind == K_OTHER && (!strncmp(n, "glDraw", 6) || !strncmp(n, "glMultiDraw", 11))) k->kind = K_DRAWOTHER;
    if (k->kind == K_OTHER && !strncmp(n, "glGet", 5)) k->kind = K_GETOTHER;
    k->live = (uint8_t)live_only(n);
    {   /* which client-array call, for track_arrays: its 1-based place here */
        static const char *const TR[] = {
            "glEnableClientState", "glDisableClientState", "glClientActiveTexture", "glClientActiveTextureARB",
            "glEnableVertexAttribArrayARB", "glDisableVertexAttribArrayARB", "glVertexPointer", "glNormalPointer",
            "glColorPointer", "glTexCoordPointer", "glVertexAttribPointerARB",
        };
        k->track = 0;
        for (unsigned i = 0; i < sizeof TR / sizeof TR[0]; i++)
            if (!strcmp(n, TR[i])) { k->track = (uint8_t)(i + 1); break; }
    }
    int idx; uint32_t bytes; uint8_t pk;
    pointer_arg(n, &idx, &bytes, &pk);
    k->pidx = (int8_t)idx; k->pbytes = bytes; k->pkind = pk;
    k->ok = 1;
    return k;
}

static int is_draw(int kind) { return kind == K_DRAWARRAYS || kind == K_DRAWELEMENTS || kind == K_DRAWOTHER; }

/* Returns 1 when it did the call's work itself. */
/* Is this 2D draw the game's sight? Learns it, and says whether to skip it. */
/* Widescreen: where a HUD piece goes. The 4:3 layout sits in the middle of
 * the image; the life gauge, the weapon panel, Press Start - the pieces off
 * centre in the top and bottom bands - keep to the image's edge instead, as a HUD made for a
 * wide screen would. A piece continuing the one before it on the same row
 * (the next letter, the next segment of a gauge) goes where that one went.
 * Anything near the gun stays: the sight and the markers on soldiers lie on
 * the world, which the game aims through. */
static float wide_place(CPU *c, const char *n)
{
    static float last[4]; static float last_s; static unsigned last_frame = ~0u;
    float b[4];
    if (!ndc_box(c, n, b)) {
        if (getenv("LINDBERGH_NOBOX")) fprintf(stderr, "[nobox] frame %u %s tex %u\n", g_frame, n, g_tex2d_bound);
        return 0;
    }
    float edge = 1 - g_wide_k, w = b[2] - b[0], h = b[3] - b[1], s = 0;
    if (getenv("LINDBERGH_NOBOX") && (w > 0.9f || h > 0.9f))
        fprintf(stderr, "[big2d] frame %u box %.2f..%.2f x %.2f..%.2f tex %u\n", g_frame, b[0], b[2], b[1], b[3], g_tex2d_bound);
    /* spans the screen's width or height: a banner, a fade, the orange of
     * being hit (two full-height halves) */
    if (w >= 1.9f || h >= 1.9f) { g_wide_span = 1; return 0; }
    float gx = (b[0] + b[2]) * 0.5f - g_gun_ndc[0], gy = (b[1] + b[3]) * 0.5f - g_gun_ndc[1];
    int near_gun = g_gun_ndc[0] > -2 && gx * gx + gy * gy < 0.04f;
    int follows = 0;
    if (last_frame == g_frame) {
        float lh = last[3] - last[1];
        int same_row = fabsf(b[1] - last[1]) < 0.5f * (h > lh ? h : lh) + 0.01f;
        follows = same_row && b[0] > last[0] - 0.01f && b[0] < last[2] + 0.25f * (w + h) + 0.02f;
    }
    /* the gauges and panels are in the top and bottom bands: a band's pieces
     * either side of its centre go to that side. The rows between - the
     * reload warning, sliding in with big fading copies of itself - stay
     * centred, or the animation jumped between the middle and an edge. */
    float cx = (b[0] + b[2]) * 0.5f, cy = (b[1] + b[3]) * 0.5f;
    float cut = 0.25f;
    if (fabsf(cy) <= 0.6f) s = 0;
    else if (follows) s = last_s;
    else if (!near_gun && cx < -cut) s = -edge;
    else if (!near_gun && cx >  cut) s = edge;
    memcpy(last, b, sizeof b); last_s = s; last_frame = g_frame;
    return s;
}

static int sight_draw(CPU *c, const char *n)
{
    if (g_gun_ndc[0] < -2 || !g_tex2d_on || !g_tex2d_bound) return 0;
    float b[4];
    if (!ndc_box(c, n, b)) return 0;
    float w = b[2] - b[0], h = b[3] - b[1];
    if (w > 0.4f || h > 0.5f) return 0;
    float cx = (b[0] + b[2]) * 0.5f - g_gun_ndc[0], cy = (b[1] + b[3]) * 0.5f - g_gun_ndc[1];
    {   /* LINDBERGH_SIGHT_LOG=1: small sprites near the gun, and how far off it, in game pixels */
        static int on = -1;
        if (on < 0) { const char *v = getenv("LINDBERGH_SIGHT_LOG"); on = v && *v == '1'; }
        static unsigned last;
        if (on && g_frame - last >= 30 && fabsf(cx) < 0.3f && fabsf(cy) < 0.3f && w < 0.15f && h < 0.2f) {
            last = g_frame;
            fprintf(stderr, "[sight] frame %u tex %u size %.0fx%.0f off %+.1f %+.1f px (gun %.0f,%.0f)\n", g_frame, g_tex2d_bound,
                    w * 320, h * 240, cx * 320, -cy * 240, (g_gun_ndc[0] + 1) * 320, (1 - g_gun_ndc[1]) * 240);
        }
    }
    if (cx * cx * 100 + cy * cy * 56 > 0.02f) return 0;    /* centred on the gun, within a few pixels */
    if (!g_sight_tex) {
        if (g_tex2d_bound != g_sight_cand) {
            g_sight_cand = g_tex2d_bound; g_sight_hits = 0;
            g_sight_from[0] = g_gun_ndc[0]; g_sight_from[1] = g_gun_ndc[1];
        }
        if (g_frame != g_sight_frame) { g_sight_frame = g_frame; g_sight_hits++; }
        float mx = g_gun_ndc[0] - g_sight_from[0], my = g_gun_ndc[1] - g_sight_from[1];
        if (g_sight_hits >= 30 && mx * mx + my * my > 0.01f) {   /* followed a moving gun */
            g_sight_tex = g_sight_cand;
            fprintf(stderr, "[stereo] frame %u: the game's sight is texture %u\n", g_frame, g_sight_tex);
        }
        return 0;
    }
    return !g_show_sight && g_tex2d_bound == g_sight_tex;
}

/* The world's lens: the one most of a frame's 3D is drawn with. The game's
 * own gun - the reload animation, a hand bringing a magazine up - is drawn
 * into the same scene with a lens of its own, right in front of the camera,
 * and sometimes last. */
static struct { float p[16]; unsigned n; } g_lens_tally[4];
static float    g_world_persp[16] = { 1.732f, 0, 0, 0, 0, 2.309f, 0, 0, 0, 0, -1, -1, 0, 0, -0.02f, 0 };

static void tally_lens(const float *p)
{
    int k = -1, least = 0;
    for (int i = 0; i < 4; i++) {
        if (g_lens_tally[i].n && fabsf(g_lens_tally[i].p[0] - p[0]) < 1e-3f && fabsf(g_lens_tally[i].p[14] - p[14]) < 1e-6f) { k = i; break; }
        if (g_lens_tally[i].n < g_lens_tally[least].n) least = i;
    }
    if (k < 0) { k = least; memcpy(g_lens_tally[k].p, p, 64); g_lens_tally[k].n = 0; }
    g_lens_tally[k].n++;
}

/* The game's gun in VR is in the player's hand: its textures are learned
 * from draws with the gun's lens (1.73, unlike the world's) within a few
 * units of the camera while the player reloads (a cutscene's close-ups use
 * that lens too), and draws with them that close are the gun's: hidden. */
#define MAX_WEAPON_TEX 32
static GLuint   g_weapon_tex[MAX_WEAPON_TEX];
static unsigned g_nweapon_tex, g_weapon_draws, g_weapon_frame_draws;
static int      g_weapon_learn, g_weapon_loaded;
void stereo_game_gun_reloading(int on) { g_weapon_learn = on; }

/* The gun's textures are the same every session (loaded at start-up, in the
 * same order): remembered in game_gun2.txt, so the gun is hidden from the
 * first reload - also in a scene whose world shares the gun's lens. */
static void weapon_load(void)
{
    g_weapon_loaded = 1;
    FILE *f = fopen("game_gun2.txt", "r");
    unsigned t;
    while (f && g_nweapon_tex < MAX_WEAPON_TEX && fscanf(f, "%u", &t) == 1) g_weapon_tex[g_nweapon_tex++] = t;
    if (f) fclose(f);
}
static void weapon_save(void)
{
    FILE *f = fopen("game_gun2.txt", "w");
    for (unsigned i = 0; f && i < g_nweapon_tex; i++) fprintf(f, "%u\n", g_weapon_tex[i]);
    if (f) fclose(f);
}
unsigned stereo_game_gun_draws(void) { return g_weapon_frame_draws; }

static int weapon_draw(void)
{
    const float *P = g_proj[g_proj_top];
    if (!g_tex2d_bound) return 0;
    if (!g_weapon_loaded) weapon_load();
    float mv[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    if (mv[12] * mv[12] + mv[13] * mv[13] + mv[14] * mv[14] > 9.0f) return 0;
    if (getenv("LINDBERGH_GUN_LOG") && g_weapon_learn && !g_replaying)
        fprintf(stderr, "[gun] frame %u: lens %.3f (world %.3f), at %.2f %.2f %.2f, tex %u\n", g_frame, P[0], g_world_persp[0],
                mv[12], mv[13], mv[14], g_tex2d_bound);
    /* The gun is drawn with a lens of its own (1.732) while the world uses
     * another: only such a draw can be it. Texture names are reused once a
     * mission loads, so a name alone hid soldiers who came near. */
    int gun_lens = fabsf(P[0] - 1.732f) < 0.01f && fabsf(g_world_persp[0] - P[0]) > 0.05f * P[0];
    if (!gun_lens) return 0;
    int known = 0;
    for (unsigned i = 0; i < g_nweapon_tex; i++) if (g_weapon_tex[i] == g_tex2d_bound) known = 1;
    if (!known && !g_replaying && g_weapon_learn && g_nweapon_tex < MAX_WEAPON_TEX) {
        g_weapon_tex[g_nweapon_tex++] = g_tex2d_bound;
        fprintf(stderr, "[stereo] frame %u: texture %u is the game's own gun\n", g_frame, g_tex2d_bound);
        weapon_save();
        known = 1;
    }
    if (!known) return 0;
    if (!g_replaying) g_weapon_draws++;
    return 1;
}

/* ---- copies of the window ----
 * A game without framebuffer objects (The House of the Dead 4) draws into
 * the window and copies what it drew into textures, glCopyTexSubImage2D:
 * a small copy of the scene for its glare, the whole picture to draw back
 * over itself through its screen filters. The window is the eyes here, at
 * the eye's resolution: the copy takes the region the game means, scaled
 * back to the texture's size, from the first eye. A copy of the whole
 * screen drawn back over it would cover both eyes' scene with one eye's
 * flat picture; the scene is already there, so that draw is left out. */
#define MAX_SCREEN_COPIES 8
static GLuint   g_screen_copy[MAX_SCREEN_COPIES];
static unsigned g_nscreen_copy;
static int      g_skip_begin;           /* inside a glBegin/glEnd that is left out */
static HleId    g_cur_id;               /* the call transform_call is looking at */

static int screen_copy(GLuint tex)
{
    for (unsigned i = 0; i < g_nscreen_copy; i++) if (g_screen_copy[i] == tex) return 1;
    return 0;
}

/* ---- side passes into a pbuffer ----
 * The pbuffer is the eyes, but the game also draws other views into it to
 * copy into textures: its glare at 80x60, and in some rooms three 512x512
 * passes after the scene (reflections) - each cleared first, which wiped the
 * scene from the eyes and left the clear colour under the HUD. A pass whose
 * clear comes with less than the whole screen as its viewport goes into a
 * framebuffer of its own, at the game's resolution and with its projection,
 * until a clear of the whole screen or the next glXMakeCurrent. */
#define SIDE_W 1024
#define SIDE_H 1024
static int      g_pbuffers;             /* the game draws into pbuffers (the eyes) */
static GLuint   g_side_fbo, g_side_tex[2];

void stereo_pbuffer_made(void) { g_pbuffers = 1; }

static void side_pass(int on)
{
#ifdef LINDBERGH_GL4ES
    if (on == g_side) return;
    if (on && !g_side_fbo) {
        glGenTextures(2, g_side_tex);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, g_side_tex[0]);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA, SIDE_W, SIDE_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, g_side_tex[1]);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_DEPTH_COMPONENT, SIDE_W, SIDE_H, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, g_unit0_rect);
        glGenFramebuffers(1, &g_side_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_side_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE_ARB, g_side_tex[0], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_RECTANGLE_ARB, g_side_tex[1], 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (g_nfbs < MAX_FBS) { g_fbs[g_nfbs].fb = g_side_fbo; g_fbs[g_nfbs++].offscreen = 1; }
        fprintf(stderr, "[stereo] frame %u: side passes into the pbuffer drawn apart%s\n", g_frame,
                st == GL_FRAMEBUFFER_COMPLETE ? "" : " (incomplete framebuffer)");
    }
    g_side = on;
    bind_framebuffer(GL_FRAMEBUFFER, on ? g_side_fbo : 0);
    /* the viewport and scissor box in the new target's pixels */
    float sx = on ? 1 : g_sx, sy = on ? 1 : g_sy;
    GLint X = (GLint)lroundf(g_vp[0] * sx), Y = (GLint)lroundf(g_vp[1] * sy);
    g_eye_vp[0] = X; g_eye_vp[1] = Y;
    g_eye_vp[2] = (GLint)lroundf((g_vp[0] + g_vp[2]) * sx) - X; g_eye_vp[3] = (GLint)lroundf((g_vp[1] + g_vp[3]) * sy) - Y;
    glViewport(g_eye_vp[0], g_eye_vp[1], g_eye_vp[2], g_eye_vp[3]);
    const GLint *s = g_game_scissor;
    X = (GLint)lroundf(s[0] * sx); Y = (GLint)lroundf(s[1] * sy);
    g_scissor[0] = X; g_scissor[1] = Y;
    g_scissor[2] = (GLint)lroundf((s[0] + s[2]) * sx) - X; g_scissor[3] = (GLint)lroundf((s[1] + s[3]) * sy) - Y;
    glScissor(g_scissor[0], g_scissor[1], g_scissor[2], g_scissor[3]);
#else
    (void)on;
#endif
}

static int copy_from_eyes(CPU *c, int k)
{
#ifdef LINDBERGH_GL4ES
    if (!g_mv || (g_game_fb != 0 && !g_side) || A32(0) != GL_TEXTURE_2D) return 0;
    GLint tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    GLint level = AI32(1), xo = 0, yo = 0, x, y, w, h;
    if (k == K_COPYTEXSUB) { xo = AI32(2); yo = AI32(3); x = AI32(4); y = AI32(5); w = AI32(6); h = AI32(7); }
    else {
        GLenum ifmt = A32(2);
        if (ifmt == GL_DEPTH_COMPONENT || ifmt == GL_DEPTH_COMPONENT16 || ifmt == GL_DEPTH_COMPONENT24 ||
            ifmt == GL_DEPTH_COMPONENT32) return 0;
        x = AI32(3); y = AI32(4); w = AI32(5); h = AI32(6);
        glTexImage2D(GL_TEXTURE_2D, level, ifmt, w, h, 0,
                     ifmt == GL_RGB || ifmt == GL_RGB8 || ifmt == 3 ? GL_RGB : GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    if (!tex || w <= 0 || h <= 0) return 1;
    float sx = g_side ? 1 : g_sx, sy = g_side ? 1 : g_sy;
    int X0 = (int)lroundf(x * sx), Y0 = (int)lroundf(y * sy);
    int X1 = (int)lroundf((x + w) * sx), Y1 = (int)lroundf((y + h) * sy);
    static int off = -1;
    if (off < 0) { const char *v = getenv("LINDBERGH_EYE_COPY"); off = v && *v == '0'; }
    if (!off) gl4es_multiview_copy(g_side ? g_side_tex[0] : g_eye_tex[0], 0, X0, Y0, X1, Y1, (GLuint)tex, level, xo, yo, xo + w, yo + h);
    if (!g_side && w >= GAME_W * 9 / 10 && h >= GAME_H * 9 / 10 && !screen_copy((GLuint)tex) && g_nscreen_copy < MAX_SCREEN_COPIES) {
        g_screen_copy[g_nscreen_copy++] = (GLuint)tex;
        fprintf(stderr, "[stereo] frame %u: texture %d is a copy of the whole screen\n", g_frame, tex);
    }
    return 1;
#else
    (void)c; (void)k;
    return 0;
#endif
}

/* glXMakeCurrent: the window (or a pbuffer standing in for it) is the eyes */
void stereo_bind_window(void)
{
    if (g_side) side_pass(0);
    else bind_framebuffer(GL_FRAMEBUFFER, 0);
}

/* ---- what can be aimed at on the screen ----
 * Some of the game's 2D is a target: the path choice's two boxes, each a
 * view of a way on (shoot one to go there). On the screen in front of the
 * player those boxes hang on the screen's plane, but the gun is told where
 * its laser meets the world behind them - from the hand, not the eye, a
 * point some degrees off the box aimed at. So the quads the game draws in 2D
 * (glBegin, an identity modelview, not a perspective projection) are noted
 * each frame as rectangles in the game's pixels; host_xr.c aims at the
 * screen's plane where the laser crosses one (stereo_aim_box). */
#define MAX_AIM_BOXES 16
static float g_aim_box[2][MAX_AIM_BOXES][4];    /* [0]: the last whole frame's, [1]: this one's */
static float g_aim_dist[2][MAX_AIM_BOXES];      /* how far in front of the game camera each hangs */
static float g_imm_z;
static int   g_n_aim_box[2];
static int   g_mv_ident = 1, g_mv_ident_stack[32], g_mv_ident_top;
static int   g_imm, g_imm_n;                    /* inside a glBegin we measure; its vertices */
static float g_imm_box[4];
static GLuint g_view_tex[16];                   /* textures the game copies its rendering into */
static int   g_n_view_tex;

static void aim_box_track(CPU *c, int k)
{
    if (g_replaying) return;
    if (g_mode == GL_MODELVIEW) {
        if (k == K_LOADID) g_mv_ident = 1;
        else if (k == K_PUSHMATRIX) { if (g_mv_ident_top < 31) g_mv_ident_stack[g_mv_ident_top++] = g_mv_ident; }
        else if (k == K_POPMATRIX) { if (g_mv_ident_top > 0) g_mv_ident = g_mv_ident_stack[--g_mv_ident_top]; }
        else if (k == K_LOADMATRIX || k == K_MULTMATRIX || k == K_TRANSLATE || k == K_SCALE ||
                 k == K_ORTHO || k == K_ORTHO2D || k == K_FRUSTUM || k == K_PERSPECTIVE) g_mv_ident = 0;
    }
    if (k == K_COPYTEXSUB || k == K_COPYTEX) {         /* a texture the game renders into: a view */
        /* the game's own name for it, as the draws bind it: in the headset GL
         * may have a larger stand-in of a render target bound instead */
        GLint t = 0;
        if (g_active_unit == GL_TEXTURE0) t = (GLint)g_tex2d_bound;
        else glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
        int known = 0;
        for (int i = 0; i < g_n_view_tex; i++) known |= g_view_tex[i] == (GLuint)t;
        if (t && !known && g_n_view_tex < 16) g_view_tex[g_n_view_tex++] = (GLuint)t;
        return;
    }
    if (k == K_BEGIN) {
        int view = 0;
        for (int i = 0; i < g_n_view_tex; i++) view |= g_view_tex[i] == g_tex2d_bound;
        g_imm = view && !g_skip_begin && g_mv_ident && g_tex2d_on;
        { static int dbg = -1; if (dbg < 0) dbg = getenv("LINDBERGH_AIM_BOX_DEBUG") != NULL;
          if (dbg && g_n_view_tex) fprintf(stderr, "[aimbox?] frame %u begin %#x: tex %u view %d ident %d tex2d %d skip %d persp %d (%d view textures)\n",
                                   g_frame, A32(0), g_tex2d_bound, view, g_mv_ident, g_tex2d_on, g_skip_begin,
                                   is_perspective(g_proj[g_proj_top]), g_n_view_tex); }
        g_imm_n = 0; g_imm_z = 0;
        g_imm_box[0] = g_imm_box[1] = 1e9f; g_imm_box[2] = g_imm_box[3] = -1e9f;
    } else if (k == K_VERTEX && g_imm) {
        const char *n = hle_name(g_cur_id);
        float x = n[9] == 'i' ? (float)AI32(0) : argf(c, 0), y = n[9] == 'i' ? (float)AI32(1) : argf(c, 1);
        float z = n[8] == '3' ? argf(c, 2) : 0;
        const float *m = g_proj[g_proj_top];
        float w = m[3] * x + m[7] * y + m[11] * z + m[15];
        if (w <= 1e-6f) { g_imm = 0; return; }
        float nx = (m[0] * x + m[4] * y + m[8] * z + m[12]) / w, ny = (m[1] * x + m[5] * y + m[9] * z + m[13]) / w;
        float gx = GAME_W * 0.5f * (1 + nx), gy = GAME_H * 0.5f * (1 - ny);
        g_imm_z += z;
        if (gx < g_imm_box[0]) g_imm_box[0] = gx; if (gy < g_imm_box[1]) g_imm_box[1] = gy;
        if (gx > g_imm_box[2]) g_imm_box[2] = gx; if (gy > g_imm_box[3]) g_imm_box[3] = gy;
        g_imm_n++;
        if (getenv("LINDBERGH_AIM_BOX_DEBUG") && g_frame % 10 == 0)
            fprintf(stderr, "[aimbox?] frame %u vertex %.3f,%.3f,%.3f -> %.1f,%.1f (proj %.3f %.3f %.3f %.3f)\n", g_frame, x, y, z, gx, gy, m[0], m[5], m[8], m[9]);
    } else if (k == K_END && g_imm) {
        g_imm = 0;
        float w = g_imm_box[2] - g_imm_box[0], h = g_imm_box[3] - g_imm_box[1], a = w * h, all = (float)GAME_W * GAME_H;
        if (g_imm_n >= 3 && a >= all * 0.005f && a <= all * 0.6f && g_n_aim_box[1] < MAX_AIM_BOXES) {
            int i = g_n_aim_box[1]++;
            memcpy(g_aim_box[1][i], g_imm_box, sizeof g_imm_box);
            /* where it hangs: on the screen for a flat projection, else at its
             * own depth in front of the game camera (the modelview is identity) */
            g_aim_dist[1][i] = is_perspective(g_proj[g_proj_top]) ? -g_imm_z / (float)g_imm_n : g_screen_dist;
        }
    }
}

static void aim_box_frame(void)
{
    static int log = -1;
    if (log < 0) { const char *v = getenv("LINDBERGH_AIM_BOX_LOG"); log = v ? atoi(v) : 0; }
    /* =2: every tenth frame a box is up, not only when their number changes */
    if (log && g_n_aim_box[1] && (g_n_aim_box[1] != g_n_aim_box[0] || (log == 2 && g_frame % 10 == 0))) {
        fprintf(stderr, "[aimbox] frame %u:", g_frame);
        for (int i = 0; i < g_n_aim_box[1]; i++)
            fprintf(stderr, " %.0f,%.0f-%.0f,%.0f at %.2f", g_aim_box[1][i][0], g_aim_box[1][i][1], g_aim_box[1][i][2],
                    g_aim_box[1][i][3], g_aim_dist[1][i]);
        fputc('\n', stderr);
    }
    memcpy(g_aim_box[0], g_aim_box[1], sizeof g_aim_box[0]);
    memcpy(g_aim_dist[0], g_aim_dist[1], sizeof g_aim_dist[0]);
    g_n_aim_box[0] = g_n_aim_box[1];
    g_n_aim_box[1] = 0;
}

static void from_level(const float in[3], float out[3]);
/* the last whole frame's route box i, on the game's screen; 0 when there's none */
int stereo_aim_box_rect(int i, float r[4])
{
    if (i < 0 || i >= g_n_aim_box[0]) return 0;
    memcpy(r, g_aim_box[0][i], sizeof(float) * 4);
    return 1;
}

int stereo_aim_box(const float o_[3], const float d_[3], float *x, float *y, float *reach)
{
    float o[3], d[3];
    from_level(o_, o); from_level(d_, d);
    if (d[2] > -1e-4f) return 0;
    for (int i = 0; i < g_n_aim_box[0]; i++) {
        float dist = g_aim_dist[0][i], t = (-dist - o[2]) / d[2];
        if (t <= 0 || dist <= 0) continue;
        /* where the laser crosses the box's plane, as the game camera sees it */
        float u = (o[0] + t * d[0]) * g_screen_lens[0] / dist, v = (o[1] + t * d[1]) * g_screen_lens[1] / dist;
        float sx = GAME_W * 0.5f * (1 + u), sy = GAME_H * 0.5f * (1 - v);
        const float *b = g_aim_box[0][i];
        if (sx >= b[0] && sx <= b[2] && sy >= b[1] && sy <= b[3]) { *x = sx; *y = sy; *reach = t; return 1; }
    }
    return 0;
}

/* ---- 2D over the whole screen ----
 * A fade or a wipe covers the game's whole screen; put on the screen in front
 * of the player it is a rectangle in the scene instead of the change of
 * scene it is. A 2D draw that covers the screen goes over the whole eye
 * (effective_projection's hud 3). An array draw is measured as it is made; a
 * glBegin only at its glEnd, when gl4es draws it. */
static int   g_fill_track;
static float g_fill_box[4];

static int fills_screen(CPU *c, const char *n)
{
    float b[4];
    return ndc_box(c, n, b) && b[0] <= -0.98f && b[2] >= 0.98f && b[1] <= -0.98f && b[3] >= 0.98f;
}

static void fill_track(CPU *c, int k)
{
    if (!g_fill_track) return;
    if (k == K_VERTEX) {
        const char *n = hle_name(g_cur_id);
        float x = n[9] == 'i' ? (float)AI32(0) : argf(c, 0), y = n[9] == 'i' ? (float)AI32(1) : argf(c, 1);
        float z = n[8] == '3' ? argf(c, 2) : 0;
        const float *m = g_proj[g_proj_top];
        float w = m[3] * x + m[7] * y + m[11] * z + m[15];
        if (w <= 1e-6f) { g_fill_track = 0; return; }
        float nx = (m[0] * x + m[4] * y + m[8] * z + m[12]) / w, ny = (m[1] * x + m[5] * y + m[9] * z + m[13]) / w;
        if (nx < g_fill_box[0]) g_fill_box[0] = nx; if (ny < g_fill_box[1]) g_fill_box[1] = ny;
        if (nx > g_fill_box[2]) g_fill_box[2] = nx; if (ny > g_fill_box[3]) g_fill_box[3] = ny;
    } else if (k == K_END) {
        g_fill_track = 0;
        if (g_fill_box[0] <= -0.98f && g_fill_box[2] >= 0.98f && g_fill_box[1] <= -0.98f && g_fill_box[3] >= 0.98f) {
            clip_done();
            g_loaded_hud = 3;
            load_projection();
            g_proj_dirty = 1;           /* the next draw decides afresh */
        }
    }
}

static int transform_call(CPU *c, HleId id, const char *n)
{
    int k = cls(id)->kind;
    g_cur_id = id;
    aim_box_track(c, k);
    fill_track(c, k);
    if (g_skip_begin) {
        if (k == K_END) g_skip_begin = 0;
        return 1;
    }
    if (k == K_OTHER) return 0;
    if (g_on && (k == K_COPYTEXSUB || k == K_COPYTEX)) return copy_from_eyes(c, k);
    if (g_on && g_mv && !g_side && (is_draw(k) || k == K_BEGIN) && g_tex2d_on && g_nscreen_copy && screen_copy(g_tex2d_bound)) {
        if (k == K_BEGIN) g_skip_begin = 1;
        return 1;
    }
    if (k == K_BINDFB) {
        bind_framebuffer(A32(0), A32(1));
        return 1;
    }
    if (k == K_CLEAR) {
        /* The game clears its world target's depth but not its colour: on
         * the cabinet the 3D covered all of its view. The eye sees far wider
         * than the game's camera and the game draws only what that camera
         * sees, so outside it last frame's pixels stayed and dragged along
         * whenever the camera or the head moved. Clear the colour too. */
        GLbitfield mask = A32(0);
        if (g_mv && g_pbuffers && (g_game_fb == 0 || g_side))
            side_pass(!(g_vp[0] <= 0 && g_vp[1] <= 0 && g_vp[0] + g_vp[2] >= GAME_W && g_vp[1] + g_vp[3] >= GAME_H));
        if (g_side) return 0;
        /* Only the frame's first such clear, and not for a flat picture: the
         * game clears depth again to draw over its finished scene (the
         * scope's reload), and clearing the colour there wiped the scene.
         * Nor in a frame that cleared its colour already: The House of the
         * Dead 4 clears depth alone only for its HUD, over the scene. */
        static unsigned cleared_frame = ~0u, colour_frame = ~0u;
        if ((mask & GL_COLOR_BUFFER_BIT) && g_game_fb == g_world_fb) colour_frame = g_frame;
        if ((mask & GL_DEPTH_BUFFER_BIT) && !(mask & GL_COLOR_BUFFER_BIT) && g_game_fb == g_world_fb &&
            !g_flat && cleared_frame != g_frame && colour_frame != g_frame) {
            cleared_frame = g_frame;
            GLfloat cc[4];
            glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
            glClearColor(0, 0, 0, 1);
            glClear(mask | GL_COLOR_BUFFER_BIT);
            glClearColor(cc[0], cc[1], cc[2], cc[3]);
            return 1;
        }
        /* Depth and stencil cleared in one call, with both eyes in one pass,
         * cleared the right eye's depth to nothing on the Quest's driver: every
         * draw there then failed its depth test, and the right eye showed only
         * the clear colour (The House of the Dead 4 clears all three at once).
         * Stencil goes in a call of its own. */
        if (g_mv && (mask & GL_STENCIL_BUFFER_BIT) && (mask & GL_DEPTH_BUFFER_BIT)) {
            glClear(mask & ~GL_STENCIL_BUFFER_BIT);
            glClear(GL_STENCIL_BUFFER_BIT);
            return 1;
        }
        return 0;
    }
    if (k == K_VIEWPORT || k == K_SCISSOR) {
        GLint x = AI32(0), y = AI32(1), w = AI32(2), h = AI32(3);
        if (n[2] == 'V') { g_vp[0] = x; g_vp[1] = y; g_vp[2] = w; g_vp[3] = h; }
        else { g_game_scissor[0] = x; g_game_scissor[1] = y; g_game_scissor[2] = w; g_game_scissor[3] = h; }
        apply_rect(n[2] == 'V');
        return 1;
    }
    if (k == K_FBSTATUS) {
        /* an incomplete target sends a game down another path: say so */
        GLenum st = glCheckFramebufferStatus(A32(0));
        if (st != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "[stereo] framebuffer %u incomplete (%#x)\n", g_game_fb, st);
        c->eax = st;
        return 1;
    }
    if (k == K_TEXIMAGE) {
        GLenum ifmt = A32(2);
        int is_float = ifmt == 0x881A || ifmt == 0x881B || ifmt == 0x8814 || ifmt == 0x8815;   /* RGBA/RGB 16F, 32F */
        static int rt8 = -1;           /* LINDBERGH_RT_8BIT=1: not kept (a debugging aid) */
        if (rt8 < 0) { const char *v = getenv("LINDBERGH_RT_8BIT"); rt8 = v && *v == '1'; }
        if (A32(0) == GL_TEXTURE_2D && A32(1) == 0 && is_float && !rt8) {
            GLint tex = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
            int i = texfmt_find((GLuint)tex);
            if (i < 0 && g_ntexfmt < MAX_TEXFMT) i = (int)g_ntexfmt++;
            if (i >= 0) {
                g_texfmt[i].tex = (GLuint)tex; g_texfmt[i].ifmt = ifmt;
                g_texfmt[i].fmt = A32(6); g_texfmt[i].type = A32(7);
            }
        }
        return 0;
    }
    if (k == K_FBTEX2D) {
        scale_target(A32(2), A32(3), A32(1));
        note_fb_scale(g_game_fb, A32(3));
        if (!g_side) fb_scale(g_game_fb, g_cur_k);
        if (A32(1) == GL_COLOR_ATTACHMENT0) {
            note_fb_target(g_game_fb, A32(3));
            unsigned i = 0;
            while (i < g_nfb_color && g_fb_color[i].fb != g_game_fb) i++;
            if (i < MAX_FB_K) { if (i == g_nfb_color) g_nfb_color++; g_fb_color[i].fb = g_game_fb; g_fb_color[i].tex = A32(3); }
        }
        if (A32(1) == GL_DEPTH_ATTACHMENT || A32(1) == GL_DEPTH_STENCIL_ATTACHMENT) note_fb_depth(g_game_fb, A32(3));
        return 0;
    }
    if (k == K_ACTIVETEX) { g_active_unit = A32(0); return 0; }
    if (k == K_BINDTEX) {
        unsigned u = g_active_unit - GL_TEXTURE0;
        if (A32(0) == GL_TEXTURE_2D && u == 0) g_tex2d_bound = A32(1);
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
        if (g_env_proj) {       /* the projection is in the program's constants */
            float P[16];
            if (arb_env_projection(P) && memcmp(P, g_proj[g_proj_top], sizeof P)) {
                memcpy(g_proj[g_proj_top], P, sizeof P);
                g_proj_dirty = 1;
            }
        }
        int persp = is_perspective(g_proj[g_proj_top]);
        if (persp && !g_flat && (g_game_fb == g_world_fb || g_game_fb == g_world_fb_next)) {
            if (!g_replaying) tally_lens(g_proj[g_proj_top]);
            if (is_draw(k) && weapon_draw()) return 1;
        }
        if (persp && g_game_fb != g_hud_fb && !g_replaying && !offscreen_fb(g_game_fb)) { g_scene_draws++; g_world_fb_next = g_game_fb; if (g_env_proj) arb_camera_count(); }
        if (persp && g_proj[g_proj_top][0] > g_lens_seen) g_lens_seen = g_proj[g_proj_top][0];
        if (persp && !g_replaying && g_game_fb != g_hud_fb) {
            const float *P = g_proj[g_proj_top];
            float zn = P[14] / (P[10] - 1.0f);          /* the camera's near plane */
            if (zn > 0 && (g_near_seen == 0 || zn < g_near_seen)) g_near_seen = zn;
        }
        /* A long lens - the sniper scope, a cutscene's close-up - is a
         * magnified view the eye's own frustum would undo: it goes on the
         * screen too, framed by the scope the game draws there. */
        int zoom = persp && g_proj[g_proj_top][0] > zoom_limit() && g_zoom_frames >= 6;
        if (persp && !g_replaying) { if (zoom) g_zoom_draws++; if (g_game_fb == g_hud_fb) g_hudfb_draws++; }
        int hud = persp ? g_game_fb == g_hud_fb || menu_state() || g_menu || zoom
                        : !(g_unit0_rect || picture_2d(g_tex2d_bound) || picture_fb(g_game_fb)) || !covers_viewport(c, n);
        if (offscreen_fb(g_game_fb) || g_flat) hud = 2;  /* the game's own projection, untouched */
        if (hud == 1 && !persp && g_mv && fills_screen(c, n)) hud = 3;
        g_fill_track = hud == 1 && !persp && g_mv && k == K_BEGIN;    /* glBegin: measured until glEnd */
        if (g_fill_track) { g_fill_box[0] = g_fill_box[1] = 1e9f; g_fill_box[2] = g_fill_box[3] = -1e9f; }
        if (hud == 1 && !is_perspective(g_proj[g_proj_top]) && !g_replaying) g_hud_fb_next = g_game_fb;
        if (!persp && !g_replaying) { if (hud) { if (!g_hud2d) g_hud_lens = g_last_persp[0]; g_hud2d++; } else g_pass2d++; }
        if (hud == 1 && !persp && !g_replaying && sight_draw(c, n)) return 1;
        if (hud == 1 && !persp && letterbox_bar(c, n)) return 1;
        if (g_wide) {
            int span = g_wide_span;
            g_wide_span = 0;
            float sh = hud == 1 && !persp ? wide_place(c, n) : 0;
            if (sh != g_wide_s || span != g_wide_span) { g_wide_s = sh; g_proj_dirty = 1; }
        }
        if (hud != g_loaded_hud) { g_loaded_hud = hud; g_proj_dirty = 1; }
        if (g_proj_dirty) load_projection();
        if (hud == 1 && persp && !g_menu_vp[2] && (g_vp[2] != GAME_W || g_vp[3] != GAME_H)) memcpy(g_menu_vp, g_vp, sizeof g_vp);
        if (hud == 1) clip_to_screen();
        else if (hud == 3) clip_done();
        /* The game's 2D all sits at one depth and is drawn with LEQUAL, so on
         * the arcade the last draw wins. Put on the screen, coplanar quads
         * round to slightly different depths and fight pixel by pixel - the
         * HUD's gauges and text flickered in stripes. They still test against
         * the world; they just stop writing depth for each other. */
        if (hud == 1 && !persp) {
            GLboolean wm = GL_FALSE;
            glGetBooleanv(GL_DEPTH_WRITEMASK, &wm);
            if (wm) { glDepthMask(GL_FALSE); g_hud_nowrite = 1; }
        }
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
    else if (k == K_PUSHMATRIX) { if (g_proj_top < 31) { memcpy(g_proj[g_proj_top + 1], top, 64); g_proj_top++; } return 1; }
    else if (k == K_POPMATRIX) { if (g_proj_top > 0) g_proj_top--; g_proj_dirty = 1; return 1; }
    else if (k == K_TRANSLATE) { mat_identity(m); m[12] = argf(c, 0); m[13] = argf(c, 1); m[14] = argf(c, 2); mat_mul(top, top, m); }
    else if (k == K_SCALE) { mat_identity(m); m[0] = argf(c, 0); m[5] = argf(c, 1); m[10] = argf(c, 2); mat_mul(top, top, m); }
    else return 0;
    g_proj_dirty = 1;
    return 1;
}

/* Answers the game must get in its own terms. */
static void fix_query(CPU *c, int k)
{
    if (k == K_GETINTEGERV && A32(0) == GL_VIEWPORT) {
        GLint *o = (GLint *)APTR(1); if (o) memcpy(o, g_vp, sizeof g_vp);
    } else if (k == K_GETINTEGERV && (A32(0) == GL_FRAMEBUFFER_BINDING)) {
        GLint *o = (GLint *)APTR(1); if (o) *o = g_side ? 0 : (GLint)g_game_fb;
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
    if (is_draw(k) || k == K_END) {
        fixed_function_rect_done(); clip_done();
        if (g_hud_nowrite) { glDepthMask(GL_TRUE); g_hud_nowrite = 0; }
    }
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
static void track_arrays(CPU *c, int t)
{
    /* t: the call, as cls() numbered it - names compared once, not per call */
    Arr *a = NULL;
    if (t == T_ENABLECS || t == T_DISABLECS) {
        int i = client_array(A32(0)); if (i >= 0) g_arr[i].on = t == T_ENABLECS;
    } else if (t == T_CLIENTTEX || t == T_CLIENTTEXARB) {
        g_client_unit = (int)(A32(0) - GL_TEXTURE0);
    } else if (t == T_ENABLEVA || t == T_DISABLEVA) {
        if (A32(0) < 16) g_arr[ARR_ATTR0 + A32(0)].on = t == T_ENABLEVA;
    } else if (t == T_VERTEXPTR) { a = &g_arr[ARR_VERTEX]; a->size = A32(0); a->type = A32(1); a->stride = AI32(2); a->ptr = A32(3); }
    else if (t == T_NORMALPTR) { a = &g_arr[ARR_NORMAL]; a->size = 3; a->type = A32(0); a->stride = AI32(1); a->ptr = A32(2); }
    else if (t == T_COLORPTR) { a = &g_arr[ARR_COLOR]; a->size = A32(0); a->type = A32(1); a->stride = AI32(2); a->ptr = A32(3); }
    else if (t == T_TEXCOORDPTR) { a = &g_arr[ARR_TEX0 + (g_client_unit & 7)]; a->size = A32(0); a->type = A32(1); a->stride = AI32(2); a->ptr = A32(3); }
    else if (t == T_ATTRIBPTR) {
        if (A32(0) >= 16) return;
        a = &g_arr[ARR_ATTR0 + A32(0)]; a->size = A32(1); a->type = A32(2); a->norm = A32(3); a->stride = AI32(4); a->ptr = A32(5);
        {   /* LINDBERGH_ATTRIB_LOG=1: each kind of generic array the game gives */
            static int on = -1, n; static uint32_t seen[64];
            if (on < 0) { const char *v = getenv("LINDBERGH_ATTRIB_LOG"); on = v && *v == '1'; }
            uint32_t key = A32(0) << 24 | A32(1) << 20 | (A32(2) & 0xFFFF) << 4 | (A32(3) & 1);
            int k = 0;
            while (k < n && seen[k] != key) k++;
            if (on && k == n && n < 64) {
                seen[n++] = key;
                fprintf(stderr, "[attrib] index %u: %u x %#x, normalised %u, stride %d\n", A32(0), A32(1), A32(2), A32(3), AI32(4));
            }
        }
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
    if (k->track) track_arrays(c, k->track);
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
    gl4es_mv_trace = ((long)g_frame == at || (long)g_frame == g_rt_dump_at) && getenv("LIBGL_MV_CHECK");
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
static double g_glprof_ns[HLE_ID_MAX];
static unsigned g_glprof_n[HLE_ID_MAX];
static unsigned g_glprof_f0;
static double g_glprof_rec_ns;          /* of which recording for the second eye */
static double g_frame_ns[HLE_ID_MAX];    /* this frame's, to name what a slow frame spent */
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
    for (unsigned i = 0; i < HLE_ID_MAX; i++) { gl += g_frame_ns[i]; if (g_frame_ns[i] > g_frame_ns[best]) best = i; }
    static double slow = -1;            /* LINDBERGH_GLPROF_SLOW=<ms>: which frames to name (40) */
    if (slow < 0) { const char *v = getenv("LINDBERGH_GLPROF_SLOW"); slow = (v && *v ? atof(v) : 40) * 1e6; }
    if (all > slow)
        fprintf(stderr, "[glprof] frame %u took %.0f ms, GL %.0f ms of it, most in %s (%.0f ms)\n", g_frame,
                all / 1e6, gl / 1e6, hle_name((HleId)best), g_frame_ns[best] / 1e6);
#ifdef LINDBERGH_GL4ES
    {   /* buffer uploads: the driver's, and gl4es' own copy */
        extern double gl4es_drawprof[4], gl4es_bufprof_copy, gl4es_bufprof_bytes;
        if (all > slow && gl4es_bufprof_bytes > 0)
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
    for (unsigned i = 0; i < HLE_ID_MAX; i++) total += g_glprof_ns[i], calls += g_glprof_n[i];
    extern double g_arb_draw_ns;
    fprintf(stderr, "[glprof] frames %u..%u: GL %.1f ms (recording %.1f, ARB programs %.1f), %.0f calls per frame; costliest:",
            g_glprof_f0, g_frame, total / frames / 1e6, g_glprof_rec_ns / frames / 1e6, g_arb_draw_ns / frames / 1e6, calls / frames);
    g_glprof_rec_ns = g_arb_draw_ns = 0;
    for (int k = 0; k < 10; k++) {
        unsigned best = 0;
        for (unsigned i = 1; i < HLE_ID_MAX; i++) if (g_glprof_ns[i] > g_glprof_ns[best]) best = i;
        if (g_glprof_ns[best] <= 0) break;
        fprintf(stderr, " %s %.2f ms/%.0f", hle_name((HleId)best), g_glprof_ns[best] / frames / 1e6, g_glprof_n[best] / frames);
        g_glprof_ns[best] = -g_glprof_ns[best];
    }
    fprintf(stderr, "; most called:");
    {   /* the call count is what the per-call overhead multiplies */
        static unsigned shown[HLE_ID_MAX];
        memset(shown, 0, sizeof shown);
        static int most = -1;
        if (most < 0) { const char *v = getenv("LINDBERGH_GLPROF_CALLS"); most = v && *v ? atoi(v) : 16; }
        for (int k = 0; k < most; k++) {
            unsigned best = 0;
            for (unsigned i = 1; i < HLE_ID_MAX; i++) if (!shown[i] && g_glprof_n[i] > g_glprof_n[best]) best = i;
            if (!best || !g_glprof_n[best]) break;
            shown[best] = 1;
            fprintf(stderr, " %s %.0f", hle_name((HleId)best), g_glprof_n[best] / frames);
        }
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
    /* Most calls in a busy frame - texture and fog parameters, uniforms, the
     * lighting state - are nothing to this file in the headset: no view, no
     * target, no array, nothing to record. Straight to them. */
    if (g_mv && g_on && !g_skip_begin && g_glprof <= 0 && (long)g_frame != g_map_frame) {
        const Cls *k = cls(id);
        if (k->kind == K_OTHER && !k->track
#ifdef LINDBERGH_GL4ES
            && !gl4es_mv_trace
#endif
           ) { fn(c); return; }
    }
    stereo_glprof_on();
#ifdef LINDBERGH_GL4ES
    if ((long)g_frame == g_map_frame && g_mv) {
        /* the frame map: the eye's centre pixel after each draw */
        const char *nm = hle_name(id);
        static double t_last;
        int draw = !strncmp(nm, "glDraw", 6) || !strncmp(nm, "glMultiDraw", 11) || !strcmp(nm, "glEnd");
        if (draw && !t_last) { glFinish(); t_last = now_ns(); }
        stereo_gl_call_inner(c, id, fn);
        if (draw) {
            glFinish();             /* offscreen targets too: a tiler runs them only when needed */
            unsigned char px[4], pr[4];
            gl4es_multiview_read_pixel(g_ew / 2, g_eh / 2, px);
            gl4es_multiview_read_pixel_layer(g_ew / 2, g_eh / 2, 1, pr);
            /* the read waits for the GPU: the time since the last is this draw's */
            double t = now_ns();
            GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
            GLuint ct = 0;
            for (unsigned i = 0; i < g_nfb_color; i++) if (g_fb_color[i].fb == g_game_fb) ct = g_fb_color[i].tex;
            if (g_game_fb && ct) {  /* an offscreen target (8-bit, LINDBERGH_RT_8BIT): its centre and lower middle */
                static unsigned char *buf;
                if (!buf) buf = malloc(4096u * 4096u * 4u);
                int tw = vp[0] + vp[2], th = vp[1] + vp[3];
                if (buf && tw * th <= 4096 * 4096 && gl4es_multiview_read(ct, GL_TEXTURE_2D, 0, buf)) {
                    const unsigned char *a0 = buf + ((size_t)(th / 2) * tw + tw / 2) * 4, *a1 = buf + ((size_t)(th / 4) * tw + tw / 2) * 4;
                    fprintf(stderr, "[map]     target %u centre %u %u %u %u, lower %u %u %u %u\n", ct,
                            a0[0], a0[1], a0[2], a0[3], a1[0], a1[1], a1[2], a1[3]);
                }
            }
            fprintf(stderr, "[map]   %s %.2f ms, centre now %u %u %u %u, right eye %u %u %u (fb %u, viewport %dx%d, v%u f%u)\n", nm,
                    (t - t_last) / 1e6, px[0], px[1], px[2], px[3], pr[0], pr[1], pr[2], g_game_fb, vp[2], vp[3], arb_bound_vp(), arb_bound_fp());
            t_last = now_ns();
        }
        return;
    }
#endif
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
    {   /* A render target drawn larger still reports the size the game made
         * it: the plane's air shimmer builds its lookups from the size it is
         * told, in texels, which the lookups then scaled a second time. */
        static signed char tlp[HLE_ID_MAX];
        if (!tlp[id]) tlp[id] = strcmp(hle_name(id), "glGetTexLevelParameteriv") ? 2 : 1;
        if (tlp[id] == 1 && g_on && A32(0) == GL_TEXTURE_RECTANGLE_ARB && A32(1) == 0 &&
            (A32(2) == GL_TEXTURE_WIDTH || A32(2) == GL_TEXTURE_HEIGHT)) {
            GLint b = 0;
            glGetIntegerv(0x84F6 /* GL_TEXTURE_BINDING_RECTANGLE_ARB */, &b);
            for (unsigned i = 0; b && i < g_nscaled; i++)
                if (g_scaled[i] == (GLuint)b) {
                    wr32(A32(3), (uint32_t)(A32(2) == GL_TEXTURE_WIDTH ? g_scaled_w[i] : g_scaled_h[i]));
                    return;
                }
        }
    }
    if ((long)g_frame == g_map_frame) {
        uint32_t a[6]; for (int i = 0; i < 6; i++) a[i] = A32(i);
        const char *nm = hle_name(id);
        if (!strncmp(nm, "glDraw", 6) || !strncmp(nm, "glMultiDraw", 11) || !strcmp(nm, "glBegin")) {
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
    /* the client arrays' state, for telling what a 2D draw covers (with two
     * passes record() keeps it) */
    if ((g_mv || g_wide) && cls(id)->track) track_arrays(c, cls(id)->track);
    if (!g_mv && !g_wide) {
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
    /* LINDBERGH_EYE_DUMP=<first>,<count>: the left eye's image every frame
     * of a run, as eye_<frame>.ppm - for finding what flickers */
    static long e0 = -2, en, es = 1;
    if (e0 == -2) {
        const char *v = getenv("LINDBERGH_EYE_DUMP");
        e0 = v && *v ? atol(v) : -1;
        en = v && strchr(v, ',') ? atol(strchr(v, ',') + 1) : 1;
        const char *s2 = v && strchr(v, ',') ? strchr(strchr(v, ',') + 1, ',') : NULL;
        if (s2) es = atol(s2 + 1) > 0 ? atol(s2 + 1) : 1;   /* ,<stride>: every stride-th frame, a quarter the size */
    }
    if (e0 >= 0 && (long)g_frame >= e0 && (long)g_frame < e0 + en && ((long)g_frame - e0) % es == 0) {
        for (int layer = 0; layer < (g_mv ? 2 : 1); layer++) {     /* eye_ the left eye, eyeR_ the right */
        GLint w = g_ew, h = g_eh;
        unsigned char *px = malloc((size_t)w * h * 4);
        int ok = 0;
#ifdef LINDBERGH_GL4ES
        if (px && g_mv) ok = gl4es_multiview_read(g_eye_tex[0], GL_TEXTURE_RECTANGLE_ARB, layer, px);
#endif
        if (px && !g_mv && !ok) {       /* the eyes drawn one at a time: read the first eye's framebuffer */
            GLint was = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was);
            glBindFramebuffer(GL_FRAMEBUFFER, g_eye_fbo[0]);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)was);
            ok = 1;
        }
        if (ok && (es > 1 || en > 10)) {   /* many frames: a quarter the size */
            int qw = w / 4, qh = h / 4;
            for (int y = 0; y < qh; y++)
                for (int x = 0; x < qw; x++) memcpy(px + ((size_t)y * qw + x) * 4, px + ((size_t)y * 4 * w + x * 4) * 4, 4);
            w = qw; h = qh;
        }
        if (ok) { char name[64]; snprintf(name, sizeof name, layer ? "eyeR_%u.ppm" : "eye_%u.ppm", g_frame); write_ppm(name, w, h, px); }
        free(px);
        }
#ifdef LINDBERGH_GL4ES
        /* LINDBERGH_EYE_DUMP_TEX=a,b,...: those render targets every frame too */
        const char *xt = getenv("LINDBERGH_EYE_DUMP_TEX");
        for (const char *q = xt; g_mv && q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : NULL) {
            GLuint tex = (GLuint)atoi(q);
            GLint tw = 0, th = 0, was = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB, &was);
            glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
            glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE_ARB, 0, GL_TEXTURE_WIDTH, &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE_ARB, 0, GL_TEXTURE_HEIGHT, &th);
            glBindTexture(GL_TEXTURE_RECTANGLE_ARB, (GLuint)was);
            GLenum tt = GL_TEXTURE_RECTANGLE_ARB;
            if (tw <= 0 || th <= 0) {       /* a 2D render target */
                tt = GL_TEXTURE_2D;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &was);
                glBindTexture(GL_TEXTURE_2D, tex);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
                glBindTexture(GL_TEXTURE_2D, (GLuint)was);
            }
            unsigned char *tp = tw > 0 && th > 0 ? malloc((size_t)tw * th * 4) : NULL;
            if (tp && gl4es_multiview_read(tex, tt, 0, tp)) {
                char name[64]; snprintf(name, sizeof name, "t%u_%u.ppm", tex, g_frame); write_ppm(name, tw, th, tp);
            }
            free(tp);
        }
#endif
    }
    static long at = -2, every;
    if (at == -2) {
        const char *v = getenv("LINDBERGH_RT_DUMP"); at = v && *v ? atol(v) : -1;
        v = getenv("LINDBERGH_EYE_DUMP_EVERY"); every = v && *v ? atol(v) : 0;
    }
    if (every > 0 && g_frame > 0 && g_frame % (unsigned long)every == 0) {
        /* LINDBERGH_EYE_DUMP_EVERY=N: the first eye every N frames, eye_<frame>.ppm */
        unsigned char *px = malloc((size_t)g_ew * g_eh * 4);
#ifdef LINDBERGH_GL4ES
        for (int layer = 0; px && g_mv && layer < 2; layer++)
            if (gl4es_multiview_read(g_eye_tex[0], GL_TEXTURE_RECTANGLE_ARB, layer, px)) {
                char name[64];
                snprintf(name, sizeof name, layer ? "eyeR_%u.ppm" : "eye_%u.ppm", g_frame);
                write_ppm(name, g_ew, g_eh, px);
            }
#endif
        free(px);
    }
    if ((long)g_frame != at && (long)g_frame != g_rt_dump_at) return;
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
#ifdef LINDBERGH_GL4ES
        if (g_mv && tg == GL_TEXTURE_2D && i < g_nscaled) {   /* and the 2D texture itself, which is what is sampled */
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            snprintf(name, sizeof name, "rt2d_%u.ppm", tex);
            write_ppm(name, w, h, px);
        }
#endif
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

/* The game turning its camera hard - Let's Go Jungle swings round to a
 * spider on the jeep's tailgate - measured over a third of a second, the
 * camera's direction taken from the draws' matrices (arb_camera_basis). The
 * headset puts the game's view back in front of the player then
 * (stereo_camera_swung).
 *
 * The horizon kept level (stereo_level_horizon): the game tips its camera
 * too - 70 degrees down into the jeep for its bees, for many seconds - and
 * in the headset that tips the whole world. Its turning left and right is
 * kept; its pitch and roll go into where its view sits instead: the eyes see
 * the world from a level camera, the HUD and the sight lie where the game
 * looks, and the player looks down at the bees. g_level takes the game
 * camera's space to the level one. */
#define SWING_SPAN 20
static float    g_cam_hist[SWING_SPAN][3];
static unsigned g_cam_n, g_swung;
static int      g_level_on;
static float    g_level[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
static float    g_head_view[2][16];     /* level space -> eye, as the headset gave it */

void stereo_level_horizon(int on) { g_level_on = on; }

static void update_level(const float r[3], const float u[3], const float f[3])
{
    static float hf[3] = { 0, 0, -1 };      /* the level heading, kept while the camera looks straight down */
    float h = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (h > 0.2f) { hf[0] = f[0] / h; hf[1] = 0; hf[2] = f[2] / h; }
    else if (h < 1e-3f) return;
    /* the level frame: right = heading x up, up, back */
    const float L[3][3] = { { -hf[2], 0, hf[0] }, { 0, 1, 0 }, { -hf[0], 0, -hf[2] } };
    const float *C[3] = { r, u, NULL };
    float cb[3] = { -f[0], -f[1], -f[2] };
    C[2] = cb;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            g_level[j * 4 + i] = L[i][0] * C[j][0] + L[i][1] * C[j][1] + L[i][2] * C[j][2];
    for (int e = 0; e < 2; e++) mat_mul(g_view[e], g_head_view[e], g_level);
    g_proj_dirty = 1;
}

/* a direction or point in level space -> the game camera's (g_level's transpose) */
static void from_level(const float in[3], float out[3])
{
    for (int j = 0; j < 3; j++) out[j] = g_level[j * 4] * in[0] + g_level[j * 4 + 1] * in[1] + g_level[j * 4 + 2] * in[2];
}

static void track_camera_swing(void)
{
    float r[3], u[3], d[3];
    if (!arb_camera_basis(r, u, d)) { g_cam_n = 0; return; }
    if (g_level_on) update_level(r, u, d);
    static int log = -1;
    if (log < 0) { const char *v = getenv("LINDBERGH_CAM_LOG"); log = v && *v && *v != '0'; }
    if (g_cam_n >= SWING_SPAN) {
        const float *o = g_cam_hist[g_cam_n % SWING_SPAN];
        float c = o[0] * d[0] + o[1] * d[1] + o[2] * d[2];
        if (log && g_frame % 30 == 0)
            fprintf(stderr, "[cam] frame %u: looking %.2f %.2f %.2f, up %.2f %.2f %.2f, %.0f deg in %d frames\n",
                    g_frame, d[0], d[1], d[2], u[0], u[1], u[2], acosf(c > 1 ? 1 : c < -1 ? -1 : c) * 57.2958f, SWING_SPAN);
        if (c < 0.82f && g_frame - g_swing_at > 90) {      /* more than ~35 degrees */
            g_swung = 1; g_swing_at = g_frame;
            fprintf(stderr, "[cam] frame %u: the game swung its camera %.0f deg\n", g_frame, acosf(c < -1 ? -1 : c) * 57.2958f);
        }
    }
    memcpy(g_cam_hist[g_cam_n % SWING_SPAN], d, sizeof d);
    g_cam_n++;
}

int stereo_camera_swung(void) { int s = g_swung; g_swung = 0; return s; }

void stereo_frame_end(void)
{
    if (!g_on) return;
    if (g_env_proj) track_camera_swing();
    g_read_at_leave = -1;
    if (g_depth_read == 1) g_read_at_leave = 0;             /* LG_DEPTH_FIRST_LEAVE */
    else if (g_depth_read == 0)                             /* LG_DEPTH_AUTO */
        for (unsigned i = 0; i < g_leaves && i < 16; i++)
            if (g_leave_draws[i] == g_scene_draws) { g_read_at_leave = (int)i; break; }
    g_leaves = 0;
    g_flat_shown = g_flat;
    dump_targets();
    if (!g_mv && !g_wide) replay(1);
    if (g_overflow) { fprintf(stderr, "[stereo] frame %u: recording overflowed %u times\n", g_frame, g_overflow); g_overflow = 0; }
}

void stereo_frame_begin(void)
{
    aim_box_frame();
    glprof_report();
    if (!g_on) return;
    g_nops = 0;
    g_nfmt = 0;
    g_arena_used = 0;
    /* Menus put a few 3D models among 2D panels and the arcade monitor's edge
     * framed them; the game world takes far more draws. Two seconds of
     * menu-sized frames turns the 3D into screen content (gameplay has
     * sparse moments of a second or so; a menu stays), a quarter second
     * of world-sized ones turns it back (one busy frame used to flip it). */
    static unsigned world_frames;
    if (g_scene_draws >= g_menu_draws) {
        g_menu_frames = 0;
        if (++world_frames >= 15) g_menu = 0;
    } else {
        world_frames = 0;
        if (++g_menu_frames >= 120) g_menu = 1;
    }
    if (getenv("LINDBERGH_SCENE_STATS") && g_frame % 30 == 0)
        fprintf(stderr, "[stereo] frame %u: %u world draws%s, lens %.2f, near %.4f; world %.3f %.3f (%.3f %.3f), screen lens %.3f %.3f\n",
                g_frame, g_scene_draws, g_menu ? " (menu)" : "", g_lens_seen, g_near_seen, g_world_persp[0], g_world_persp[5],
                g_world_persp[8], g_world_persp[9], g_screen_lens[0], g_screen_lens[1]);
    static int mode_log = -1, last_sig = -1;
    if (mode_log < 0) { const char *v = getenv("LINDBERGH_MODE_LOG"); mode_log = v && *v && *v != '0'; }
    if (mode_log) {
        int sig = g_menu | (g_zoom_draws ? 2 : 0) | (g_hudfb_draws ? 4 : 0) | (g_flat ? 8 : 0);
        if (sig != last_sig)
            fprintf(stderr, "[mode] frame %u: flat %d, menu %d, zoom draws %u, 3D into HUD target %u, world draws %u, lens %.2f\n",
                    g_frame, g_flat, g_menu, g_zoom_draws, g_hudfb_draws, g_scene_draws, g_lens_seen);
        last_sig = sig;
    }
    static int hud_log = -1;
    static char last_hud[160];
    if (hud_log < 0) { const char *v = getenv("LINDBERGH_HUD_LOG"); hud_log = v && *v && *v != '0'; }
    if (hud_log) {
        char sig[160];
        snprintf(sig, sizeof sig, "HUD target %d, world target %d, 2D on screen %u, passes %u, 3D into HUD %u, menu state %d, screen 3D viewport %d,%d %dx%d",
                 (int)g_hud_fb_next, (int)g_world_fb_next, g_hud2d, g_pass2d, g_hudfb_draws,
                 menu_state(), g_menu_vp[0], g_menu_vp[1], g_menu_vp[2], g_menu_vp[3]);
        memset(g_menu_vp, 0, sizeof g_menu_vp);
        if (strcmp(sig, last_hud)) fprintf(stderr, "[hud] frame %u: %s\n", g_frame, sig);
        strcpy(last_hud, sig);
    }
    /* The screen is as wide as the game camera's view: the game turns the gun's
     * point on its screen into a shot through its own lens, so a screen of
     * any other width aims every shot off the centre's way (fixed at 1.73,
     * the jungle's 2.41 put shots ~30% wide of the laser). A flat picture -
     * a menu, a scope - has a size of its own, below. */
    {
        int best = 0;
        for (int i = 1; i < 4; i++) if (g_lens_tally[i].n > g_lens_tally[best].n) best = i;
        if (g_lens_tally[best].n) memcpy(g_world_persp, g_lens_tally[best].p, 64);
        memset(g_lens_tally, 0, sizeof g_lens_tally);
        g_weapon_frame_draws = g_weapon_draws;
        g_weapon_draws = 0;
    }
    if (!g_flat && g_scene_draws > 0 && is_perspective(g_world_persp) && g_world_persp[0] > 0.3f && g_world_persp[0] < zoom_limit()) {
        g_screen_lens[0] = g_world_persp[0];
        g_screen_lens[1] = g_world_persp[5];
    }
    g_hud2d = g_pass2d = 0;
    g_zoom_draws = g_hudfb_draws = 0;
    /* a scope is held; a lens passing through on a camera move is not one */
    g_zoom_frames = g_lens_seen > zoom_limit() ? g_zoom_frames + 1 : 0;
    g_lens_seen = 0;
    g_near_seen = 0;
    g_scene_draws = 0;
    {
        static int flat_env = -1;
        if (flat_env < 0) { const char *v = getenv("LINDBERGH_FLAT_MENUS"); flat_env = !v || *v != '0'; }
        static int flat_all = -1;       /* LINDBERGH_FLAT_ALL=1: everything flat, the arcade's picture - for comparing */
        if (flat_all < 0) { const char *v = getenv("LINDBERGH_FLAT_ALL"); flat_all = v && *v == '1'; }
        /* A scope held up is a flat picture too: the arcade's own image on
         * the screen, nothing around it. Put on the screen as 3D, the game's
         * full-screen haze passes ran over the empty room and turned it a
         * flat lavender. */
        g_flat = flat_all || (flat_env && (menu_state() || g_zoom_frames >= 6));
        /* Widescreen: a sniping event is the arcade's 4:3 from its start - the
         * scope's mask is drawn for the 4:3 screen, and waiting for its lens
         * to hold left the first frames wide, the mask's corners at the edges
         * and the world around them */
        if (g_wide && flat_env && g_snipe_frame != ~0u && g_frame - g_snipe_frame <= 2) g_flat = 1;
    }
    /* A flat picture is the arcade's whole monitor: the screen at the size
     * it has in play (lens 1.73). Kept as last followed, a scope coming up -
     * its lens ramps 2.4, 8.4, 11.4 over a few frames - left the screen a
     * third of its size, and the magnified picture looked like no zoom. */
    if (g_flat) { g_screen_lens[0] = 1.732f; g_screen_lens[1] = 1.732f * GAME_W / GAME_H; }
    g_hud_fb = g_hud_fb_next;           /* 3D drawn there this frame is screen content */
    g_world_fb = g_world_fb_next;
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
    g_cur_k[0] = g_sx; g_cur_k[1] = g_sy;
}

void stereo_set_eye_target(int eye, unsigned fbo)
{
    g_external[eye] = 1;
    g_eye_fbo[eye] = fbo;
}

/* head -> eye, for a HUD that follows the head */
void stereo_set_eye_head(int eye, const float view[16]) { memcpy(g_hud_view[eye], view, 64); }

void stereo_set_eye(int eye, const float view[16], float tl, float tr, float td, float tu)
{
    memcpy(g_head_view[eye], view, 64);
    mat_mul(g_view[eye], view, g_level);
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
    /* a pass over the world's own picture, in the eyes' pixels already: as is */
    if (g_env_proj && !is_perspective(p) && !g_loaded_hud) return;
    if (!is_perspective(p)) p = g_last_persp;
    float eff[16], inv[16];
    effective_projection(p, g_loaded_hud, eff);
    if (mat_invert(p, inv)) mat_mul(out, eff, inv);
}

void stereo_set_screen_distance(float d) { if (d > 0) g_screen_dist = d; }

/* The depth this frame's world left in both eyes (a multiview array, layer
 * per eye) and the near and far planes it was drawn with, in game units: for
 * the headset, which uses it to correct a repeated frame for the head's
 * movement as well as its turn. 0 when the frame has no world in stereo (a
 * flat frame, no world drawn, or the eyes drawn one at a time). */
unsigned stereo_world_depth(float *zn, float *zf)
{
#ifdef LINDBERGH_GL4ES
    if (!g_on || !g_mv || g_flat || g_scene_draws == 0) return 0;
    GLuint dt = 0;
    for (unsigned i = 0; i < g_nfb_depth; i++) if (g_fb_depth[i].fb == g_world_fb_next) dt = g_fb_depth[i].depth;
    if (g_world_fb_next == 0) dt = g_eye_tex[1];      /* the window: the eyes' own target */
    unsigned arr = dt ? gl4es_multiview_array(dt) : 0;
    const float *p = g_world_persp;
    float n = p[14] / (p[10] - 1), f = p[14] / (p[10] + 1);
    if (!arr || !(n > 0) || !(f > n)) return 0;
    *zn = n; *zf = f;
    return arr;
#else
    (void)zn; (void)zf;
    return 0;
#endif
}

/* Where a ray from the game camera's space crosses the HUD's screen, in the
 * game's pixels. 0 when it points away from the screen. */
/* Points along the gun's ray (game units, the game camera's space), placed
 * in the first eye's view of this frame's world: their pixel there and their
 * depth, to be compared with the depth the world left - where the ray first
 * goes behind a surface is what it points at. 0 if there is no such view (a
 * flat frame, or the eyes drawn one at a time). */
int stereo_aim_samples(const float o_[3], const float d_[3], int max, float (*px)[2], float *z, float *t, unsigned *depth, float *zn, float *zf)
{
#ifdef LINDBERGH_GL4ES
    float o[3], d[3];
    from_level(o_, o); from_level(d_, d);
    if (!g_on || !g_mv || g_flat) return 0;
    GLuint dt = 0;
    for (unsigned i = 0; i < g_nfb_depth; i++) if (g_fb_depth[i].fb == g_world_fb_next) dt = g_fb_depth[i].depth;
    unsigned arr = dt ? gl4es_multiview_array(dt) : 0;
    static unsigned dbg;
    if (getenv("LINDBERGH_AIM_LOG") && ++dbg % 120 == 1) {
        fprintf(stderr, "[aim] world fb %u, depth texture %u, array %u; depth attachments:", g_world_fb, dt, arr);
        for (unsigned i = 0; i < g_nfb_depth; i++) fprintf(stderr, " fb%u=%u", g_fb_depth[i].fb, g_fb_depth[i].depth);
        fprintf(stderr, "\n");
    }
    if (!arr) return 0;
    const float *p = g_world_persp;
    float n = p[14] / (p[10] - 1), f = p[14] / (p[10] + 1), e[16], m[16];
    if (!(n > 0) || !(f > n)) return 0;
    const float *tn = g_tan[0];
    mat_frustum(e, -tn[0] * n, tn[1] * n, -tn[2] * n, tn[3] * n, n, f);
    mat_mul(m, e, g_view[0]);
    int k = 0;
    for (int i = 0; i < max; i++) {
        float ti = 0.5f * powf(200.0f, (float)i / (float)(max - 1));     /* 0.5 to 100 units */
        float q[3] = { o[0] + d[0] * ti, o[1] + d[1] * ti, o[2] + d[2] * ti };
        float cx = m[0] * q[0] + m[4] * q[1] + m[8] * q[2] + m[12];
        float cy = m[1] * q[0] + m[5] * q[1] + m[9] * q[2] + m[13];
        float cz = m[2] * q[0] + m[6] * q[1] + m[10] * q[2] + m[14];
        float cw = m[3] * q[0] + m[7] * q[1] + m[11] * q[2] + m[15];
        if (cw <= 1e-5f) continue;
        float x = cx / cw, y = cy / cw;
        if (x <= -1 || x >= 1 || y <= -1 || y >= 1) continue;
        px[k][0] = (x * 0.5f + 0.5f) * (float)g_ew;
        px[k][1] = (y * 0.5f + 0.5f) * (float)g_eh;
        z[k] = cz / cw * 0.5f + 0.5f;
        t[k] = ti;
        k++;
    }
    *depth = arr; *zn = n; *zf = f;
    return k;
#else
    (void)o_; (void)d_; (void)max; (void)px; (void)z; (void)t; (void)depth; (void)zn; (void)zf;
    return 0;
#endif
}

int stereo_ray_to_screen(const float o_[3], const float d_[3], float *x, float *y)
{
    float o[3], d[3];
    from_level(o_, o); from_level(d_, d);
    if (d[2] > -1e-4f) return 0;
    float t = (-g_screen_dist - o[2]) / d[2];
    if (t <= 0) return 0;
    float hx = o[0] + t * d[0], hy = o[1] + t * d[1];
    float u = hx * g_screen_lens[0] / g_screen_dist, v = hy * g_screen_lens[1] / g_screen_dist;
    *x = GAME_W * 0.5f * (1 + u);
    *y = GAME_H * 0.5f * (1 - v);
    return 1;
}

void stereo_game_size(int *w, int *h) { *w = GAME_W; *h = GAME_H; }

int stereo_is_render_target(unsigned tex) { return g_on && scaled(tex); }

void stereo_rect_scale(float *x, float *y)
{
    *x = g_on ? g_sx : 1; *y = g_on ? g_sy : 1;
}

unsigned stereo_eye_texture(int eye) { return g_eye_tex[eye]; }
#ifdef LINDBERGH_GL4ES
unsigned stereo_eye_array(void) { return g_mv ? gl4es_multiview_array(g_eye_tex[0]) : 0; }
#endif
unsigned stereo_eye_framebuffer(int eye) { return g_eye_fbo[eye]; }

/* Desktop check: both eyes side by side in the window (widescreen: the one
 * image, filling it). */
void stereo_present_desktop(int win_w, int win_h)
{
    if (!g_on) return;
    if (g_wide) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_eye_fbo[0]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        if (g_flat) {       /* a menu, a scope: the arcade's 4:3, black at the sides */
            glViewport(0, 0, win_w, win_h);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            int x0 = (int)(win_w * (1 - g_wide_k) / 2 + 0.5f), x1 = win_w - x0;
            glBlitFramebuffer(0, 0, g_ew, g_eh, x0, 0, x1, win_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        } else
            glBlitFramebuffer(0, 0, g_ew, g_eh, 0, 0, win_w, win_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, g_eye_fbo[0]);
        return;
    }
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
    {
        int w = 0, h = 0;
        lindbergh_window_size(&w, &h);
        if (w > 0 && h > 0) { g_game_w = w; g_game_h = h; }
        g_vp[2] = g_game_w; g_vp[3] = g_game_h;
    }
    if (g_game_w * 3 != g_game_h * 4) g_screen_lens[1] = g_screen_lens[0] * g_game_w / g_game_h;
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
    if (!v || !*v || *v == '0') {
        stereo_eye_size(GAME_W, GAME_H);
        /* LINDBERGH_WIDE=1 (1920x1080) or =WxH: the widescreen image */
        const char *w = getenv("LINDBERGH_WIDE");
        int ww = 1920, wh = 1080;
        if (w && *w && *w != '0') {
            if (strchr(w, 'x') && sscanf(w, "%dx%d", &ww, &wh) != 2) { ww = 1920; wh = 1080; }
            stereo_start_wide(ww, wh);
        }
        return;
    }
    stereo_start();
}

void stereo_start_wide(int w, int h)
{
    watch_states();                         /* menus: shown as the arcade's 4:3 */
    if (g_on || w <= 0 || h <= 0) return;
    void *a = guest_alloc(ARENA_SIZE, 64);
    if (!a) { fprintf(stderr, "[stereo] no guest memory - widescreen off\n"); return; }
    g_arena = H2G(a);
    stereo_eye_size(w, h);
    g_wide = 1;
    g_wide_k = (float)GAME_W / GAME_H * h / w;
    float m[16]; mat_identity(m);
    for (int i = 0; i < 2; i++) stereo_set_eye(i, m, 1, 1, 1, 1);
    g_on = 1;
    fprintf(stderr, "[stereo] widescreen: %dx%d, the 4:3 picture %.0f%% of its width\n", w, h, g_wide_k * 100);
}

int stereo_wide(int *w, int *h, float *k)
{
    if (!g_wide) return 0;
    if (w) *w = g_ew;
    if (h) *h = g_eh;
    if (k) *k = g_wide_k;
    return 1;
}

/* Desktop defaults: a 90-degree square eye, 6.4 cm apart. A VR host
 * replaces all of it every frame. */
void stereo_start(void)
{
    watch_states();
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
    fprintf(stderr, "[stereo] on: %dx%d per eye (x%.2f, x%.2f of the game's %dx%d)\n", g_ew, g_eh, g_sx, g_sy, GAME_W, GAME_H);
}

void stereo_set_gun(int x, int y)
{
    if (x < -1000 || y < -1000) { g_gun_ndc[0] = g_gun_ndc[1] = -9; return; }
    g_gun_ndc[0] = x / (GAME_W * 0.5f) - 1;
    g_gun_ndc[1] = 1 - y / (GAME_H * 0.5f);
}

void stereo_show_game_sight(int show)
{
    g_show_sight = show;
    fprintf(stderr, "[stereo] game sight %s (texture %u)\n", show ? "shown" : "hidden", g_sight_tex);
}

