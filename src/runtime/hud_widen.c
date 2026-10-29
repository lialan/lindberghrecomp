/*
 * hud_widen.c - a 2D layout made for a narrower screen, drawn at its own shape
 * on a wider one.
 *
 * A game whose 3D supports a wide monitor but whose 2D was only ever laid out
 * for a narrower one draws that layout stretched across the frame. Its 2D
 * arrives as immediate-mode quads with an identity modelview, so each quad is
 * held back until glEnd, measured in normalised device coordinates, and drawn
 * again through a projection that squeezes it back to the layout's own width:
 * x' = f x + s. Where it goes is decided by where it was:
 *
 *   - a quad that covers the screen (fades, flashes, frames) is left alone;
 *   - one in the middle third stays centred (s = 0);
 *   - one further out keeps to its edge (s = +-(1 - f)), as a HUD on a wide
 *     monitor would.
 *
 * A quad that continues the one before it - the next letter of a word, the
 * next piece of a gauge - goes wherever that one went, so a string is never
 * split between the middle and an edge.
 *
 * hud_widen_enable(f, anchored) turns it on: f is the layout's width as a
 * fraction of the frame's (0.75 for 4:3 in 16:9), anchored() says whether the
 * edges apply now (gameplay) or everything is centred (menus, attract).
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lindbergh_rt.h"
#include "gl_posix.h"

extern unsigned g_frame;
static float g_f;
static int (*g_anchored)(void);
/* the layout's own coordinates, when it was positioned for a smaller frame
 * than it is drawn into: NDC x -> lx x + lox, y -> ly y + loy, applied to quads
 * drawn into a viewport at least g_layout_vw wide, before the widening */
static float g_lx = 1, g_lox, g_ly = 1, g_loy;
static int g_layout_vw;

/* the modelview, as far as it matters here: identity or not */
static GLenum g_mode = GL_MODELVIEW;
static int g_mv_identity;

enum { OP_COLOR4UB, OP_TEXCOORD2F, OP_MULTITEX2F, OP_VERTEX3F };
typedef struct { int op; GLenum target; float v[4]; } Op;
#define MAX_OPS 1024
static Op g_ops[MAX_OPS];
static int g_nops, g_recording, g_overflow;
static GLenum g_prim;

/* the previous quad, for keeping words and gauges together */
static float g_last_box[4];     /* x0 x1 y0 y1, NDC */
static float g_last_s;
static unsigned g_last_frame = ~0u;

static HleHandler o_glMatrixMode, o_glLoadIdentity, o_glLoadMatrixf, o_glMultMatrixf, o_glTranslatef,
                  o_glScalef, o_glRotatef, o_glPushMatrix, o_glPopMatrix, o_glBegin, o_glEnd,
                  o_glColor4ub, o_glTexCoord2f, o_glMultiTexCoord2f, o_glVertex3f, o_glVertex3fv;

static float argf(CPU *c, int i) { uint32_t b = A32(i); float f; memcpy(&f, &b, 4); return f; }

static void mv_changed(void) { if (g_mode == GL_MODELVIEW) g_mv_identity = 0; }

static void w_glMatrixMode(CPU *c)   { g_mode = A32(0); o_glMatrixMode(c); }
static void w_glLoadIdentity(CPU *c) { if (g_mode == GL_MODELVIEW) g_mv_identity = 1; o_glLoadIdentity(c); }
static void w_glLoadMatrixf(CPU *c)  { mv_changed(); o_glLoadMatrixf(c); }
static void w_glMultMatrixf(CPU *c)  { mv_changed(); o_glMultMatrixf(c); }
static void w_glTranslatef(CPU *c)   { mv_changed(); o_glTranslatef(c); }
static void w_glScalef(CPU *c)       { mv_changed(); o_glScalef(c); }
static void w_glRotatef(CPU *c)      { mv_changed(); o_glRotatef(c); }
static void w_glPushMatrix(CPU *c)   { o_glPushMatrix(c); }
static void w_glPopMatrix(CPU *c)    { mv_changed(); o_glPopMatrix(c); }

