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
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_posix.h"
#include "stereo.h"

extern unsigned g_frame;

#define GAME_W 640
#define GAME_H 480

/* ---- eyes ---- */
static int      g_on;
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
static float    g_last_persp[16];       /* the game's last perspective */

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

/* What GL is given for the game's projection p. */
static void effective_projection(const float *p, float *out)
{
    if (!g_on || !is_perspective(p)) { memcpy(out, p, 64); return; }
    memcpy(g_last_persp, p, 64);
    float n = p[14] / (p[10] - 1), f = p[14] / (p[10] + 1);
    const float *t = g_tan[g_eye];
    float e[16];
    mat_frustum(e, -t[0] * n, t[1] * n, -t[2] * n, t[3] * n, n, f);
    mat_mul(out, e, g_view[g_eye]);
}

static void load_projection(void)
{
    float m[16];
    effective_projection(g_proj[g_proj_top], m);
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

static void eye_targets(void)
{
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

/* Returns 1 when it did the call's work itself. */
static int transform_call(CPU *c, const char *n)
{
    if (!strcmp(n, "glBindFramebufferEXT") || !strcmp(n, "glBindFramebuffer")) {
        bind_framebuffer(A32(0), A32(1));
        return 1;
    }
    if (!strcmp(n, "glViewport") || !strcmp(n, "glScissor")) {
        GLint x = AI32(0), y = AI32(1), w = AI32(2), h = AI32(3);
        if (n[2] == 'V') { g_vp[0] = x; g_vp[1] = y; g_vp[2] = w; g_vp[3] = h; }
        GLint X = (GLint)lroundf(x * g_sx), Y = (GLint)lroundf(y * g_sy);
        GLint W = (GLint)lroundf((x + w) * g_sx) - X, H = (GLint)lroundf((y + h) * g_sy) - Y;
        if (n[2] == 'V') glViewport(X, Y, W, H); else glScissor(X, Y, W, H);
        return 1;
    }
    if (!strcmp(n, "glFramebufferTexture2DEXT") || !strcmp(n, "glFramebufferTexture2D")) {
        scale_target(A32(2), A32(3));
        return 0;
    }
    if (!strcmp(n, "glMatrixMode")) { g_mode = A32(0); return 0; }
    if (g_mode != GL_PROJECTION) return 0;

    float *top = g_proj[g_proj_top], m[16];
    if (!strcmp(n, "glLoadIdentity")) mat_identity(top);
    else if (!strcmp(n, "glLoadMatrixf")) { const float *p = (const float *)APTR(0); if (p) memcpy(top, p, 64); }
    else if (!strcmp(n, "glMultMatrixf")) { const float *p = (const float *)APTR(0); if (p) mat_mul(top, top, p); }
    else if (!strcmp(n, "glOrtho")) { mat_ortho(m, gl_arg_d(c, 0), gl_arg_d(c, 2), gl_arg_d(c, 4), gl_arg_d(c, 6), gl_arg_d(c, 8), gl_arg_d(c, 10)); mat_mul(top, top, m); }
    else if (!strcmp(n, "gluOrtho2D")) { mat_ortho(m, gl_arg_d(c, 0), gl_arg_d(c, 2), gl_arg_d(c, 4), gl_arg_d(c, 6), -1, 1); mat_mul(top, top, m); }
    else if (!strcmp(n, "glFrustum")) { mat_frustum(m, gl_arg_d(c, 0), gl_arg_d(c, 2), gl_arg_d(c, 4), gl_arg_d(c, 6), gl_arg_d(c, 8), gl_arg_d(c, 10)); mat_mul(top, top, m); }
    else if (!strcmp(n, "gluPerspective")) {
        double zn = gl_arg_d(c, 4), t = zn * tan(gl_arg_d(c, 0) * M_PI / 360), r = t * gl_arg_d(c, 2);
        mat_frustum(m, -r, r, -t, t, zn, gl_arg_d(c, 6)); mat_mul(top, top, m);
    }
    else if (!strcmp(n, "glPushMatrix")) { if (g_proj_top < 31) { memcpy(g_proj[g_proj_top + 1], top, 64); g_proj_top++; } glPushMatrix(); return 1; }
    else if (!strcmp(n, "glPopMatrix")) { if (g_proj_top > 0) g_proj_top--; glPopMatrix(); load_projection(); return 1; }
    else if (!strcmp(n, "glTranslatef")) { mat_identity(m); m[12] = argf(c, 0); m[13] = argf(c, 1); m[14] = argf(c, 2); mat_mul(top, top, m); }
    else if (!strcmp(n, "glScalef")) { mat_identity(m); m[0] = argf(c, 0); m[5] = argf(c, 1); m[10] = argf(c, 2); mat_mul(top, top, m); }
    else return 0;
    load_projection();
    return 1;
}

/* Answers the game must get in its own terms. */
static void fix_query(CPU *c, const char *n)
{
    if (!strcmp(n, "glGetIntegerv") && A32(0) == GL_VIEWPORT) {
        GLint *o = (GLint *)APTR(1); if (o) memcpy(o, g_vp, sizeof g_vp);
    } else if (!strcmp(n, "glGetIntegerv") && (A32(0) == GL_FRAMEBUFFER_BINDING)) {
        GLint *o = (GLint *)APTR(1); if (o) *o = (GLint)g_game_fb;
    } else if (!strcmp(n, "glGetFloatv") && A32(0) == GL_PROJECTION_MATRIX) {
        float *o = (float *)APTR(1); if (o) memcpy(o, g_proj[g_proj_top], 64);
    } else if (!strcmp(n, "glGetFloatv") && A32(0) == GL_VIEWPORT) {
        float *o = (float *)APTR(1); if (o) for (int i = 0; i < 4; i++) o[i] = (float)g_vp[i];
    }
}

static void exec(CPU *c, HleId id, HleHandler fn)
{
    const char *n = hle_name(id);
    if (!transform_call(c, n)) fn(c);
    if (!g_replaying && n[2] == 'G' && n[3] == 'e' && n[4] == 't') fix_query(c, n);
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
static void pointer_arg(const char *n, CPU *c, int *idx, uint32_t *bytes)
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
    *idx = -1; *bytes = 0;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(n, T[i].name)) { *idx = T[i].idx; *bytes = T[i].bytes; return; }
    if (!strcmp(n, "glBufferDataARB") || !strcmp(n, "glBufferData")) { *idx = 2; *bytes = A32(1); }
    else if (!strcmp(n, "glBufferSubDataARB") || !strcmp(n, "glBufferSubData")) { *idx = 3; *bytes = A32(2); }
}

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
static int snapshot_draw(CPU *c, const char *n, uint32_t *args_va)
{
    int elements = !strcmp(n, "glDrawElements");
    if (!elements && strcmp(n, "glDrawArrays")) return 1;
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
    if (!strcmp(n, "glUnmapBufferARB") || !strcmp(n, "glUnmapBuffer")) {
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
    if (live_only(n)) return;
    track_arrays(c, n);
    uint32_t va = arena_take(ARGS * 4);
    if (!va) { g_overflow++; return; }
    memcpy(G2H(va), G2H(c->esp), ARGS * 4);
    /* the copies of the draw's vertices go in before the draw itself */
    if (!snapshot_draw(c, n, &va)) { g_overflow++; return; }
    Op *o = op_new();
    if (!o) { g_overflow++; return; }
    int idx; uint32_t bytes;
    pointer_arg(n, c, &idx, &bytes);
    if (idx >= 0 && bytes && A32(idx)) {
        if (bytes > (4u << 20)) { g_nops--; return; }       /* a load-time upload: not per frame */
        uint32_t copy = arena_take(bytes);
        if (!copy) { g_nops--; g_overflow++; return; }
        memcpy(G2H(copy), GP(A32(idx)), bytes);
        wr32(va + 4u * (uint32_t)idx, copy);
    }
    o->kind = OP_CALL; o->id = id; o->fn = fn; o->va = va;
}

static void stereo_gl_call(CPU *c, HleId id, HleHandler fn)
{
    if ((long)g_frame == g_map_frame) {
        uint32_t a[6]; for (int i = 0; i < 6; i++) a[i] = A32(i);
        fprintf(stderr, "[map] %s %#x %#x %#x %#x %#x %#x\n", hle_name(id), a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    if (!g_on) { fn(c); return; }
    const char *n = hle_name(id);
    record(c, id, fn, n);
    exec(c, id, fn);
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
        if (o->kind == OP_PTR) { g_arr[o->target] = g_ptr_fmt[o->n]; replay_pointer((int)o->target, o->va); continue; }
        memset(&cpu, 0, sizeof cpu);
        cpu.esp = o->va;
        exec(&cpu, o->id, o->fn);
    }
    memcpy(g_arr, live, sizeof live);
    g_replaying = 0;
}

void stereo_frame_end(void)
{
    if (!g_on) return;
    replay(1);
    if (g_overflow) { fprintf(stderr, "[stereo] frame %u: recording overflowed %u times\n", g_frame, g_overflow); g_overflow = 0; }
}

void stereo_frame_begin(void)
{
    if (!g_on) return;
    g_nops = 0;
    g_nfmt = 0;
    g_arena_used = 0;
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

void stereo_rect_scale(float *x, float *y)
{
    *x = g_on ? g_sx : 1; *y = g_on ? g_sy : 1;
}

unsigned stereo_eye_texture(int eye) { return g_eye_tex[eye]; }
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
