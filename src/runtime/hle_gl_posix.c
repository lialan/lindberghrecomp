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
#include "glsl_pp.h"
#include "stereo.h"

int g_gl_trace;
int g_gl_errcheck;

static const char *arb_source(GLenum target, GLuint id) { return arb_get_source(target, id); }

#ifdef LINDBERGH_GL4ES
#include <EGL/egl.h>
/* What the GLES driver itself says about its current program: gl4es draws
 * with programs of its own, and its glValidateProgram reports nothing. */
static void gles_program_report(void)
{
    typedef void (*Iv)(GLenum, GLint *);
    typedef void (*Prog)(GLuint);
    typedef void (*ProgIv)(GLuint, GLenum, GLint *);
    typedef void (*Log)(GLuint, GLsizei, GLsizei *, char *);
    typedef void (*Active)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *);
    typedef GLint (*Loc)(GLuint, const char *);
    typedef void (*UIv)(GLuint, GLint, GLint *);
    Iv get = (Iv)eglGetProcAddress("glGetIntegerv");
    Prog validate = (Prog)eglGetProcAddress("glValidateProgram");
    ProgIv piv = (ProgIv)eglGetProcAddress("glGetProgramiv");
    Log plog = (Log)eglGetProcAddress("glGetProgramInfoLog");
    Active act = (Active)eglGetProcAddress("glGetActiveUniform");
    Loc loc = (Loc)eglGetProcAddress("glGetUniformLocation");
    UIv uiv = (UIv)eglGetProcAddress("glGetUniformiv");
    if (!get || !validate || !piv || !plog || !act || !loc || !uiv) return;
    GLint prog = 0, linked = 0, n = 0;
    get(GL_CURRENT_PROGRAM, &prog);
    if (!prog) { fprintf(stderr, "[gl]   GLES: no current program\n"); return; }
    char log[2048] = "";
    validate((GLuint)prog);
    plog((GLuint)prog, sizeof log, NULL, log);
    piv((GLuint)prog, GL_LINK_STATUS, &linked);
    fprintf(stderr, "[gl]   GLES program %d, linked %d, validation: %s\n", prog, linked, *log ? log : "(no log)");
    piv((GLuint)prog, GL_ACTIVE_UNIFORMS, &n);
    for (GLint i = 0; i < n; i++) {
        char nm[128]; GLint sz; GLenum ty;
        act((GLuint)prog, (GLuint)i, sizeof nm, NULL, &sz, &ty, nm);
        GLint unit = -1;
        if (ty == GL_SAMPLER_2D || ty == GL_SAMPLER_CUBE || ty == 0x8B5F /* 3D */ || ty == 0x8B62 /* 2D shadow */)
            uiv((GLuint)prog, loc((GLuint)prog, nm), &unit);
        fprintf(stderr, "[gl]     uniform %s type %#x%s", nm, ty, unit >= 0 ? "" : "\n");
        if (unit >= 0) fprintf(stderr, " unit %d\n", unit);
    }
}
#endif

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
#ifdef LINDBERGH_GL4ES
    if (e == GL_INVALID_OPERATION) gles_program_report();
#endif
}

/* LINDBERGH_GL_TRACE=1 names every GL call; =N (N > 1) only those of frame N */
void gl_trace_call(const char *name)
{
    extern unsigned g_frame;
    if (g_gl_trace > 1 && g_frame != (unsigned)g_gl_trace) return;
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

/* glMultiDrawElements: `indices` is an array of pointers - 32-bit guest
 * values here, 64-bit host pointers (or buffer offsets) to the driver. */
static void h_glMultiDrawElements(CPU *c)
{
    GLenum mode = A32(0), type = A32(2);
    const GLsizei *count = (const GLsizei *)APTR(1);
    uint32_t idx = A32(3);
    GLsizei n = AI32(4);
    if (n <= 0 || !count || !idx) return;
    const void *host[256];
    arb_before_draw();
    for (GLsizei base = 0; base < n; base += 256) {
        GLsizei k = n - base < 256 ? n - base : 256;
        for (GLsizei i = 0; i < k; i++)
            host[i] = gl_ptr_or_offset(GL_ELEMENT_ARRAY_BUFFER, rd32(idx + 4u * (uint32_t)(base + i)));
        glMultiDrawElements(mode, count + base, type, host, k);
    }
}

/* glMapBuffer returns driver memory, which has no guest address. The guest
 * gets a shadow in its own window instead: filled from the buffer, written
 * back on unmap when it was written. */
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
        /* Even for GL_WRITE_ONLY: NVIDIA's driver hands back memory that still
         * holds the buffer, and the game counts on it - its smoke rewrites
         * position, colour and matrix index each frame and leaves the texture
         * coordinates written at set-up where they are. A fresh shadow
         * zeroed them, and every smoke quad sampled one texel. */
        glGetBufferSubData(target, 0, size, s);
        g_maps[i].target = target; g_maps[i].buffer = *slot;
        g_maps[i].shadow = s; g_maps[i].size = size; g_maps[i].access = access;
        RET(s);
        return;
    }
    fprintf(stderr, "[gl] glMapBuffer: no shadow slot for target %#x\n", target);
    RET(0);
}

