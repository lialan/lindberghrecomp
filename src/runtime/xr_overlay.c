/*
 * xr_overlay.c - what the headset adds to the game's picture: the gun in the
 * player's hand and the aiming laser.
 *
 * Drawn by host_xr.c into each eye's swapchain image after the game's frame
 * is copied there, with raw GLES (past gl4es). Every piece of GL state touched
 * is put back afterwards, so gl4es' cached view of the state stays true.
 *
 * Everything here is in metres, in the LOCAL reference space the headset
 * reports poses in. The gun is a low-poly SMG built from boxes in code: its
 * barrel lies along the controller's aim ray, so the laser leaves the muzzle.
 */
#if defined(__ANDROID__)

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>

#include "xr_overlay.h"

static struct {
#define F(ret, name, args) ret (*name) args;
    F(GLuint, CreateShader, (GLenum)) F(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
    F(void, CompileShader, (GLuint)) F(void, GetShaderiv, (GLuint, GLenum, GLint *))
    F(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) F(GLuint, CreateProgram, (void))
    F(void, AttachShader, (GLuint, GLuint)) F(void, BindAttribLocation, (GLuint, GLuint, const GLchar *))
    F(void, LinkProgram, (GLuint)) F(void, UseProgram, (GLuint)) F(GLint, GetUniformLocation, (GLuint, const GLchar *))
    F(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) F(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
    F(void, GenVertexArrays, (GLsizei, GLuint *)) F(void, BindVertexArray, (GLuint))
    F(void, GenBuffers, (GLsizei, GLuint *)) F(void, BindBuffer, (GLenum, GLuint))
    F(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum))
    F(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
    F(void, EnableVertexAttribArray, (GLuint)) F(void, DrawArrays, (GLenum, GLint, GLsizei))
    F(void, Enable, (GLenum)) F(void, Disable, (GLenum)) F(GLboolean, IsEnabled, (GLenum))
    F(void, GetIntegerv, (GLenum, GLint *)) F(void, GetBooleanv, (GLenum, GLboolean *))
    F(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) F(void, DepthMask, (GLboolean))
    F(void, DepthFunc, (GLenum)) F(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))
    F(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) F(void, Clear, (GLbitfield)) F(void, ClearDepthf, (GLfloat))
    F(void, FrontFace, (GLenum)) F(void, CullFace, (GLenum))
    F(void, ActiveTexture, (GLenum)) F(void, BindTexture, (GLenum, GLuint)) F(void, Uniform1i, (GLint, GLint))
    F(void, GenSamplers, (GLsizei, GLuint *)) F(void, BindSampler, (GLuint, GLuint))
    F(void, SamplerParameteri, (GLuint, GLenum, GLint)) F(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
    F(void, DepthRangef, (GLfloat, GLfloat)) F(void, GetFloatv, (GLenum, GLfloat *)) F(void, Uniform2f, (GLint, GLfloat, GLfloat))
    F(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
    F(void, GenTextures, (GLsizei, GLuint *)) F(void, TexParameteri, (GLenum, GLenum, GLint))
    F(void, GenFramebuffers, (GLsizei, GLuint *)) F(void, BindFramebuffer, (GLenum, GLuint))
    F(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
    F(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
    F(void *, MapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) F(GLboolean, UnmapBuffer, (GLenum))
    F(void, Uniform2iv, (GLint, GLsizei, const GLint *)) F(void, Uniform1f, (GLint, GLfloat))
    F(GLsync, FenceSync, (GLenum, GLbitfield)) F(GLenum, ClientWaitSync, (GLsync, GLbitfield, GLuint64))
    F(void, DeleteSync, (GLsync))
#undef F
} gl;

static GLuint prog_gun, prog_beam, vao_gun, vbo_gun, vao_beam, vbo_beam;
static GLuint prog_depth, smp_depth;    /* the world's depth, copied for the headset */
static GLint  u_depth_d, u_depth_layer, u_depth_size;
static GLuint prog_screen[2], vao_screen, vbo_screen, smp_screen;   /* [0] a 2D texture, [1] a layer of an array */
static GLint u_screen_mvp[2], u_screen_tex[2];
static GLuint prog_panel;
static GLint u_panel_mvp, u_panel_tex;
static GLint u_gun_mvp, u_gun_model, u_beam_mvp, u_beam_color, u_beam_shape;
static int gun_verts;

/* ---- the gun ----
 * Chosen by the game's profile: Quaternius' SubmachineGun_2 (CC0), made into
 * xr_gun_model.h by quest/tools/obj2gun.py; a B&T TP9 and the Ghost Squad
 * cabinet's gun, built by quest/tools/gunbuild.py (xr_gun_tp9.h, xr_gun_gs.h). Both: barrel along -Z on the aim
 * ray, the rail on top. A sight goes on the rail, built from boxes here: its
 * glass is where the reticle shows. */
#include "xr_gun_model.h"
#include "xr_gun_tp9.h"
#include "xr_gun_gs.h"

/* a debug switch, looked up once */
#define ENV_ON(name) ({ static int _on = -1; if (_on < 0) _on = getenv(name) != NULL; _on; })

/* the sights' boxes, y measured up from the rail's top */
typedef struct { float c[3], h[3], rgb[3]; } Box;
static const Box RED_DOT[] = {          /* a slim reflex sight, like a Trijicon RMR */
    { { 0, 0.003f, 0.028f }, { 0.0125f, 0.003f, 0.020f }, { 0.12f, 0.12f, 0.13f } },          /* base */
    { { 0, 0.008f, 0.040f }, { 0.0115f, 0.004f, 0.008f }, { 0.15f, 0.15f, 0.16f } },          /* emitter housing */
    { { -0.0110f, 0.0155f, 0.018f }, { 0.0015f, 0.0095f, 0.004f }, { 0.15f, 0.15f, 0.16f } }, /* hood: sides */
    { { 0.0110f, 0.0155f, 0.018f }, { 0.0015f, 0.0095f, 0.004f }, { 0.15f, 0.15f, 0.16f } },
    { { 0, 0.0255f, 0.018f }, { 0.0125f, 0.0015f, 0.004f }, { 0.15f, 0.15f, 0.16f } },        /* hood: top */
};
static const Box HOLO[] = {             /* a window sight, like an EOTech */
    { { 0, 0.008f, 0.032f }, { 0.017f, 0.008f, 0.048f }, { 0.12f, 0.12f, 0.13f } },           /* body */
    { { -0.019f, 0.032f, 0.018f }, { 0.003f, 0.020f, 0.022f }, { 0.15f, 0.15f, 0.16f } },     /* hood: sides */
    { { 0.019f, 0.032f, 0.018f }, { 0.003f, 0.020f, 0.022f }, { 0.15f, 0.15f, 0.16f } },
    { { 0, 0.050f, 0.018f }, { 0.022f, 0.003f, 0.022f }, { 0.15f, 0.15f, 0.16f } },          /* hood: top */
    { { 0.008f, 0.018f, 0.064f }, { 0.007f, 0.004f, 0.010f }, { 0.08f, 0.08f, 0.09f } },      /* buttons */
};
/* each sight's glass: centre z, half width, half height, and the sight line's height above the rail */
static const float GLASS[3][4] = { { 0, 0, 0, 0 }, { 0.018f, 0.0095f, 0.0085f, 0.0145f },
                                   { 0.004f, 0.016f, 0.017f, 0.030f } };

#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define MAX_VERTS MAX2(MAX2(GUN_VERTS, TP9_VERTS), GS_VERTS)
static float mesh[(MAX_VERTS + 8 * 36) * 9];
static float rail_y;                    /* the current gun's rail top */

static void add_boxes(int *n, const Box *bx, int count)
{
    static const int face[6][4] = { { 1, 3, 7, 5 }, { 0, 4, 6, 2 }, { 2, 6, 7, 3 },
                                    { 0, 1, 5, 4 }, { 4, 5, 7, 6 }, { 0, 2, 3, 1 } };
    static const float nrm[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int p = 0; p < count; p++) {
        float v[8][3];
        for (int k = 0; k < 8; k++)
            for (int a = 0; a < 3; a++)
                v[k][a] = bx[p].c[a] + (a == 1 ? rail_y : 0) + ((k >> a) & 1 ? bx[p].h[a] : -bx[p].h[a]);
        for (int f = 0; f < 6; f++) {
            static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
            for (int t = 0; t < 6; t++) {
                float *o = &mesh[(*n)++ * 9];
                memcpy(o, v[face[f][tri[t]]], 12);
                memcpy(o + 3, nrm[f], 12);
                memcpy(o + 6, bx[p].rgb, 12);
            }
        }
    }
}

static int cur_sight = -1, cur_model = -1;
static void build_gun(int sight, int model)
{
    int n;
    if (model == XO_GUN_TP9) { memcpy(mesh, TP9_MESH, sizeof TP9_MESH); n = TP9_VERTS; rail_y = TP9_RAIL_Y; }
    else if (model == XO_GUN_GHOST_SQUAD) { memcpy(mesh, GS_MESH, sizeof GS_MESH); n = GS_VERTS; rail_y = GS_RAIL_Y; }
    else { memcpy(mesh, GUN_MESH, sizeof GUN_MESH); n = GUN_VERTS; rail_y = GUN_RAIL_Y; }
    if (sight == XO_SIGHT_RED_DOT) add_boxes(&n, RED_DOT, (int)(sizeof RED_DOT / sizeof *RED_DOT));
    if (sight == XO_SIGHT_HOLO) add_boxes(&n, HOLO, (int)(sizeof HOLO / sizeof *HOLO));
    gun_verts = n;
    cur_sight = sight;
    cur_model = model;
}

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = gl.CreateShader(type);
    gl.ShaderSource(s, 1, &src, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512] = ""; gl.GetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "[overlay] shader: %s\n", log); }
    return s;
}

static GLuint program(const char *vs, const char *fs, const char *a0, const char *a1, const char *a2)
{
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, shader(GL_VERTEX_SHADER, vs));
    gl.AttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    gl.BindAttribLocation(p, 0, a0);
    if (a1) gl.BindAttribLocation(p, 1, a1);
    if (a2) gl.BindAttribLocation(p, 2, a2);
    gl.LinkProgram(p);
    return p;
}

int xo_init(void)
{
    void *lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return 0;
#define L(n) if (!(*(void **)&gl.n = dlsym(lib, "gl" #n))) { fprintf(stderr, "[overlay] no gl" #n "\n"); return 0; }
    L(CreateShader) L(ShaderSource) L(CompileShader) L(GetShaderiv) L(GetShaderInfoLog) L(CreateProgram)
    L(AttachShader) L(BindAttribLocation) L(LinkProgram) L(UseProgram) L(GetUniformLocation) L(UniformMatrix4fv)
    L(Uniform4f) L(GenVertexArrays) L(BindVertexArray) L(GenBuffers) L(BindBuffer) L(BufferData)
    L(VertexAttribPointer) L(EnableVertexAttribArray) L(DrawArrays) L(Enable) L(Disable) L(IsEnabled)
    L(GetIntegerv) L(GetBooleanv) L(BlendFuncSeparate) L(DepthMask) L(DepthFunc) L(ColorMask) L(Viewport)
    L(Clear) L(ClearDepthf) L(FrontFace) L(CullFace) L(ActiveTexture) L(BindTexture) L(Uniform1i)
    L(GenSamplers) L(BindSampler) L(SamplerParameteri) L(ClearColor) L(DepthRangef) L(GetFloatv) L(Uniform2f)
    L(TexImage2D) L(GenTextures) L(TexParameteri)
    L(GenFramebuffers) L(BindFramebuffer) L(FramebufferTexture2D) L(ReadPixels) L(MapBufferRange) L(UnmapBuffer)
    L(Uniform2iv) L(Uniform1f) L(FenceSync) L(ClientWaitSync) L(DeleteSync)
#undef L
    prog_gun = program(
        "#version 300 es\nlayout(location=0) in vec3 p; layout(location=1) in vec3 n; layout(location=2) in vec3 c;\n"
        "uniform mat4 mvp, model; out vec3 col;\n"
        "void main() { vec3 wn = normalize(mat3(model) * n);\n"
        "  float l = 0.35 + 0.65 * max(dot(wn, normalize(vec3(0.3, 1.0, 0.4))), 0.0);\n"
        "  float rim = pow(1.0 - abs(wn.z), 3.0) * 0.08;\n"
        "  col = c * l + rim; gl_Position = mvp * vec4(p, 1.0); }\n",
        "#version 300 es\nprecision mediump float; in vec3 col; out vec4 o;\n"
        "void main() { o = vec4(col, 1.0); }\n", "p", "n", "c");
    u_gun_mvp = gl.GetUniformLocation(prog_gun, "mvp");
    u_gun_model = gl.GetUniformLocation(prog_gun, "model");
    prog_beam = program(
        "#version 300 es\nlayout(location=0) in vec3 p; layout(location=1) in vec2 uv;\n"
        "uniform mat4 mvp; out vec2 t; void main() { t = uv; gl_Position = mvp * vec4(p, 1.0); }\n",
        "#version 300 es\nprecision mediump float; in vec2 t; uniform vec4 color; uniform vec2 shape; out vec4 o;\n"
        "void main() { float d = length(t);\n"
        "  float a = shape.y > 0.0 ? clamp(1.0 - abs(d - shape.x) / shape.y, 0.0, 1.0)  /* a ring */\n"
        "                          : clamp(1.0 - d, 0.0, 1.0);\n"
        "  o = vec4(color.rgb, color.a * a * a); }\n",
        "p", "uv", NULL);
    u_beam_mvp = gl.GetUniformLocation(prog_beam, "mvp");
    u_beam_color = gl.GetUniformLocation(prog_beam, "color");
    u_beam_shape = gl.GetUniformLocation(prog_beam, "shape");

    for (int a = 0; a < 2; a++) {
        prog_screen[a] = program(
            "#version 300 es\nlayout(location=0) in vec2 p; uniform mat4 mvp; out vec2 t;\n"
            "void main() { t = p * 0.5 + 0.5; gl_Position = mvp * vec4(p, 0.0, 1.0); }\n",
            a ? "#version 300 es\nprecision highp float; precision highp sampler2DArray; in vec2 t; uniform sampler2DArray tex; out vec4 o;\n"
                "void main() { o = vec4(texture(tex, vec3(t, 0.0)).rgb, 1.0); }\n"
              : "#version 300 es\nprecision highp float; in vec2 t; uniform sampler2D tex; out vec4 o;\n"
                "void main() { o = vec4(texture(tex, t).rgb, 1.0); }\n",
            "p", NULL, NULL);
        u_screen_mvp[a] = gl.GetUniformLocation(prog_screen[a], "mvp");
        u_screen_tex[a] = gl.GetUniformLocation(prog_screen[a], "tex");
    }
    prog_panel = program(   /* the panel's picture: row 0 at the top */
        "#version 300 es\nlayout(location=0) in vec2 p; uniform mat4 mvp; out vec2 t;\n"
        "void main() { t = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); gl_Position = mvp * vec4(p, 0.0, 1.0); }\n",
        "#version 300 es\nprecision mediump float; in vec2 t; uniform sampler2D tex; out vec4 o;\n"
        "void main() { o = texture(tex, t); }\n", "p", NULL, NULL);
    u_panel_mvp = gl.GetUniformLocation(prog_panel, "mvp");
    u_panel_tex = gl.GetUniformLocation(prog_panel, "tex");
    static const float quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    gl.GenVertexArrays(1, &vao_screen);
    gl.GenBuffers(1, &vbo_screen);
    gl.BindVertexArray(vao_screen);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_screen);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (const void *)0);
    gl.EnableVertexAttribArray(0);
    gl.GenSamplers(1, &smp_screen);
    gl.SamplerParameteri(smp_screen, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.SamplerParameteri(smp_screen, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.SamplerParameteri(smp_screen, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.SamplerParameteri(smp_screen, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    build_gun(XO_SIGHT_NONE, XO_GUN_SMG);
    gl.GenVertexArrays(1, &vao_gun);
    gl.GenBuffers(1, &vbo_gun);
    gl.BindVertexArray(vao_gun);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_gun);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof mesh, mesh, GL_DYNAMIC_DRAW);
    for (int a = 0; a < 3; a++) {
        gl.VertexAttribPointer((GLuint)a, 3, GL_FLOAT, GL_FALSE, 36, (const void *)(uintptr_t)(a * 12));
        gl.EnableVertexAttribArray((GLuint)a);
    }
    gl.GenVertexArrays(1, &vao_beam);
    gl.GenBuffers(1, &vbo_beam);
    gl.BindVertexArray(vao_beam);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_beam);
    gl.BufferData(GL_ARRAY_BUFFER, 4 * 6 * 5 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (const void *)0);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (const void *)12);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.BindVertexArray(0);
    fprintf(stderr, "[overlay] gun model: %d triangles\n", gun_verts / 3);
    return 1;
}

