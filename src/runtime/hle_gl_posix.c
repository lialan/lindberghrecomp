/*
 * hle_gl_posix.c - OpenGL on a 64-bit POSIX host: the thunks a generator
 * cannot write.
 *
 * Most GL imports are generated (tools/gen_gl_thunks.py -> the game's
 * recomp_gl_thunks.c). The ones here move pointers or handles ACROSS the
 * boundary rather than just through it: a string GL owns, a buffer mapping, an
 * array of guest string pointers, and the ARB shader-object handles - which
 * macOS declares as void * and GLES does not have at all, so they are routed
 * to the core GL 2.0 calls that take a GLuint.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "gl_posix.h"
#include "arbprog.h"

int g_gl_trace;
int g_gl_errcheck;

static const char *arb_source(GLenum target, GLuint id) { return arb_get_source(target, id); }

void gl_check_error(const char *name)
{
    static struct { const char *name; GLenum err; } seen[64];
    static int nseen, in_begin;
    /* glGetError between glBegin and glEnd is itself an error. */
    if (strcmp(name, "glBegin") == 0) { in_begin = 1; return; }
    if (in_begin) {
        if (strcmp(name, "glEnd") != 0) return;
        in_begin = 0;
    }
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) return;
    for (int i = 0; i < nseen; i++)
        if (seen[i].err == e && strcmp(seen[i].name, name) == 0) return;
    if (nseen < 64) { seen[nseen].name = name; seen[nseen].err = e; nseen++; }
    fprintf(stderr, "[gl] %s raised GL error %#x\n", name, e);
    /* A draw refused with INVALID_OPERATION is usually the current program
     * failing validation - ask the driver why. */
    if (e == GL_INVALID_OPERATION) {
        GLenum tg[2] = { GL_VERTEX_PROGRAM_ARB, GL_FRAGMENT_PROGRAM_ARB };
        for (int i = 0; i < 2; i++) {
            GLint bound = 0, native = -1, len = 0;
            glGetProgramivARB(tg[i], GL_PROGRAM_BINDING_ARB, &bound);
            glGetProgramivARB(tg[i], GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB, &native);
            glGetProgramivARB(tg[i], GL_PROGRAM_LENGTH_ARB, &len);
            fprintf(stderr, "[gl]   %s program: enabled %d, bound %d, length %d, native %d\n",
                    i ? "fragment" : "vertex", glIsEnabled(tg[i]), bound, len, native);
            const char *src = arb_source(tg[i], (GLuint)bound);
            if (src) fprintf(stderr, "----- %s program %d -----\n%s\n-----\n",
                             i ? "fragment" : "vertex", bound, src);
        }
    }
    GLint prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    if (e == GL_INVALID_OPERATION && prog) {
        char log[2048] = "";
        glValidateProgram((GLuint)prog);
        glGetProgramInfoLog((GLuint)prog, sizeof log, NULL, log);
        fprintf(stderr, "[gl]   current program %d validation: %s\n", prog, *log ? log : "(no log)");
        GLint n = 0;
        glGetProgramiv((GLuint)prog, GL_ACTIVE_UNIFORMS, &n);
        for (GLint i = 0; i < n; i++) {
            char nm[128]; GLint sz; GLenum ty;
            glGetActiveUniform((GLuint)prog, (GLuint)i, sizeof nm, NULL, &sz, &ty, nm);
            if (ty == GL_SAMPLER_2D || ty == GL_SAMPLER_CUBE || ty == GL_SAMPLER_2D_SHADOW || ty == GL_SAMPLER_3D) {
                GLint unit = -1;
                glGetUniformiv((GLuint)prog, glGetUniformLocation((GLuint)prog, nm), &unit);
                fprintf(stderr, "[gl]   sampler %s type %#x unit %d\n", nm, ty, unit);
            }
        }
    }
}

void gl_trace_call(const char *name)
{
    fprintf(stderr, "[gl] %s\n", name);
    fflush(stderr);
}

/* ---- buffer bindings ----
 * Tracked here rather than queried: the pointer-or-offset decision is made on
 * every vertex array and draw call. */
static GLuint g_bound_array, g_bound_element, g_bound_pack, g_bound_unpack;

static GLuint *binding_slot(GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER:         return &g_bound_array;
    case GL_ELEMENT_ARRAY_BUFFER: return &g_bound_element;
    case GL_PIXEL_PACK_BUFFER:    return &g_bound_pack;
    case GL_PIXEL_UNPACK_BUFFER:  return &g_bound_unpack;
    }
    return NULL;
}