static void push(int op, GLenum target, float a, float b, float cc, float d)
{
    if (g_nops >= MAX_OPS) { g_overflow = 1; return; }
    Op *o = &g_ops[g_nops++];
    o->op = op; o->target = target; o->v[0] = a; o->v[1] = b; o->v[2] = cc; o->v[3] = d;
}

static void w_glBegin(CPU *c)
{
    if (g_f > 0 && g_mv_identity && A32(0) == GL_QUADS) {
        g_recording = 1; g_nops = 0; g_overflow = 0; g_prim = A32(0);
        return;
    }
    o_glBegin(c);
}

static void w_glColor4ub(CPU *c)
{
    if (!g_recording) { o_glColor4ub(c); return; }
    push(OP_COLOR4UB, 0, A32(0) & 0xFF, A32(1) & 0xFF, A32(2) & 0xFF, A32(3) & 0xFF);
}
static void w_glTexCoord2f(CPU *c)
{
    if (!g_recording) { o_glTexCoord2f(c); return; }
    push(OP_TEXCOORD2F, 0, argf(c, 0), argf(c, 1), 0, 0);
}
static void w_glMultiTexCoord2f(CPU *c)
{
    if (!g_recording) { o_glMultiTexCoord2f(c); return; }
    push(OP_MULTITEX2F, A32(0), argf(c, 1), argf(c, 2), 0, 0);
}
static void w_glVertex3f(CPU *c)
{
    if (!g_recording) { o_glVertex3f(c); return; }
    push(OP_VERTEX3F, 0, argf(c, 0), argf(c, 1), argf(c, 2), 0);
}
static void w_glVertex3fv(CPU *c)
{
    if (!g_recording) { o_glVertex3fv(c); return; }
    const float *v = (const float *)APTR(0);
    push(OP_VERTEX3F, 0, v[0], v[1], v[2], 0);
}

static void replay(void)
{
    glBegin(g_prim);
    for (int i = 0; i < g_nops; i++) {
        const Op *o = &g_ops[i];
        switch (o->op) {
        case OP_COLOR4UB:   glColor4ub((GLubyte)o->v[0], (GLubyte)o->v[1], (GLubyte)o->v[2], (GLubyte)o->v[3]); break;
        case OP_TEXCOORD2F: glTexCoord2f(o->v[0], o->v[1]); break;
        case OP_MULTITEX2F: glMultiTexCoord2f(o->target, o->v[0], o->v[1]); break;
        case OP_VERTEX3F:   glVertex3f(o->v[0], o->v[1], o->v[2]); break;
        }
    }
    glEnd();
}

/* where along x this quad goes: s in x' = f x + s */
static float place(const float box[4])
{
    float w = box[1] - box[0], h = box[3] - box[2], cx = (box[0] + box[1]) / 2;
    float edge = 1 - g_f;
    /* continuing the previous quad: same row, starting about where it ended */
    if (g_last_frame == g_frame) {
        float lh = g_last_box[3] - g_last_box[2];
        int same_row = fabsf(box[2] - g_last_box[2]) < 0.5f * (h > lh ? h : lh) + 0.01f;
        int follows = box[0] > g_last_box[0] - 0.01f && box[0] < g_last_box[1] + 0.25f * (w + h) + 0.02f;
        if (same_row && follows) return g_last_s;
    }
    if (!g_anchored || !g_anchored()) return 0;
    if (cx < -1.0f / 3) return -edge;
    if (cx >  1.0f / 3) return  edge;
    return 0;
}