/* ---- maths (column-major) ---- */
static void mul(float *o, const float *a, const float *b)
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, 64);
}

/* rigid transform from a pose: rotation q, then translation p */
static void pose_matrix(const float q[4], const float p[3], float *m)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);     m[3] = 0;
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);     m[7] = 0;
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y); m[11] = 0;
    m[12] = p[0]; m[13] = p[1]; m[14] = p[2]; m[15] = 1;
}

/* ---- per eye ---- */
typedef struct {
    GLint prog, vao, vbo, vp[4], blend_src[2], blend_dst[2], depth_func, front, cull_face;
    GLboolean blend, depth, cull, depth_mask, color_mask[4];
} Saved;

static void save(Saved *s)
{
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &s->prog);
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->vbo);
    gl.GetIntegerv(GL_VIEWPORT, s->vp);
    gl.GetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src[0]); gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src[1]);
    gl.GetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst[0]); gl.GetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst[1]);
    gl.GetIntegerv(GL_DEPTH_FUNC, &s->depth_func);
    gl.GetIntegerv(GL_FRONT_FACE, &s->front);
    gl.GetIntegerv(GL_CULL_FACE_MODE, &s->cull_face);
    s->blend = gl.IsEnabled(GL_BLEND); s->depth = gl.IsEnabled(GL_DEPTH_TEST); s->cull = gl.IsEnabled(GL_CULL_FACE);
    gl.GetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    gl.GetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
}