unsigned gl_bound_buffer(unsigned target)
{
    GLuint *slot = binding_slot(target);
    return slot ? *slot : 0;
}

int gl_map_shadow(unsigned target, void **shadow, int *size)
{
    GLuint *slot = binding_slot(target);
    for (int i = 0; i < 4; i++)
        if (g_maps[i].shadow && g_maps[i].target == target && slot && g_maps[i].buffer == *slot &&
            g_maps[i].access != GL_READ_ONLY) {
            *shadow = g_maps[i].shadow; *size = g_maps[i].size; return 1;
        }
    return 0;
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
    {   /* what the game decides its features from, once */
        static int said;
        if (A32(0) == GL_EXTENSIONS && !said++) fprintf(stderr, "[gl] extensions: %s\n", s ? s : "(none)");
    }
    RET(guest_strdup_static(s ? s : ""));
}

/* ---- ARB shader objects -> core GL 2.0 ----
 * One handle space covers programs and shaders in ARB_shader_objects; core
 * splits them, and glIsProgram tells which one a handle is. */
static void h_glCreateProgramObjectARB(CPU *c) { RET(glCreateProgram()); }
/* generic attribute arrays: arbprog reads the classic attribute NVIDIA
 * aliased onto an index while its generic array is off */
static void h_glEnableVertexAttribArrayARB(CPU *c)  { glEnableVertexAttribArray(A32(0)); arb_attrib_array(A32(0), 1); }
static void h_glDisableVertexAttribArrayARB(CPU *c) { glDisableVertexAttribArray(A32(0)); arb_attrib_array(A32(0), 0); }
/* glGetHandleARB(GL_PROGRAM_OBJECT_ARB): the program in use. Let's Go Jungle
 * looks it up to decide ARB_shader_objects is there at all. */
static void h_glGetHandleARB(CPU *c)
{
    GLint p = 0;
    if (A32(0) == 0x8B40) glGetIntegerv(GL_CURRENT_PROGRAM, &p);
    RET((uint32_t)p);
}
/* ARB_color_buffer_float: clamping is what the game asks for anyway, and
 * what a GL without the call does */
static void h_glClampColorARB(CPU *c)
{
#if defined(__APPLE__)
    glClampColorARB(A32(0), A32(1));
#else
    (void)c;
#endif
}
static void h_glCreateShaderObjectARB(CPU *c)  { RET(glCreateShader(A32(0))); }
static void h_glAttachObjectARB(CPU *c)        { glAttachShader(A32(0), A32(1)); }
static void h_glUseProgramObjectARB(CPU *c)    { arb_use_glsl(A32(0)); glUseProgram(A32(0)); }
/* NVIDIA linked a fragment shader that reads a varying its vertex shader
 * never writes, and left the value undefined; Apple refuses the link. The
 * House of the Dead 4's shared fragment shaders read texture coordinates some
 * vertex shaders do not produce. So when that is the only complaint, declare
 * each missing varying in the vertex shader, zero it at the top of main, and
 * link again. Returns 1 if it relinked. */