static void w_glEnd(CPU *c)
{
    if (!g_recording) { o_glEnd(c); return; }
    g_recording = 0;

    GLfloat p[16];
    glGetFloatv(GL_PROJECTION_MATRIX, p);
    float box[4] = { 1e9f, -1e9f, 1e9f, -1e9f };
    int nv = 0;
    for (int i = 0; i < g_nops; i++) {
        const Op *o = &g_ops[i];
        if (o->op != OP_VERTEX3F) continue;
        float x = o->v[0], y = o->v[1], z = o->v[2];
        float cx = p[0] * x + p[4] * y + p[8] * z + p[12];
        float cy = p[1] * x + p[5] * y + p[9] * z + p[13];
        float cw = p[3] * x + p[7] * y + p[11] * z + p[15];
        if (fabsf(cw) < 1e-6f) continue;
        float nx = cx / cw, ny = cy / cw;
        if (nx < box[0]) box[0] = nx;
        if (nx > box[1]) box[1] = nx;
        if (ny < box[2]) box[2] = ny;
        if (ny > box[3]) box[3] = ny;
        nv++;
    }
    /* covers the screen, or nothing to measure: as it was */
    if (g_overflow || !nv || box[1] - box[0] >= 1.9f) { replay(); return; }

    /* the layout's coordinates into the frame's */
    float lx = 1, lox = 0, ly = 1, loy = 0;
    if (g_layout_vw) {
        GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
        if (vp[2] >= g_layout_vw) { lx = g_lx; lox = g_lox; ly = g_ly; loy = g_loy; }
    }
    box[0] = lx * box[0] + lox; box[1] = lx * box[1] + lox;
    box[2] = ly * box[2] + loy; box[3] = ly * box[3] + loy;
    if (box[1] - box[0] >= 1.9f) {          /* now covers the frame: scaled, not squeezed */
        GLfloat m[16];
        for (int col = 0; col < 4; col++) {
            m[col * 4 + 0] = lx * p[col * 4 + 0] + lox * p[col * 4 + 3];
            m[col * 4 + 1] = ly * p[col * 4 + 1] + loy * p[col * 4 + 3];
            m[col * 4 + 2] = p[col * 4 + 2];
            m[col * 4 + 3] = p[col * 4 + 3];
        }
        GLint mode; glGetIntegerv(GL_MATRIX_MODE, &mode);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadMatrixf(m);
        replay();
        glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode((GLenum)mode);
        return;
    }

    float s = place(box);
    memcpy(g_last_box, box, sizeof box);
    g_last_s = s;
    g_last_frame = g_frame;

    /* T * P, T taking NDC x to f (lx x + lox) + s and y to ly y + loy: in
     * clip space x' = f lx x + (f lox + s) w */
    GLfloat m[16];
    for (int col = 0; col < 4; col++) {
        m[col * 4 + 0] = g_f * lx * p[col * 4 + 0] + (g_f * lox + s) * p[col * 4 + 3];
        m[col * 4 + 1] = ly * p[col * 4 + 1] + loy * p[col * 4 + 3];
        m[col * 4 + 2] = p[col * 4 + 2];
        m[col * 4 + 3] = p[col * 4 + 3];
    }
    GLint mode; glGetIntegerv(GL_MATRIX_MODE, &mode);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(m);
    replay();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode((GLenum)mode);
}

void hud_widen_enable(float f, int (*anchored)(void))
{
    g_f = f;
    g_anchored = anchored;
    for (unsigned i = 0; i < HLE_COUNT; i++) {
        const char *n = hle_name((HleId)i);
        if (!g_hle_handlers[i]) continue;
#define HOOK(nm) if (!strcmp(n, #nm)) { o_##nm = g_hle_handlers[i]; g_hle_handlers[i] = w_##nm; continue; }
        HOOK(glMatrixMode) HOOK(glLoadIdentity) HOOK(glLoadMatrixf) HOOK(glMultMatrixf) HOOK(glTranslatef)
        HOOK(glScalef) HOOK(glRotatef) HOOK(glPushMatrix) HOOK(glPopMatrix) HOOK(glBegin) HOOK(glEnd)
        HOOK(glColor4ub) HOOK(glTexCoord2f) HOOK(glMultiTexCoord2f) HOOK(glVertex3f) HOOK(glVertex3fv)
#undef HOOK
    }
    fprintf(stderr, "[hud] 2D drawn at %.3f of the frame's width\n", f);
}

/* The 2D was positioned for a frame lx, ly times smaller than the viewport it
 * is drawn into (at least vw wide), anchored at (ox, oy) in NDC. */
void hud_widen_layout(float lx, float lox, float ly, float loy, int vw)
{
    g_lx = lx; g_lox = lox; g_ly = ly; g_loy = loy; g_layout_vw = vw;
}