static void restore(const Saved *s)
{
    gl.UseProgram((GLuint)s->prog);
    gl.BindVertexArray((GLuint)s->vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)s->vbo);
    gl.Viewport(s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    gl.BlendFuncSeparate((GLenum)s->blend_src[0], (GLenum)s->blend_dst[0], (GLenum)s->blend_src[1], (GLenum)s->blend_dst[1]);
    gl.DepthFunc((GLenum)s->depth_func);
    gl.FrontFace((GLenum)s->front);
    gl.CullFace((GLenum)s->cull_face);
    if (s->blend) gl.Enable(GL_BLEND); else gl.Disable(GL_BLEND);
    if (s->depth) gl.Enable(GL_DEPTH_TEST); else gl.Disable(GL_DEPTH_TEST);
    if (s->cull) gl.Enable(GL_CULL_FACE); else gl.Disable(GL_CULL_FACE);
    gl.DepthMask(s->depth_mask);
    gl.ColorMask(s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3]);
}

/* A strip from a to b facing the eye, half-width w; uv.x runs across it. */
static void beam(const float *vp, const float eye[3], const float a[3], const float b[3], float w,
                 float r, float g, float bl, float alpha)
{
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e[3] = { eye[0] - a[0], eye[1] - a[1], eye[2] - a[2] };
    float s[3] = { d[1] * e[2] - d[2] * e[1], d[2] * e[0] - d[0] * e[2], d[0] * e[1] - d[1] * e[0] };
    float l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (l < 1e-6f) return;
    for (int i = 0; i < 3; i++) s[i] *= w / l;
    float q[6][5] = {
        { a[0] - s[0], a[1] - s[1], a[2] - s[2], -1, 0 }, { a[0] + s[0], a[1] + s[1], a[2] + s[2], 1, 0 },
        { b[0] + s[0], b[1] + s[1], b[2] + s[2], 1, 0 }, { a[0] - s[0], a[1] - s[1], a[2] - s[2], -1, 0 },
        { b[0] + s[0], b[1] + s[1], b[2] + s[2], 1, 0 }, { b[0] - s[0], b[1] - s[1], b[2] - s[2], -1, 0 },
    };
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_beam);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_DYNAMIC_DRAW);
    gl.UniformMatrix4fv(u_beam_mvp, 1, GL_FALSE, vp);
    gl.Uniform4f(u_beam_color, r, g, bl, alpha);
    gl.Uniform2f(u_beam_shape, 0, 0);
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
}