const void *gl_ptr_or_offset(unsigned target, uint32_t va)
{
    GLuint *slot = binding_slot(target);
    if (slot && *slot) return (const void *)(uintptr_t)va;   /* offset into the buffer */
    return GP(va);
}

static void h_glBindBufferARB(CPU *c)
{
    GLenum target = A32(0);
    GLuint *slot = binding_slot(target);
    if (slot) *slot = A32(1);
    glBindBuffer(target, A32(1));
}

/* glMapBuffer returns driver memory, which has no guest address. The guest
 * gets a shadow in its own window instead: filled from the buffer when it will
 * be read, written back on unmap when it was written. */
static struct { GLenum target; GLuint buffer; void *shadow; GLint size; GLenum access; } g_maps[4];

static void h_glMapBufferARB(CPU *c)
{
    GLenum target = A32(0), access = A32(1);
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    GLuint *slot = binding_slot(target);
    if (size <= 0 || !slot || !*slot) { RET(0); return; }
    for (int i = 0; i < 4; i++) {
        if (g_maps[i].shadow) continue;
        void *s = guest_alloc((size_t)size, 64);
        if (!s) break;
        if (access != GL_WRITE_ONLY)
            glGetBufferSubData(target, 0, size, s);
        g_maps[i].target = target; g_maps[i].buffer = *slot;
        g_maps[i].shadow = s; g_maps[i].size = size; g_maps[i].access = access;
        RET(s);
        return;
    }
    fprintf(stderr, "[gl] glMapBuffer: no shadow slot for target %#x\n", target);
    RET(0);
}

static void h_glUnmapBufferARB(CPU *c)
{
    GLenum target = A32(0);
    GLuint *slot = binding_slot(target);
    for (int i = 0; i < 4; i++) {
        if (!g_maps[i].shadow || g_maps[i].target != target ||
            !slot || g_maps[i].buffer != *slot) continue;
        if (g_maps[i].access != GL_READ_ONLY)
            glBufferSubData(target, 0, g_maps[i].size, g_maps[i].shadow);
        guest_free(g_maps[i].shadow);
        g_maps[i].shadow = NULL;
        RET(1);
        return;
    }
    RET(0);
}

/* ---- strings GL owns ---- */
static void h_glGetString(CPU *c)
{
    const char *s = (const char *)glGetString(A32(0));
    RET(guest_strdup_static(s ? s : ""));
}

/* ---- ARB shader objects -> core GL 2.0 ----
 * One handle space covers programs and shaders in ARB_shader_objects; core
 * splits them, and glIsProgram tells which one a handle is. */
static void h_glCreateProgramObjectARB(CPU *c) { RET(glCreateProgram()); }
static void h_glCreateShaderObjectARB(CPU *c)  { RET(glCreateShader(A32(0))); }
static void h_glAttachObjectARB(CPU *c)        { glAttachShader(A32(0), A32(1)); }
static void h_glUseProgramObjectARB(CPU *c)    { arb_use_glsl(A32(0)); glUseProgram(A32(0)); }
/* A shader that fails to build draws nothing, silently. Say so. */
static void h_glLinkProgramARB(CPU *c)
{
    GLuint p = A32(0);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "[gl] program %u failed to link: %s\n", p, log);
    }
}
static void h_glCompileShaderARB(CPU *c)
{
    GLuint sh = A32(0);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "", src[400] = "";
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        glGetShaderSource(sh, sizeof src, NULL, src);
        fprintf(stderr, "[gl] shader %u failed to compile: %s\n  source starts: %.200s\n", sh, log, src);
    }
}

/* ARB assembly programs report a rejection through an error position. */
static void h_glProgramStringARB(CPU *c)
{
    const char *src = (const char *)APTR(3);
    glProgramStringARB(A32(0), A32(1), AI32(2), src);
    arb_set_source(A32(0), src, AI32(2));
    GLint pos = -1;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos);
    if (pos >= 0) {
        static int shown;
        if (shown++ < 20) {
            const char *why = (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB);
            int from = pos > 60 ? pos - 60 : 0;
            fprintf(stderr, "[gl] %s program rejected at byte %d: %s\n  near: %.120s\n",
                    A32(0) == GL_FRAGMENT_PROGRAM_ARB ? "fragment" : "vertex",
                    (int)pos, why ? why : "", src ? src + from : "");
        }
    }
}
static void h_glDeleteObjectARB(CPU *c)
{
    GLuint h = A32(0);
    if (glIsProgram(h)) glDeleteProgram(h); else glDeleteShader(h);
}
static void h_glGetUniformLocationARB(CPU *c) { RET((uint32_t)glGetUniformLocation(A32(0), ASTR(1))); }
static void h_glGetAttribLocationARB(CPU *c)  { RET((uint32_t)glGetAttribLocation(A32(0), ASTR(1))); }