static int relink_with_missing_varyings(GLuint p, const char *log)
{
    GLuint sh[8]; GLsizei n = 0;
    glGetAttachedShaders(p, 8, &n, sh);
    GLuint vs = 0, fs = 0;
    for (GLsizei i = 0; i < n; i++) {
        GLint type = 0;
        glGetShaderiv(sh[i], GL_SHADER_TYPE, &type);
        if (type == GL_VERTEX_SHADER) vs = sh[i]; else if (type == GL_FRAGMENT_SHADER) fs = sh[i];
    }
    if (!vs || !fs) return 0;
    GLint vlen = 0, flen = 0;
    glGetShaderiv(vs, GL_SHADER_SOURCE_LENGTH, &vlen);
    glGetShaderiv(fs, GL_SHADER_SOURCE_LENGTH, &flen);
    char *vsrc = malloc((size_t)vlen + 1), *fsrc = malloc((size_t)flen + 1);
    glGetShaderSource(vs, vlen + 1, NULL, vsrc);
    glGetShaderSource(fs, flen + 1, NULL, fsrc);

    char decl[4096] = "", init[4096] = "";
    const char *at = log;
    while ((at = strstr(at, "Input of fragment shader '")) != NULL) {
        at += strlen("Input of fragment shader '");
        const char *end = strchr(at, '\'');
        if (!end || end - at > 120) break;
        char name[128];
        snprintf(name, sizeof name, "%.*s", (int)(end - at), at);
        /* the type, from the fragment shader's own declaration */
        char type[32] = "vec4";
        const char *f = fsrc;
        while ((f = strstr(f, name)) != NULL) {
            const char *v = f;
            while (v > fsrc && v[-1] != ';' && v[-1] != '\n' && v[-1] != '}') v--;
            char kw[16], ty[32], nm[128];
            if (sscanf(v, " %15s %31s %127[A-Za-z0-9_]", kw, ty, nm) == 3 &&
                strcmp(kw, "varying") == 0 && strcmp(nm, name) == 0) { snprintf(type, sizeof type, "%s", ty); break; }
            f += strlen(name);
        }
        if (strlen(decl) + 200 < sizeof decl) {
            char line[256];
            if (!strstr(vsrc, name)) {          /* declared already, just not written */
                snprintf(line, sizeof line, "varying %s %s;\n", type, name);
                strcat(decl, line);
            }
            snprintf(line, sizeof line, " %s = %s(0.0);", name, type);
            strcat(init, line);
        }
        at = end;
    }
    int done = 0;
    /* Declared just before main, not at the top: the types are the shader's
     * own (half4), which it #defines further down. */
    char *mainp = strstr(vsrc, "void main");
    char *brace = mainp ? strchr(mainp, '{') : NULL;
    if (*init && brace) {
        size_t out_n = strlen(vsrc) + strlen(decl) + strlen(init) + 8;
        char *out = malloc(out_n);
        size_t a = (size_t)(mainp - vsrc), b = (size_t)(brace + 1 - vsrc);
        snprintf(out, out_n, "%.*s%s%.*s%s%s", (int)a, vsrc, decl, (int)(b - a), mainp, init, brace + 1);
        const GLchar *one = out;
        glShaderSource(vs, 1, &one, NULL);
        glCompileShader(vs);
        {
            GLint cok = 0;
            glGetShaderiv(vs, GL_COMPILE_STATUS, &cok);
            static int shown;
            if (!cok && shown++ < 3) {
                char l[2048] = "";
                glGetShaderInfoLog(vs, sizeof l, NULL, l);
                fprintf(stderr, "[gl] patched vertex shader %u does not compile: %s\n---\n%.1500s\n---\n", vs, l, out);
            }
        }
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        done = ok;
        free(out);
    }
    free(vsrc); free(fsrc);
    return done;
}