/* A round spot at p facing the eye, radius rad. */
/* A round spot at p facing the eye, radius rad; with thick > 0 a ring at
 * ring (both in units of rad). */
static void spot_shape(const float *vp, const float eye[3], const float p[3], float rad, float r, float g, float b, float alpha,
                       float ring, float thick);
static void spot(const float *vp, const float eye[3], const float p[3], float rad, float r, float g, float b, float alpha)
{
    spot_shape(vp, eye, p, rad, r, g, b, alpha, 0, 0);
}
static void spot_shape(const float *vp, const float eye[3], const float p[3], float rad, float r, float g, float b, float alpha,
                       float ring, float thick)
{
    float f[3] = { eye[0] - p[0], eye[1] - p[1], eye[2] - p[2] };
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (l < 1e-6f) return;
    for (int i = 0; i < 3; i++) f[i] /= l;
    float up[3] = { 0, 1, 0 }, x[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
    float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (xl < 1e-6f) { x[0] = 1; x[1] = x[2] = 0; xl = 1; }
    for (int i = 0; i < 3; i++) x[i] *= rad / xl;
    float y[3] = { f[1] * x[2] - f[2] * x[1], f[2] * x[0] - f[0] * x[2], f[0] * x[1] - f[1] * x[0] };
    float q[6][5];
    static const float c[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < 3; i++) q[k][i] = p[i] + c[k][0] * x[i] + c[k][1] * y[i];
        q[k][3] = c[k][0]; q[k][4] = c[k][1];
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_beam);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_DYNAMIC_DRAW);
    gl.UniformMatrix4fv(u_beam_mvp, 1, GL_FALSE, vp);
    gl.Uniform4f(u_beam_color, r, g, b, alpha);
    gl.Uniform2f(u_beam_shape, ring, thick);
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
}