/* GL_OBJECT_*_ARB pnames share their values with the core ones, except
 * OBJECT_TYPE, which core has no query for. */
#define GL_OBJECT_TYPE_ARB 0x8B4E
#define GL_PROGRAM_OBJECT_ARB 0x8B40
#define GL_SHADER_OBJECT_ARB  0x8B48
static void h_glGetObjectParameterivARB(CPU *c)
{
    GLuint h = A32(0);
    GLenum pname = A32(1);
    GLint *out = (GLint *)APTR(2);
    if (!out) return;
    int prog = glIsProgram(h);
    if (pname == GL_OBJECT_TYPE_ARB) { *out = prog ? GL_PROGRAM_OBJECT_ARB : GL_SHADER_OBJECT_ARB; return; }
    if (prog) glGetProgramiv(h, pname, out); else glGetShaderiv(h, pname, out);
}
static void h_glGetInfoLogARB(CPU *c)
{
    GLuint h = A32(0);
    if (glIsProgram(h)) glGetProgramInfoLog(h, AI32(1), (GLsizei *)APTR(2), ASTR(3));
    else                glGetShaderInfoLog(h, AI32(1), (GLsizei *)APTR(2), ASTR(3));
    {   /* a shader that does not compile is worth seeing, not just counting */
        const char *log = ASTR(3);
        if (log && *log) fprintf(stderr, "[gl] info log %u: %s\n", h, log);
    }
}

/* The source is an array of guest string pointers, each 4 bytes. */
static void h_glShaderSourceARB(CPU *c)
{
    GLuint sh = A32(0);
    GLsizei n = AI32(1);
    uint32_t strs = A32(2);
    const GLint *lens = (const GLint *)APTR(3);
    if (n <= 0 || n > 256 || !strs) return;
    const GLchar *host[256];
    for (GLsizei i = 0; i < n; i++)
        host[i] = (const GLchar *)GP(rd32(strs + 4u * (uint32_t)i));
    glShaderSource(sh, n, host, lens);
}

/* ---- assembly-program state, owned by arbprog.c ---- */
static void h_glBindProgramARB(CPU *c)  { arb_bind(A32(0), A32(1)); glBindProgramARB(A32(0), A32(1)); }
static void h_glDeleteProgramsARB(CPU *c)
{
    GLsizei n = AI32(0);
    const GLuint *ids = (const GLuint *)APTR(1);
    for (GLsizei i = 0; ids && i < n; i++) arb_delete(0, ids[i]);
    glDeleteProgramsARB(n, ids);
}
static void h_glProgramEnvParameter4fvARB(CPU *c)
{
    const float *v = (const float *)APTR(2);
    if (v) { arb_env(A32(0), A32(1), v); glProgramEnvParameter4fvARB(A32(0), A32(1), v); }
}
static void h_glProgramLocalParameter4fvARB(CPU *c)
{
    const float *v = (const float *)APTR(2);
    if (v) { arb_local(A32(0), A32(1), v); glProgramLocalParameter4fvARB(A32(0), A32(1), v); }
}
static void h_glProgramLocalParameter4fARB(CPU *c)
{
    float v[4] = { gl_arg_f(c, 2), gl_arg_f(c, 3), gl_arg_f(c, 4), gl_arg_f(c, 5) };
    arb_local(A32(0), A32(1), v);
    glProgramLocalParameter4fARB(A32(0), A32(1), v[0], v[1], v[2], v[3]);
}
static void h_glEnable(CPU *c)  { if (!arb_enable(A32(0), 1)) glEnable(A32(0)); }
static void h_glDisable(CPU *c) { if (!arb_enable(A32(0), 0)) glDisable(A32(0)); }
static void h_glIsEnabled(CPU *c)
{
    int on;
    if (arb_is_enabled(A32(0), &on)) RET(on); else RET(glIsEnabled(A32(0)));
}
static void h_glPushAttrib(CPU *c) { arb_push_attrib(A32(0)); glPushAttrib(A32(0)); }
static void h_glPopAttrib(CPU *c)  { arb_pop_attrib(); glPopAttrib(); }