/* A shader that fails to build draws nothing, silently. Say so. */
static void h_glLinkProgramARB(CPU *c)
{
    GLuint p = A32(0);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = "";
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        if (strstr(log, "not written by vertex shader") && relink_with_missing_varyings(p, log))
            ok = 1;
    }
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
    {   /* LINDBERGH_SHADER_DUMP_ALL=<dir>: every shader's source, as compiled */
        const char *dir = getenv("LINDBERGH_SHADER_DUMP_ALL");
        GLint len = 0;
        if (dir && *dir) glGetShaderiv(sh, GL_SHADER_SOURCE_LENGTH, &len);
        char *all = len > 0 ? malloc((size_t)len + 1) : NULL;
        if (all) {
            char path[1024];
            GLint type = 0;
            glGetShaderiv(sh, GL_SHADER_TYPE, &type);
            glGetShaderSource(sh, len + 1, NULL, all);
            snprintf(path, sizeof path, "%s/%s_%u.glsl", dir, type == GL_VERTEX_SHADER ? "vs" : "fs", sh);
            FILE *f = fopen(path, "w");
            if (f) { fputs(all, f); fclose(f); }
            free(all);
        }
    }
    if (!ok) {
        char log[2048] = "", src[400] = "";
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        glGetShaderSource(sh, sizeof src, NULL, src);
        fprintf(stderr, "[gl] shader %u failed to compile: %s\n  source starts: %.200s\n", sh, log, src);
        /* LINDBERGH_SHADER_DUMP=<dir>: the whole source, as shader_<n>.glsl */
        const char *dir = getenv("LINDBERGH_SHADER_DUMP");
        GLint len = 0;
        glGetShaderiv(sh, GL_SHADER_SOURCE_LENGTH, &len);
        char *all = dir && *dir && len > 0 ? malloc((size_t)len + 1) : NULL;
        if (all) {
            char path[1024];
            glGetShaderSource(sh, len + 1, NULL, all);
            snprintf(path, sizeof path, "%s/shader_%u.glsl", dir, sh);
            FILE *f = fopen(path, "w");
            if (f) { fputs(all, f); fprintf(f, "\n/* %s */\n", log); fclose(f); }
            free(all);
        }
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
    /* Two things NVIDIA's 2005 compiler let through and Apple's does not:
     * bytes past ASCII (Sega's comments are Shift-JIS), and GLSL 1.20's
     * implicit int-to-float conversions in a shader with no #version. So each
     * string is copied with those bytes blanked, and one that declares no
     * version is compiled as 1.20. */
    const GLchar *host[257];
    GLint hl[257];
    char *copy[256];
    int has_version = 0;
    for (GLsizei i = 0; i < n; i++) {
        const char *src = (const char *)GP(rd32(strs + 4u * (uint32_t)i));
        size_t len = lens && lens[i] >= 0 ? (size_t)lens[i] : (src ? strlen(src) : 0);
        copy[i] = (char *)malloc(len + 1);
        for (size_t k = 0; k < len; k++) {
            unsigned char ch = (unsigned char)src[k];
            copy[i][k] = ch >= 0x80 ? ' ' : (char)ch;
        }
        copy[i][len] = 0;
        if (strstr(copy[i], "#version")) has_version = 1;
        host[i + 1] = copy[i];
        hl[i + 1] = (GLint)len;
    }
    host[0] = has_version ? "" : "#version 120\n";
    hl[0] = (GLint)strlen(host[0]);
#ifdef LINDBERGH_GL4ES
    /* GLSL ES rejects an #if on a name that is not a macro, which desktop
     * GLSL reads as 0: the conditionals are decided here (glsl_pp.c) */
    {
        size_t total = 1;
        for (GLsizei i = 0; i <= n; i++) total += (size_t)hl[i];
        char *all = malloc(total), *w = all;
        for (GLsizei i = 0; i <= n; i++) { memcpy(w, host[i], (size_t)hl[i]); w += hl[i]; }
        *w = 0;
        if (glsl_has_conditionals(all)) {
            char *done = glsl_resolve_conditionals(all);
            free(all);
            all = done;
        }
        const GLchar *one = all;
        glShaderSource(sh, 1, &one, NULL);
        free(all);
    }
#else
    glShaderSource(sh, n + 1, host, hl);
#endif
    for (GLsizei i = 0; i < n; i++) free(copy[i]);
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
/* NV_vertex_program's parameter calls set the same registers as ARB's env
 * parameters (the target is the same number): Let's Go Jungle uploads its
 * matrices and lights this way, many registers at once. */
static void nv_params(GLenum target, GLuint index, GLuint n, const float *v)
{
    for (GLuint i = 0; v && i < n; i++) {
        arb_env(target, index + i, v + 4 * i);
#ifndef LINDBERGH_GL4ES
        glProgramEnvParameter4fvARB(target, index + i, v + 4 * i);
#endif
    }
}
/* Every other spelling of the env and local parameter calls (no ARB suffix,
 * f or d, one or many registers) reaches the same state; a game that fetched
 * one by name went straight to the driver before, which the translated
 * programs never see. Let's Go Jungle set its matrices' last column so. */
static double arg_d(CPU *c, int i)
{
    uint64_t b = (uint64_t)A32(i) | (uint64_t)A32(i + 1) << 32; double d; memcpy(&d, &b, 8); return d;
}
static void params_set(GLenum target, GLuint index, GLuint n, const float *v, int local)
{
    for (GLuint i = 0; v && i < n; i++) {
        if (local) arb_local(target, index + i, v + 4 * i); else arb_env(target, index + i, v + 4 * i);
#ifndef LINDBERGH_GL4ES
        if (local) glProgramLocalParameter4fvARB(target, index + i, v + 4 * i);
        else glProgramEnvParameter4fvARB(target, index + i, v + 4 * i);
#endif
    }
}
static void p4f(CPU *c, int local)
{
    float v[4] = { gl_arg_f(c, 2), gl_arg_f(c, 3), gl_arg_f(c, 4), gl_arg_f(c, 5) };
    params_set(A32(0), A32(1), 1, v, local);
}
static void p4d(CPU *c, int local)
{
    float v[4] = { (float)arg_d(c, 2), (float)arg_d(c, 4), (float)arg_d(c, 6), (float)arg_d(c, 8) };
    params_set(A32(0), A32(1), 1, v, local);
}
static void p4dv(CPU *c, int local)
{
    const double *d = (const double *)APTR(2);
    if (!d) return;
    float v[4] = { (float)d[0], (float)d[1], (float)d[2], (float)d[3] };
    params_set(A32(0), A32(1), 1, v, local);
}
static void h_env4f(CPU *c)   { p4f(c, 0); }
static void h_env4fv(CPU *c)  { params_set(A32(0), A32(1), 1, (const float *)APTR(2), 0); }
static void h_env4d(CPU *c)   { p4d(c, 0); }
static void h_env4dv(CPU *c)  { p4dv(c, 0); }
static void h_envs4fv(CPU *c) { params_set(A32(0), A32(1), A32(2), (const float *)APTR(3), 0); }
static void h_loc4f(CPU *c)   { p4f(c, 1); }
static void h_loc4fv(CPU *c)  { params_set(A32(0), A32(1), 1, (const float *)APTR(2), 1); }
static void h_loc4d(CPU *c)   { p4d(c, 1); }
static void h_loc4dv(CPU *c)  { p4dv(c, 1); }
static void h_locs4fv(CPU *c) { params_set(A32(0), A32(1), A32(2), (const float *)APTR(3), 1); }

static void h_glProgramParameters4fvNV(CPU *c) { nv_params(A32(0), A32(1), A32(2), (const float *)APTR(3)); }
static void h_glProgramParameter4fvNV(CPU *c)  { nv_params(A32(0), A32(1), 1, (const float *)APTR(2)); }
static void h_glProgramParameter4fNV(CPU *c)
{
    float v[4] = { gl_arg_f(c, 2), gl_arg_f(c, 3), gl_arg_f(c, 4), gl_arg_f(c, 5) };
    nv_params(A32(0), A32(1), 1, v);
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
#ifdef LINDBERGH_GL4ES
#define RECT_SEEN(cap) do { if ((cap) == GL_TEXTURE_RECTANGLE_ARB) arb_rect_enable_seen(); } while (0)
#else
#define RECT_SEEN(cap) do { } while (0)
#endif
static void h_glEnable(CPU *c)  { RECT_SEEN(A32(0)); if (!arb_enable(A32(0), 1)) glEnable(A32(0)); }
static void h_glDisable(CPU *c) { RECT_SEEN(A32(0)); if (!arb_enable(A32(0), 0)) glDisable(A32(0)); }
static void h_glIsEnabled(CPU *c)
{
    int on;
    if (arb_is_enabled(A32(0), &on)) RET(on); else RET(glIsEnabled(A32(0)));
}
static void h_glPushAttrib(CPU *c) { arb_push_attrib(A32(0)); glPushAttrib(A32(0)); }
static void h_glPopAttrib(CPU *c)  { arb_pop_attrib(); glPopAttrib(); }

#ifdef LINDBERGH_GL4ES
/* GLES has no assembly programs. Every one is translated to GLSL here
 * (arbprog.c), so the driver never sees them: it is told nothing, and the
 * game's checks are answered as a desktop driver that took them would. */
static void es_glProgramStringARB(CPU *c) { arb_set_source(A32(0), (const char *)APTR(3), AI32(2)); }
static void es_glBindProgramARB(CPU *c)  { arb_bind(A32(0), A32(1)); }
static void es_glDeleteProgramsARB(CPU *c)
{
    GLsizei n = AI32(0);
    const GLuint *ids = (const GLuint *)APTR(1);
    for (GLsizei i = 0; ids && i < n; i++) arb_delete(0, ids[i]);
}
static void es_glProgramEnvParameter4fvARB(CPU *c)
{
    const float *v = (const float *)APTR(2);
    if (v) arb_env(A32(0), A32(1), v);
}
static void es_glProgramLocalParameter4fvARB(CPU *c)
{
    const float *v = (const float *)APTR(2);
    if (v) arb_local(A32(0), A32(1), v);
}
static void es_glProgramLocalParameter4fARB(CPU *c)
{
    float v[4] = { gl_arg_f(c, 2), gl_arg_f(c, 3), gl_arg_f(c, 4), gl_arg_f(c, 5) };
    arb_local(A32(0), A32(1), v);
}
static void es_glGetProgramivARB(CPU *c)
{
    GLint *out = (GLint *)APTR(2);
    if (!out) return;
    switch (A32(1)) {
    case GL_PROGRAM_BINDING_ARB: *out = (GLint)arb_bound(A32(0)); return;
    case GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB: *out = 1; return;
    case GL_PROGRAM_LENGTH_ARB: {
        const char *src = arb_get_source(A32(0), arb_bound(A32(0)));
        *out = src ? (GLint)strlen(src) : 0;
        return;
    }
    }
    glGetProgramivARB(A32(0), A32(1), out);
}
static void es_glGetIntegerv(CPU *c)
{
    GLint *out = (GLint *)APTR(1);
    if (!out) return;
    if (A32(0) == GL_PROGRAM_ERROR_POSITION_ARB) { *out = -1; return; }
    glGetIntegerv(A32(0), out);
}
#endif

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
    g_gl_trace = t && *t ? atoi(t) : 0;
    t = getenv("LINDBERGH_GLERR");
    g_gl_errcheck = t && *t && *t != '0';

    gl_register_generated();
    hle_bind("glBindBufferARB", h_glBindBufferARB); gl_bind_named("glBindBufferARB", h_glBindBufferARB);
    /* the core names reach the same bookkeeping, or a buffer bound through
     * glBindBuffer leaves every pointer argument after it misread */
    gl_bind_named("glBindBuffer", h_glBindBufferARB);
    gl_bind_named("glMapBuffer", h_glMapBufferARB);
    gl_bind_named("glUnmapBuffer", h_glUnmapBufferARB);
    hle_bind("glMultiDrawElements", h_glMultiDrawElements);
    gl_bind_named("glMultiDrawElements", h_glMultiDrawElements);
    gl_bind_named("glMultiDrawElementsEXT", h_glMultiDrawElements);
    hle_bind("glMapBufferARB", h_glMapBufferARB); gl_bind_named("glMapBufferARB", h_glMapBufferARB);
    hle_bind("glUnmapBufferARB", h_glUnmapBufferARB); gl_bind_named("glUnmapBufferARB", h_glUnmapBufferARB);
    hle_bind("glGetString", h_glGetString); gl_bind_named("glGetString", h_glGetString);
    hle_bind("glCreateProgramObjectARB", h_glCreateProgramObjectARB); gl_bind_named("glCreateProgramObjectARB", h_glCreateProgramObjectARB);
    {
        static const char *const en[] = { "glEnableVertexAttribArrayARB", "glEnableVertexAttribArray" },
                                 *const dis[] = { "glDisableVertexAttribArrayARB", "glDisableVertexAttribArray" };
        for (int i = 0; i < 2; i++) {
            hle_bind(en[i], h_glEnableVertexAttribArrayARB); gl_bind_named(en[i], h_glEnableVertexAttribArrayARB);
            hle_bind(dis[i], h_glDisableVertexAttribArrayARB); gl_bind_named(dis[i], h_glDisableVertexAttribArrayARB);
        }
    }
    {
        static const struct { const char *n; HleHandler f; } pp[] = {
            { "glProgramEnvParameter4f", h_env4f }, { "glProgramEnvParameter4fARB", h_env4f },
            { "glProgramEnvParameter4fv", h_env4fv },
            { "glProgramEnvParameter4d", h_env4d }, { "glProgramEnvParameter4dARB", h_env4d },
            { "glProgramEnvParameter4dv", h_env4dv }, { "glProgramEnvParameter4dvARB", h_env4dv },
            { "glProgramEnvParameters4fvEXT", h_envs4fv }, { "glProgramEnvParameters4fvNV", h_envs4fv },
            { "glProgramLocalParameter4f", h_loc4f },
            { "glProgramLocalParameter4fv", h_loc4fv },
            { "glProgramLocalParameter4d", h_loc4d }, { "glProgramLocalParameter4dARB", h_loc4d },
            { "glProgramLocalParameter4dv", h_loc4dv }, { "glProgramLocalParameter4dvARB", h_loc4dv },
            { "glProgramLocalParameters4fvEXT", h_locs4fv }, { "glProgramLocalParameters4fvNV", h_locs4fv },
        };
        for (unsigned i = 0; i < sizeof pp / sizeof *pp; i++) { hle_bind(pp[i].n, pp[i].f); gl_bind_named(pp[i].n, pp[i].f); }
    }
    hle_bind("glProgramParameters4fvNV", h_glProgramParameters4fvNV); gl_bind_named("glProgramParameters4fvNV", h_glProgramParameters4fvNV);
    hle_bind("glProgramParameter4fvNV", h_glProgramParameter4fvNV); gl_bind_named("glProgramParameter4fvNV", h_glProgramParameter4fvNV);
    hle_bind("glProgramParameter4fNV", h_glProgramParameter4fNV); gl_bind_named("glProgramParameter4fNV", h_glProgramParameter4fNV);
    hle_bind("glGetHandleARB", h_glGetHandleARB); gl_bind_named("glGetHandleARB", h_glGetHandleARB);
    hle_bind("glClampColorARB", h_glClampColorARB); gl_bind_named("glClampColorARB", h_glClampColorARB);
    hle_bind("glCreateShaderObjectARB", h_glCreateShaderObjectARB); gl_bind_named("glCreateShaderObjectARB", h_glCreateShaderObjectARB);
    hle_bind("glAttachObjectARB", h_glAttachObjectARB); gl_bind_named("glAttachObjectARB", h_glAttachObjectARB);
    hle_bind("glUseProgramObjectARB", h_glUseProgramObjectARB); gl_bind_named("glUseProgramObjectARB", h_glUseProgramObjectARB);
    hle_bind("glLinkProgramARB", h_glLinkProgramARB); gl_bind_named("glLinkProgramARB", h_glLinkProgramARB);
    hle_bind("glCompileShaderARB", h_glCompileShaderARB); gl_bind_named("glCompileShaderARB", h_glCompileShaderARB);
    hle_bind("glDeleteObjectARB", h_glDeleteObjectARB); gl_bind_named("glDeleteObjectARB", h_glDeleteObjectARB);
    hle_bind("glGetUniformLocationARB", h_glGetUniformLocationARB); gl_bind_named("glGetUniformLocationARB", h_glGetUniformLocationARB);
    hle_bind("glGetAttribLocationARB", h_glGetAttribLocationARB); gl_bind_named("glGetAttribLocationARB", h_glGetAttribLocationARB);
    hle_bind("glGetObjectParameterivARB", h_glGetObjectParameterivARB); gl_bind_named("glGetObjectParameterivARB", h_glGetObjectParameterivARB);
    hle_bind("glGetInfoLogARB", h_glGetInfoLogARB); gl_bind_named("glGetInfoLogARB", h_glGetInfoLogARB);
    hle_bind("glShaderSourceARB", h_glShaderSourceARB); gl_bind_named("glShaderSourceARB", h_glShaderSourceARB);
    hle_bind("glProgramStringARB", h_glProgramStringARB); gl_bind_named("glProgramStringARB", h_glProgramStringARB);
    hle_bind("glBindProgramARB", h_glBindProgramARB); gl_bind_named("glBindProgramARB", h_glBindProgramARB);
    hle_bind("glDeleteProgramsARB", h_glDeleteProgramsARB); gl_bind_named("glDeleteProgramsARB", h_glDeleteProgramsARB);
    hle_bind("glProgramEnvParameter4fvARB", h_glProgramEnvParameter4fvARB); gl_bind_named("glProgramEnvParameter4fvARB", h_glProgramEnvParameter4fvARB);
    hle_bind("glProgramLocalParameter4fvARB", h_glProgramLocalParameter4fvARB); gl_bind_named("glProgramLocalParameter4fvARB", h_glProgramLocalParameter4fvARB);
    hle_bind("glProgramLocalParameter4fARB", h_glProgramLocalParameter4fARB); gl_bind_named("glProgramLocalParameter4fARB", h_glProgramLocalParameter4fARB);
    hle_bind("glEnable", h_glEnable); gl_bind_named("glEnable", h_glEnable);
    hle_bind("glDisable", h_glDisable); gl_bind_named("glDisable", h_glDisable);
    hle_bind("glIsEnabled", h_glIsEnabled); gl_bind_named("glIsEnabled", h_glIsEnabled);
    hle_bind("glPushAttrib", h_glPushAttrib); gl_bind_named("glPushAttrib", h_glPushAttrib);
    hle_bind("glPopAttrib", h_glPopAttrib); gl_bind_named("glPopAttrib", h_glPopAttrib);
    hle_bind("glOrtho", h_glOrtho); gl_bind_named("glOrtho", h_glOrtho);
    hle_bind("glFrustum", h_glFrustum); gl_bind_named("glFrustum", h_glFrustum);
    hle_bind("gluOrtho2D", h_gluOrtho2D); gl_bind_named("gluOrtho2D", h_gluOrtho2D);
    hle_bind("gluPerspective", h_gluPerspective); gl_bind_named("gluPerspective", h_gluPerspective);
    hle_bind("gluErrorString", h_gluErrorString); gl_bind_named("gluErrorString", h_gluErrorString);
#ifdef LINDBERGH_GL4ES
    hle_bind("glProgramStringARB", es_glProgramStringARB);
    hle_bind("glBindProgramARB", es_glBindProgramARB);
    hle_bind("glDeleteProgramsARB", es_glDeleteProgramsARB);
    hle_bind("glProgramEnvParameter4fvARB", es_glProgramEnvParameter4fvARB);
    hle_bind("glProgramLocalParameter4fvARB", es_glProgramLocalParameter4fvARB);
    hle_bind("glProgramLocalParameter4fARB", es_glProgramLocalParameter4fARB);
    hle_bind("glGetProgramivARB", es_glGetProgramivARB);
    hle_bind("glGetIntegerv", es_glGetIntegerv);
#endif
    stereo_init();
}

/* glXGetProcAddressARB tokens: a GLX game's indirect GL calls.
 *
 * A GLX game fetches most of its extension entry points by name rather than
 * importing them, and calls through the pointer. Each name it asks for gets a
 * token address in a range nothing else uses; dispatch() hands a call to one
 * back here, and it runs the typed thunk registered under that name - the
 * generated ones for every function the host library exports, and the
 * hand-written ones above, which win. */
#define GL_TOKEN_BASE 0xF0000000u
#define GL_TOKEN_MAX  2048
#define GL_NAMED_MAX  4096
static struct { const char *name; HleHandler fn; } g_named[GL_NAMED_MAX];
static struct { const char *name; HleHandler fn; HleId id; } g_tok[GL_TOKEN_MAX];
static int g_named_n, g_tok_n;

void gl_bind_named(const char *name, HleHandler fn)
{
    for (int i = 0; i < g_named_n; i++)
        if (strcmp(g_named[i].name, name) == 0) { g_named[i].fn = fn; return; }
    if (g_named_n < GL_NAMED_MAX) { g_named[g_named_n].name = name; g_named[g_named_n++].fn = fn; }
}

uint32_t gl_token_for(const char *name)
{
    for (int i = 0; i < g_tok_n; i++)
        if (strcmp(g_tok[i].name, name) == 0) return GL_TOKEN_BASE + (uint32_t)i;
    HleHandler fn = NULL;
    for (int i = 0; i < g_named_n && !fn; i++)
        if (strcmp(g_named[i].name, name) == 0) fn = g_named[i].fn;
    if (!fn || g_tok_n >= GL_TOKEN_MAX) {
        static int shown;
        if (shown++ < 64) fprintf(stderr, "[tok] %s -> NULL\n", name);
        return 0;
    }
    g_tok[g_tok_n].name = strdup(name);
    g_tok[g_tok_n].fn = fn;
    g_tok[g_tok_n].id = strncmp(name, "glX", 3) ? hle_named_id(name) : (HleId)HLE_ID_MAX;
    if (getenv("LINDBERGH_TOK_LOG")) fprintf(stderr, "[tok] %#x = %s\n", GL_TOKEN_BASE + (uint32_t)g_tok_n, name);
    return GL_TOKEN_BASE + (uint32_t)(g_tok_n++);
}

int gl_token_call(CPU *c, uint32_t va)
{
    if (va < GL_TOKEN_BASE || va >= GL_TOKEN_BASE + (uint32_t)g_tok_n) return 0;
    int t = (int)(va - GL_TOKEN_BASE);
    /* through the hook, as an import would be: stereo.c must see a draw
     * made through a fetched glDrawRangeElements like any other */
    if (g_gl_hook && g_tok[t].id < HLE_ID_MAX) g_gl_hook(c, g_tok[t].id, g_tok[t].fn);
    else g_tok[t].fn(c);
    return 1;
}

int gl_token_count(void) { return g_tok_n; }

/* A build with no generated thunks (the toolkit self-check) still links. */
__attribute__((weak)) void gl_register_generated(void) { }

/* ---- LINDBERGH_GL_TRACE=N: the arguments too, for the calls that place
 * things on the screen (matrices, viewport, immediate vertices). Each wraps
 * the handler already bound. */
static float targf(CPU *c, int i) { uint32_t b = A32(i); float f; memcpy(&f, &b, 4); return f; }
static double targd(CPU *c, int i) { uint64_t b = (uint64_t)A32(i) | (uint64_t)A32(i + 1) << 32; double d; memcpy(&d, &b, 8); return d; }
static int trace_now(void) { extern unsigned g_frame; return g_gl_trace > 1 && g_frame == (unsigned)g_gl_trace; }
#define WRAP(nm, body) \
    static HleHandler o_##nm; \
    static void w_##nm(CPU *c) { if (trace_now()) { body; } o_##nm(c); }
WRAP(glMatrixMode, fprintf(stderr, "[gla] glMatrixMode %#x\n", A32(0)))
WRAP(glLoadIdentity, fprintf(stderr, "[gla] glLoadIdentity\n"))
WRAP(glPushMatrix, fprintf(stderr, "[gla] glPushMatrix\n"))
WRAP(glPopMatrix, fprintf(stderr, "[gla] glPopMatrix\n"))
WRAP(glScalef, fprintf(stderr, "[gla] glScalef %g %g %g\n", targf(c,0), targf(c,1), targf(c,2)))
WRAP(glTranslatef, fprintf(stderr, "[gla] glTranslatef %g %g %g\n", targf(c,0), targf(c,1), targf(c,2)))
WRAP(glViewport, fprintf(stderr, "[gla] glViewport %d %d %d %d\n", (int)A32(0), (int)A32(1), (int)A32(2), (int)A32(3)))
WRAP(glScissor, fprintf(stderr, "[gla] glScissor %d %d %d %d\n", (int)A32(0), (int)A32(1), (int)A32(2), (int)A32(3)))
WRAP(glOrtho, fprintf(stderr, "[gla] glOrtho %g %g %g %g %g %g\n", targd(c,0), targd(c,2), targd(c,4), targd(c,6), targd(c,8), targd(c,10)))
WRAP(glVertex3f, fprintf(stderr, "[gla] glVertex3f %g %g %g\n", targf(c,0), targf(c,1), targf(c,2)))
WRAP(glBegin, fprintf(stderr, "[gla] glBegin %#x\n", A32(0)))
WRAP(glBindFramebufferEXT, fprintf(stderr, "[gla] glBindFramebufferEXT %#x %u\n", A32(0), A32(1)))
WRAP(glBindTexture, fprintf(stderr, "[gla] glBindTexture %#x %u\n", A32(0), A32(1)))
WRAP(glClearColor, fprintf(stderr, "[gla] glClearColor %g %g %g %g\n", targf(c,0), targf(c,1), targf(c,2), targf(c,3)))
WRAP(glVertex3fv, { const float *v = (const float *)APTR(0); fprintf(stderr, "[gla] glVertex3fv %g %g %g\n", v[0], v[1], v[2]); })
WRAP(glLoadMatrixf, { const float *m = (const float *)APTR(0);
    fprintf(stderr, "[gla] glLoadMatrixf"); for (int i = 0; i < 16; i++) fprintf(stderr, " %g", m[i]); fprintf(stderr, "\n"); })
WRAP(glMultMatrixf, { const float *m = (const float *)APTR(0);
    fprintf(stderr, "[gla] glMultMatrixf"); for (int i = 0; i < 16; i++) fprintf(stderr, " %g", m[i]); fprintf(stderr, "\n"); })

void gl_arg_trace_install(void)
{
    if (g_gl_trace <= 1) return;
    for (unsigned i = 0; i < HLE_COUNT; i++) {
        const char *n = hle_name((HleId)i);
#define HOOK(nm) if (!strcmp(n, #nm) && g_hle_handlers[i]) { o_##nm = g_hle_handlers[i]; g_hle_handlers[i] = w_##nm; }
        HOOK(glMatrixMode) HOOK(glLoadIdentity) HOOK(glPushMatrix) HOOK(glPopMatrix) HOOK(glScalef)
        HOOK(glTranslatef) HOOK(glViewport) HOOK(glScissor) HOOK(glOrtho) HOOK(glVertex3f) HOOK(glBegin)
        HOOK(glVertex3fv) HOOK(glLoadMatrixf) HOOK(glMultMatrixf)
        HOOK(glBindFramebufferEXT) HOOK(glBindTexture) HOOK(glClearColor)
#undef HOOK
    }
}