/* The sight's glass, in the gun's space through model: a quad at depth 1
 * where the reticle may show (depth cleared to 0 first, so nothing else). */
static void glass(const float *vp, const float *model, int sight, float r, float g, float b, float a)
{
    const float *gz = GLASS[sight];
    float q[6][5];
    static const float c[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int k = 0; k < 6; k++) {
        float lp[3] = { c[k][0] * gz[1], rail_y + gz[3] + c[k][1] * gz[2], gz[0] };
        for (int i = 0; i < 3; i++)
            q[k][i] = model[i] * lp[0] + model[4 + i] * lp[1] + model[8 + i] * lp[2] + model[12 + i];
        q[k][3] = 0; q[k][4] = 0;
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_beam);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_DYNAMIC_DRAW);
    gl.UniformMatrix4fv(u_beam_mvp, 1, GL_FALSE, vp);
    gl.Uniform4f(u_beam_color, r, g, b, a);
    gl.Uniform2f(u_beam_shape, 0, 0);
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
}

void xo_draw(const XoFrame *f, const float viewproj[16], const float eye_pos[3], int w, int h)
{
    if (!prog_gun) return;
    Saved s;
    save(&s);
    gl.Viewport(0, 0, w, h);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    float model[16];
    int sight = f->sight;
    if (f->gun_valid && (sight != cur_sight || f->gun_model != cur_model)) {
        build_gun(sight, f->gun_model);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo_gun);
        gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gun_verts * 9 * sizeof(float)), mesh, GL_DYNAMIC_DRAW);
    }
    if (f->gun_valid && f->show_gun) {
        /* the gun: depth-tested against itself only, always in front of the game */
        float mvp[16], kick[16];
        pose_matrix(f->gun_q, f->gun_p, model);
        memset(kick, 0, 64); kick[0] = kick[5] = kick[10] = kick[15] = 1;
        kick[14] = f->recoil;                       /* back along the barrel */
        mul(model, model, kick);
        mul(mvp, viewproj, model);
        gl.ClearDepthf(1);
        gl.DepthMask(GL_TRUE);
        gl.Clear(GL_DEPTH_BUFFER_BIT);
        gl.Enable(GL_DEPTH_TEST);
        gl.DepthFunc(GL_LESS);
        gl.Disable(GL_CULL_FACE);                  /* closed boxes: depth sorts them */
        gl.Disable(GL_BLEND);
        gl.UseProgram(prog_gun);
        gl.UniformMatrix4fv(u_gun_mvp, 1, GL_FALSE, mvp);
        gl.UniformMatrix4fv(u_gun_model, 1, GL_FALSE, model);
        gl.BindVertexArray(vao_gun);
        gl.DrawArrays(GL_TRIANGLES, 0, gun_verts);
    }

    /* The sight's reticle: at the point the gun hits on the screen, seen only
     * through the glass - like a real red dot zeroed at the screen. The glass
     * marks depth 1 where everything else was cleared to 0, and the reticle is
     * drawn where the depth is 1. */
    if (f->gun_valid && f->show_gun && sight != XO_SIGHT_NONE) {
        gl.UseProgram(prog_beam);
        gl.BindVertexArray(vao_beam);
        gl.Disable(GL_CULL_FACE);
        gl.ClearDepthf(0);
        gl.DepthMask(GL_TRUE);
        gl.Clear(GL_DEPTH_BUFFER_BIT);
        gl.Enable(GL_DEPTH_TEST);
        gl.DepthRangef(1, 1);
        gl.DepthFunc(GL_ALWAYS);
        gl.Enable(GL_BLEND);
        gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glass(viewproj, model, sight, 0.55f, 0.75f, 0.85f, 0.10f);     /* a faint tint */
        gl.DepthMask(GL_FALSE);
        gl.DepthFunc(GL_EQUAL);
        float dx = f->end[0] - eye_pos[0], dy = f->end[1] - eye_pos[1], dz = f->end[2] - eye_pos[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (sight == XO_SIGHT_RED_DOT)          /* a 2 MOA dot */
            spot(viewproj, eye_pos, f->end, 0.0012f * dist, 1.0f, 0.08f, 0.05f, 1.0f);
        else {                                  /* a 65 MOA ring round a 1 MOA dot */
            spot_shape(viewproj, eye_pos, f->end, 0.0105f * dist, 1.0f, 0.10f, 0.06f, 0.95f, 0.90f, 0.08f);
            spot(viewproj, eye_pos, f->end, 0.0009f * dist, 1.0f, 0.10f, 0.06f, 1.0f);
        }
        gl.DepthRangef(0, 1);
        gl.ClearDepthf(1);
        gl.DepthMask(GL_TRUE);
        gl.Clear(GL_DEPTH_BUFFER_BIT);
    }

    /* laser and flash over everything; blended, not added, so the red still
     * shows against a white screen */
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.DepthMask(GL_FALSE);
    gl.Enable(GL_BLEND);
    gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    gl.UseProgram(prog_beam);
    gl.BindVertexArray(vao_beam);
    if (f->beam) beam(viewproj, eye_pos, f->muzzle, f->end, 0.0015f, 1.0f, 0.12f, 0.08f, 0.85f);
    if (f->dot) {
        float dx = f->end[0] - eye_pos[0], dy = f->end[1] - eye_pos[1], dz = f->end[2] - eye_pos[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        spot(viewproj, eye_pos, f->end, 0.006f * dist, 1.0f, 0.15f, 0.1f, 1.0f);
    }
    if (f->flash) spot(viewproj, eye_pos, f->muzzle, 0.045f, 1.0f, 0.75f, 0.3f, 0.9f);

    restore(&s);
}

/* The game's own picture, drawn flat as on the arcade, on the screen in front
 * of the player: the quad -1..1 through mvp. The rest of the eye is black. */
void xo_draw_screen(const float mvp[16], unsigned tex, int array_layer0, int w, int h)
{
    if (!prog_screen[0]) return;
    Saved s;
    save(&s);
    GLint unit = 0, bound = 0;
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    gl.ActiveTexture(GL_TEXTURE0);
    GLenum target = array_layer0 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    gl.GetIntegerv(array_layer0 ? GL_TEXTURE_BINDING_2D_ARRAY : GL_TEXTURE_BINDING_2D, &bound);
    GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Viewport(0, 0, w, h);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_BLEND);
    int a = array_layer0 ? 1 : 0;
    gl.UseProgram(prog_screen[a]);
    gl.UniformMatrix4fv(u_screen_mvp[a], 1, GL_FALSE, mvp);
    gl.Uniform1i(u_screen_tex[a], 0);
    gl.BindTexture(target, tex);
    gl.BindSampler(0, smp_screen);
    gl.BindVertexArray(vao_screen);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl.BindSampler(0, 0);
    gl.BindTexture(target, (GLuint)bound);
    gl.ActiveTexture((GLenum)unit);
    if (scissor) gl.Enable(GL_SCISSOR_TEST);
    restore(&s);
}

