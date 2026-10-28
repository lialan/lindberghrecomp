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
#undef F
} gl;

static GLuint prog_gun, prog_beam, vao_gun, vbo_gun, vao_beam, vbo_beam;
static GLuint prog_screen[2], vao_screen, vbo_screen, smp_screen;   /* [0] a 2D texture, [1] a layer of an array */
static GLint u_screen_mvp[2], u_screen_tex[2];
static GLint u_gun_mvp, u_gun_model, u_beam_mvp, u_beam_color;
static int gun_verts;

/* ---- the gun: boxes (centre, half size, colour), barrel along -Z ---- */
static const struct { float c[3], h[3], rgb[3]; } PARTS[] = {
    { {  0.000f, -0.026f,  0.030f }, { 0.021f, 0.028f, 0.115f }, { 0.20f, 0.21f, 0.22f } },  /* receiver */
    { {  0.000f,  0.006f,  0.045f }, { 0.012f, 0.005f, 0.075f }, { 0.10f, 0.10f, 0.11f } },  /* top rail */
    { {  0.000f,  0.020f,  0.095f }, { 0.008f, 0.010f, 0.012f }, { 0.08f, 0.08f, 0.09f } },  /* rear sight */
    { {  0.000f, -0.016f, -0.120f }, { 0.017f, 0.019f, 0.050f }, { 0.13f, 0.14f, 0.15f } },  /* handguard */
    { {  0.000f,  0.010f, -0.150f }, { 0.003f, 0.012f, 0.004f }, { 0.08f, 0.08f, 0.09f } },  /* front sight */
    { {  0.000f, -0.006f, -0.205f }, { 0.007f, 0.007f, 0.040f }, { 0.06f, 0.06f, 0.07f } },  /* barrel */
    { {  0.000f, -0.006f, -0.250f }, { 0.010f, 0.010f, 0.010f }, { 0.04f, 0.04f, 0.05f } },  /* muzzle */
    { {  0.000f, -0.100f, -0.030f }, { 0.011f, 0.050f, 0.019f }, { 0.11f, 0.11f, 0.12f } },  /* magazine */
    { {  0.000f, -0.095f,  0.085f }, { 0.014f, 0.045f, 0.020f }, { 0.09f, 0.09f, 0.10f } },  /* pistol grip */
    { {  0.000f, -0.066f,  0.045f }, { 0.004f, 0.012f, 0.022f }, { 0.09f, 0.09f, 0.10f } },  /* trigger guard */
    { {  0.000f, -0.030f,  0.225f }, { 0.012f, 0.032f, 0.080f }, { 0.12f, 0.12f, 0.13f } },  /* stock */
    { {  0.000f, -0.034f,  0.310f }, { 0.016f, 0.045f, 0.008f }, { 0.07f, 0.07f, 0.08f } },  /* butt pad */
};

/* position, normal, colour per vertex */
static float mesh[sizeof PARTS / sizeof PARTS[0] * 36 * 9];

static void build_gun(void)
{
    static const int face[6][4] = { { 1, 3, 7, 5 }, { 0, 4, 6, 2 }, { 2, 6, 7, 3 },
                                    { 0, 1, 5, 4 }, { 4, 5, 7, 6 }, { 0, 2, 3, 1 } };
    static const float nrm[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    int n = 0;
    for (unsigned p = 0; p < sizeof PARTS / sizeof PARTS[0]; p++) {
        float v[8][3];
        for (int k = 0; k < 8; k++)
            for (int a = 0; a < 3; a++)
                v[k][a] = PARTS[p].c[a] + ((k >> a) & 1 ? PARTS[p].h[a] : -PARTS[p].h[a]);
        for (int f = 0; f < 6; f++) {
            static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
            for (int t = 0; t < 6; t++) {
                float *o = &mesh[n++ * 9];
                memcpy(o, v[face[f][tri[t]]], 12);
                memcpy(o + 3, nrm[f], 12);
                memcpy(o + 6, PARTS[p].rgb, 12);
            }
        }
    }
    gun_verts = n;
}

/* ---- set-up ---- */
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
    L(GenSamplers) L(BindSampler) L(SamplerParameteri) L(ClearColor)
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
        "#version 300 es\nprecision mediump float; in vec2 t; uniform vec4 color; out vec4 o;\n"
        "void main() { float d = length(t); float a = clamp(1.0 - d, 0.0, 1.0); o = vec4(color.rgb, color.a * a * a); }\n",
        "p", "uv", NULL);
    u_beam_mvp = gl.GetUniformLocation(prog_beam, "mvp");
    u_beam_color = gl.GetUniformLocation(prog_beam, "color");

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

    build_gun();
    gl.GenVertexArrays(1, &vao_gun);
    gl.GenBuffers(1, &vbo_gun);
    gl.BindVertexArray(vao_gun);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_gun);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gun_verts * 9 * sizeof(float)), mesh, GL_STATIC_DRAW);
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
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
}

/* A round spot at p facing the eye, radius rad. */
static void spot(const float *vp, const float eye[3], const float p[3], float rad, float r, float g, float b, float alpha)
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
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
}

void xo_draw(const XoFrame *f, const float viewproj[16], const float eye_pos[3], int w, int h)
{
    if (!prog_gun) return;
    Saved s;
    save(&s);
    gl.Viewport(0, 0, w, h);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (f->gun_valid) {
        /* the gun: depth-tested against itself only, always in front of the game */
        float model[16], mvp[16], kick[16];
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

#endif /* __ANDROID__ */