/* ---- GLU, and the double-precision matrix calls ---- */
static void h_glOrtho(CPU *c)
{
    glOrtho(gl_arg_d(c, 0), gl_arg_d(c, 2), gl_arg_d(c, 4),
            gl_arg_d(c, 6), gl_arg_d(c, 8), gl_arg_d(c, 10));
}
static void h_glFrustum(CPU *c)
{
    glFrustum(gl_arg_d(c, 0), gl_arg_d(c, 2), gl_arg_d(c, 4),
              gl_arg_d(c, 6), gl_arg_d(c, 8), gl_arg_d(c, 10));
}
static void h_gluOrtho2D(CPU *c)
{
    glOrtho(gl_arg_d(c, 0), gl_arg_d(c, 2), gl_arg_d(c, 4), gl_arg_d(c, 6), -1.0, 1.0);
}
static void h_gluPerspective(CPU *c)
{
    double fovy = gl_arg_d(c, 0), aspect = gl_arg_d(c, 2);
    double zn = gl_arg_d(c, 4), zf = gl_arg_d(c, 6);
    double top = zn * tan(fovy * 3.14159265358979323846 / 360.0);
    double right = top * aspect;
    glFrustum(-right, right, -top, top, zn, zf);
}
static void h_gluErrorString(CPU *c)
{
    static const char *msg[] = { "no error", "invalid enum", "invalid value",
                                 "invalid operation", "stack overflow",
                                 "stack underflow", "out of memory" };
    uint32_t e = A32(0);
    RET(guest_strdup_static((e >= 0x500 && e <= 0x505) ? msg[e - 0x500 + 1] : msg[0]));
}

void hle_register_gl(void)
{
    const char *t = getenv("LINDBERGH_GL_TRACE");
    g_gl_trace = t && *t && *t != '0';
    t = getenv("LINDBERGH_GLERR");
    g_gl_errcheck = t && *t && *t != '0';

    gl_register_generated();
    hle_bind("glBindBufferARB", h_glBindBufferARB);
    hle_bind("glMapBufferARB", h_glMapBufferARB);
    hle_bind("glUnmapBufferARB", h_glUnmapBufferARB);
    hle_bind("glGetString", h_glGetString);
    hle_bind("glCreateProgramObjectARB", h_glCreateProgramObjectARB);
    hle_bind("glCreateShaderObjectARB", h_glCreateShaderObjectARB);
    hle_bind("glAttachObjectARB", h_glAttachObjectARB);
    hle_bind("glUseProgramObjectARB", h_glUseProgramObjectARB);
    hle_bind("glLinkProgramARB", h_glLinkProgramARB);
    hle_bind("glCompileShaderARB", h_glCompileShaderARB);
    hle_bind("glDeleteObjectARB", h_glDeleteObjectARB);
    hle_bind("glGetUniformLocationARB", h_glGetUniformLocationARB);
    hle_bind("glGetAttribLocationARB", h_glGetAttribLocationARB);
    hle_bind("glGetObjectParameterivARB", h_glGetObjectParameterivARB);
    hle_bind("glGetInfoLogARB", h_glGetInfoLogARB);
    hle_bind("glShaderSourceARB", h_glShaderSourceARB);
    hle_bind("glProgramStringARB", h_glProgramStringARB);
    hle_bind("glBindProgramARB", h_glBindProgramARB);
    hle_bind("glDeleteProgramsARB", h_glDeleteProgramsARB);
    hle_bind("glProgramEnvParameter4fvARB", h_glProgramEnvParameter4fvARB);
    hle_bind("glProgramLocalParameter4fvARB", h_glProgramLocalParameter4fvARB);
    hle_bind("glProgramLocalParameter4fARB", h_glProgramLocalParameter4fARB);
    hle_bind("glEnable", h_glEnable);
    hle_bind("glDisable", h_glDisable);
    hle_bind("glIsEnabled", h_glIsEnabled);
    hle_bind("glPushAttrib", h_glPushAttrib);
    hle_bind("glPopAttrib", h_glPopAttrib);
    hle_bind("glOrtho", h_glOrtho);
    hle_bind("glFrustum", h_glFrustum);
    hle_bind("gluOrtho2D", h_gluOrtho2D);
    hle_bind("gluPerspective", h_gluPerspective);
    hle_bind("gluErrorString", h_gluErrorString);
}

/* glXGetProcAddressARB tokens: a GLX game's indirect GL calls. This game is
 * GLUT and never asks; dispatch() still consults the table. */
uint32_t gl_token_for(const char *name) { (void)name; return 0; }
int gl_token_call(CPU *c, uint32_t va) { (void)c; (void)va; return 0; }

/* A build with no generated thunks (the toolkit self-check) still links. */
__attribute__((weak)) void gl_register_generated(void) { }

/* Cg (hle_cg.c) is Windows-only so far; this game does not use it. */
void hle_register_cg(void) { }