/* ---- the settings panel ---- */
static GLuint panel_tex;

void xo_panel_upload(const unsigned char *rgba, int w, int h)
{
    if (!prog_screen[0]) return;
    GLint unit = 0, bound = 0;
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (!panel_tex) gl.GenTextures(1, &panel_tex);
    gl.BindTexture(GL_TEXTURE_2D, panel_tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
    gl.ActiveTexture((GLenum)unit);
}

/* The panel on the quad -1..1 placed by mvp, blended over the eye's image. */
void xo_draw_panel(const float mvp[16], int w, int h)
{
    if (!prog_screen[0] || !panel_tex) return;
    Saved s;
    save(&s);
    GLint unit = 0, bound = 0;
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Viewport(0, 0, w, h);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Enable(GL_BLEND);
    gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    gl.UseProgram(prog_panel);
    gl.UniformMatrix4fv(u_panel_mvp, 1, GL_FALSE, mvp);
    gl.Uniform1i(u_panel_tex, 0);
    gl.BindTexture(GL_TEXTURE_2D, panel_tex);
    gl.BindSampler(0, smp_screen);
    gl.BindVertexArray(vao_screen);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl.BindSampler(0, 0);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
    gl.ActiveTexture((GLenum)unit);
    if (scissor) gl.Enable(GL_SCISSOR_TEST);
    restore(&s);
}

#endif /* __ANDROID__ */

/* ---- what the laser points at ----
 * The world's depth at a few pixels along the gun's ray, read back through a
 * ring of pixel buffers two frames later, so the GPU is never waited on. */
#define PROBE_MAX 64
#define PROBE_RING 5
static GLuint prog_probe, probe_fb, probe_tex, probe_pbo[PROBE_RING], probe_smp;
static GLint u_probe_px, u_probe_d;
static struct { int n, pending; GLsync fence; float z[PROBE_MAX], t[PROBE_MAX], zn, zf; } probe_slot[PROBE_RING];
static unsigned probe_k;

static int probe_init(void)
{
    if (prog_probe) return 1;
    prog_probe = program(
        "#version 300 es\nlayout(location=0) in vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }\n",
        "#version 300 es\nprecision highp float; precision highp int; precision highp sampler2DArray;\n"
        "uniform sampler2DArray d; uniform ivec2 px[64]; uniform int selftest; out vec4 o;\n"
        "void main() { int i = int(gl_FragCoord.x);\n"
        "  vec2 sz = vec2(textureSize(d, 0).xy);\n"
        "  uint v = uint(clamp(texture(d, vec3((vec2(px[i]) + 0.5) / sz, 0.0)).r, 0.0, 1.0) * 16777215.0 + 0.5);\n"
        "  if (selftest == 1) v = uint(float(i) / 64.0 * 16777215.0);\n"
        "  if (selftest == 2) v = uint(sz.x);\n"
        "  o = vec4(float(v & 255u), float((v >> 8) & 255u), float((v >> 16) & 255u), 255.0) / 255.0; }\n",
        "p", NULL, NULL);
    u_probe_px = gl.GetUniformLocation(prog_probe, "px");
    u_probe_d = gl.GetUniformLocation(prog_probe, "d");
    GLint was = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &was);
    gl.GenTextures(1, &probe_tex);
    gl.BindTexture(GL_TEXTURE_2D, probe_tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PROBE_MAX, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)was);
    GLint fb = 0;
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    gl.GenFramebuffers(1, &probe_fb);
    gl.BindFramebuffer(GL_FRAMEBUFFER, probe_fb);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, probe_tex, 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
    GLint pb = 0;
    gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pb);
    gl.GenBuffers(PROBE_RING, probe_pbo);
    for (int i = 0; i < PROBE_RING; i++) {
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, probe_pbo[i]);
        gl.BufferData(GL_PIXEL_PACK_BUFFER, PROBE_MAX * 4, NULL, GL_STREAM_READ);
    }
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pb);
    gl.GenSamplers(1, &probe_smp);
    gl.SamplerParameteri(probe_smp, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.SamplerParameteri(probe_smp, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.SamplerParameteri(probe_smp, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    return 1;
}

void xo_copy_depth(unsigned depth_array, int layer, int w, int h)
{
    if (!prog_depth) {
        prog_depth = program(
            "#version 300 es\nlayout(location=0) in vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }\n",
            "#version 300 es\nprecision highp float; precision highp sampler2DArray;\n"
            "uniform sampler2DArray d; uniform float layer; uniform vec2 size; out vec4 o;\n"
            "void main() { gl_FragDepth = texture(d, vec3(gl_FragCoord.xy / size, layer)).r; o = vec4(0.0); }\n",
            "p", NULL, NULL);
        u_depth_d = gl.GetUniformLocation(prog_depth, "d");
        u_depth_layer = gl.GetUniformLocation(prog_depth, "layer");
        u_depth_size = gl.GetUniformLocation(prog_depth, "size");
        gl.GenSamplers(1, &smp_depth);
        gl.SamplerParameteri(smp_depth, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.SamplerParameteri(smp_depth, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.SamplerParameteri(smp_depth, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.SamplerParameteri(smp_depth, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.SamplerParameteri(smp_depth, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    }
    Saved s;
    save(&s);
    GLint unit = 0, bound = 0;
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &bound);
    gl.Disable(GL_BLEND); gl.Disable(GL_CULL_FACE);
    gl.Enable(GL_DEPTH_TEST); gl.DepthFunc(GL_ALWAYS); gl.DepthMask(GL_TRUE);
    gl.ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    gl.Viewport(0, 0, w, h);
    gl.UseProgram(prog_depth);
    gl.Uniform1i(u_depth_d, 0);
    gl.Uniform1f(u_depth_layer, (float)layer);
    gl.Uniform2f(u_depth_size, (float)w, (float)h);
    gl.BindTexture(GL_TEXTURE_2D_ARRAY, depth_array);
    gl.BindSampler(0, smp_depth);
    gl.BindVertexArray(vao_screen);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl.BindSampler(0, 0);
    gl.BindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)bound);
    gl.ActiveTexture((GLenum)unit);
    restore(&s);
}

float xo_probe(unsigned depth_array, const float (*px)[2], const float *z, const float *t, int n, float zn, float zf)
{
    if (!prog_screen[0] || !probe_init()) return -1;
    if (n > PROBE_MAX) n = PROBE_MAX;
    /* the oldest slot's answer, if it is ready: the first sample behind a surface */
    float hit = -2;
    int o = (int)(probe_k % PROBE_RING);
    if (probe_slot[o].pending) {
        /* not drawn yet: no answer, and no new question either - reading it
         * now would wait on the GPU */
        GLenum st = gl.ClientWaitSync(probe_slot[o].fence, 0, 0);
        if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED) return -2;
        GLint pb = 0;
        gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pb);
        gl.DeleteSync(probe_slot[o].fence);
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, probe_pbo[o]);
        const unsigned char *b = gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, PROBE_MAX * 4, GL_MAP_READ_BIT);
        hit = -1;
        static unsigned dbg;
        if (b && ENV_ON("LINDBERGH_AIM_LOG") && ++dbg % 60 == 0)
            for (int i = 0; i < probe_slot[o].n; i += 6)
                fprintf(stderr, "[aim] t %.2f: ray depth %.6f, scene %.6f\n", probe_slot[o].t[i], probe_slot[o].z[i],
                        (float)(b[i * 4] | b[i * 4 + 1] << 8 | b[i * 4 + 2] << 16) / 16777215.0f);
        /* The first sample behind a surface that lies between it and the one
         * before: something much nearer only hides the ray from the eye (the
         * eye and the gun see from different places), the ray passes behind */
        float zn = probe_slot[o].zn, zf = probe_slot[o].zf, prev = 0;
#define LIN(w) (2.0f * zn * zf / (zf + zn - (2.0f * (w) - 1.0f) * (zf - zn)))
        for (int i = 0; b && i < probe_slot[o].n; i++) {
            float dz = (float)(b[i * 4] | b[i * 4 + 1] << 8 | b[i * 4 + 2] << 16) / 16777215.0f;
            float ray = LIN(probe_slot[o].z[i]), scene = LIN(dz);
            if (prev > 0 && dz < 1.0f && scene < ray && scene > prev * 0.9f) { hit = probe_slot[o].t[i] * scene / ray; break; }
            prev = ray;
        }
#undef LIN
        if (b) gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pb);
        probe_slot[o].pending = 0;
    }
    if (n > 0 && depth_array) {
        Saved s;
        save(&s);
        GLint fb = 0, unit = 0, bound = 0, pb = 0;
        gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
        gl.GetIntegerv(GL_ACTIVE_TEXTURE, &unit);
        gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pb);
        GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.GetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &bound);
        GLint ipx[PROBE_MAX * 2];
        for (int i = 0; i < n; i++) { ipx[i * 2] = (GLint)px[i][0]; ipx[i * 2 + 1] = (GLint)px[i][1]; }
        gl.BindFramebuffer(GL_FRAMEBUFFER, probe_fb);
        gl.Disable(GL_SCISSOR_TEST); gl.Disable(GL_DEPTH_TEST); gl.Disable(GL_BLEND); gl.Disable(GL_CULL_FACE);
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.Viewport(0, 0, n, 1);
        gl.UseProgram(prog_probe);
        gl.Uniform2iv(u_probe_px, n, ipx);
        gl.Uniform1i(u_probe_d, 0);
        { static int st = -1; if (st < 0) { const char *v = getenv("LINDBERGH_AIM_SELFTEST"); st = v ? atoi(v) : 0; }
          if (st) gl.Uniform1i(gl.GetUniformLocation(prog_probe, "selftest"), st); }   /* test only: the uniform starts at 0 */
        gl.BindTexture(GL_TEXTURE_2D_ARRAY, depth_array);
        gl.BindSampler(0, probe_smp);
        gl.BindVertexArray(vao_screen);
        gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        int w = (int)(probe_k % PROBE_RING);
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, probe_pbo[w]);
        gl.ReadPixels(0, 0, n, 1, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        probe_slot[w].fence = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        probe_slot[w].n = n;
        probe_slot[w].zn = zn; probe_slot[w].zf = zf;
        memcpy(probe_slot[w].z, z, sizeof(float) * n);
        memcpy(probe_slot[w].t, t, sizeof(float) * n);
        probe_slot[w].pending = 1;
        gl.BindSampler(0, 0);
        gl.BindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)bound);
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pb);
        gl.ActiveTexture((GLenum)unit);
        gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
        if (scissor) gl.Enable(GL_SCISSOR_TEST);
        restore(&s);
    }
    probe_k++;
    return hit;
}
