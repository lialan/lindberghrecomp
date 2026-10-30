/*
 * arbprog.c - ARB / NV assembly programs, translated to GLSL.
 *
 * A Lindbergh game draws with ARB_vertex_program and ARB_fragment_program
 * text, and Ghost Squad Evolution's lighting programs add
 * OPTION NV_vertex_program2 - branches, subroutines and condition codes -
 * which only NVIDIA's driver ever ran. Apple's driver accepts that text and
 * then refuses every draw with it; GLES (the Quest) has no assembly programs
 * at all. So the runtime owns the assembly-program state instead of the
 * driver: which program is bound and enabled per stage, the env and local
 * parameters, and the source. Before each draw, the enabled pair is
 * translated to a GLSL program (once, cached), bound, and its parameters and
 * matrices uploaded as uniforms.
 *
 * The translation is direct: every TEMP, ATTRIB, PARAM and OUTPUT becomes a
 * vec4, every instruction one GLSL statement on vec4s with the same swizzle,
 * write-mask, negate and saturate rules. Matrices are computed on the CPU from
 * the GL matrix stacks and uploaded as rows, so nothing depends on GLSL's
 * compatibility built-ins for them. Programs with control flow (NV BRA, CAL,
 * RET) become a loop over basic blocks with a program counter and a call
 * stack, which keeps the original's arbitrary jumps exact.
 *
 * Anything the translator does not understand fails the translation with a
 * reason, once, and that program falls back to the driver's own path.
 */

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#include "gl_posix.h"
#include "arbprog.h"
#ifdef LINDBERGH_GL4ES
extern int gl4es_mv_shaders;      /* gl4es' multiview (multiview.c in quest/gl4es.patch) */
extern int gl4es_mv_trace;
#endif
#include "stereo.h"

#ifndef GL_MATRIX0_ARB
#define GL_MATRIX0_ARB 0x88C0
#endif
#ifndef GL_CURRENT_MATRIX_ARB
#define GL_CURRENT_MATRIX_ARB 0x8641
#endif

#define MAX_IDS   4096
#define MAX_ENV   256
#define MAX_LOCAL 96

/* which generic attribute arrays the game has enabled (u_gen) */
static float g_gen_on[16];

/* ------------------------------------------------------------------------ */
/* program state the driver no longer holds                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    char    *src;
    unsigned gen;                /* bumps on every new source */
    float    local[MAX_LOCAL][4];
    unsigned local_gen;
    int      failed;             /* translation failed: use the driver */
} ArbProg;

static ArbProg *g_prog[2][MAX_IDS];       /* [0] vertex, [1] fragment */
static GLuint   g_bound[2];
static int      g_enabled[2];
static float    g_env[2][MAX_ENV][4];
static unsigned g_env_gen[2];
static GLuint   g_game_glsl;              /* the game's own GLSL program, if any */

static int stage_of(GLenum target)
{
    if (target == GL_VERTEX_PROGRAM_ARB) return 0;
    if (target == GL_FRAGMENT_PROGRAM_ARB) return 1;
    return -1;
}

static ArbProg *prog_get(int st, GLuint id, int create)
{
    if (id == 0 || id >= MAX_IDS) return NULL;
    if (!g_prog[st][id] && create) g_prog[st][id] = (ArbProg *)calloc(1, sizeof(ArbProg));
    return g_prog[st][id];
}

/* ------------------------------------------------------------------------ */
/* the translator                                                            */
/* ------------------------------------------------------------------------ */

typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int need = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (b->p && b->n + (size_t)need < b->cap) { b->n += (size_t)need; return; }
        b->cap = (b->cap + (size_t)need + 1) * 2;
        b->p = (char *)realloc(b->p, b->cap);
        if (!b->n) b->p[0] = 0;
    }
}

enum { SYM_TEMP, SYM_ADDR, SYM_EXPR, SYM_ARRAY, SYM_OUT };

typedef struct {
    char name[64];
    int  kind;
    char expr[160];       /* SYM_EXPR / SYM_OUT: a vec4 lvalue/rvalue; SYM_ARRAY: array base */
    int  base;            /* SYM_ARRAY: offset into the base array */
    int  len;
} Sym;

/* A matrix the program reads: computed on the CPU and uploaded as 4 rows. */
typedef struct { int which, index, mod; } MatRef;   /* mod: 0 plain 1 inverse 2 transpose 3 invtrans */
enum { MAT_MVP, MAT_MODELVIEW, MAT_PROJECTION, MAT_TEXTURE, MAT_PROGRAM };

typedef struct {
    int     stage;
    Sym     sym[256];
    int     nsym;
    MatRef  mat[16];
    int     nmat;
    int     env_hi, local_hi;          /* highest index read, -1 none */
    int     tex_target[16];            /* 0 none, else 1=1D 2=2D 3=3D 4=CUBE 5=RECT */
    int     attrib_used[16];
    int     gen_alias;                 /* reads an attribute NVIDIA aliases (u_gen) */
    int     pos_invariant, fog_mode;   /* fog: 0 none 1 linear 2 exp 3 exp2 */
    int     writes_depth;
    char    err[200];
    Buf     decl, body;
    /* control flow */
    char    label[64][64];
    int     nlabel;
    int     has_flow;
    int     block;                     /* current block number when has_flow */
} Tr;

static int fail(Tr *t, const char *fmt, ...)
{
    if (!t->err[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(t->err, sizeof t->err, fmt, ap);
        va_end(ap);
    }
    return 0;
}

static Sym *sym_find(Tr *t, const char *name)
{
    for (int i = 0; i < t->nsym; i++)
        if (strcmp(t->sym[i].name, name) == 0) return &t->sym[i];
    return NULL;
}

static Sym *sym_add(Tr *t, const char *name, int kind)
{
    if (t->nsym >= 256) { fail(t, "too many symbols"); return NULL; }
    Sym *s = &t->sym[t->nsym++];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->kind = kind;
    return s;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int mat_slot(Tr *t, int which, int index, int mod)
{
    for (int i = 0; i < t->nmat; i++)
        if (t->mat[i].which == which && t->mat[i].index == index && t->mat[i].mod == mod) return i;
    if (t->nmat >= 16) return fail(t, "too many matrices"), 0;
    t->mat[t->nmat].which = which; t->mat[t->nmat].index = index; t->mat[t->nmat].mod = mod;
    return t->nmat++;
}

/* "state.matrix.X[.mod][.row[a..b]]" -> uniform rows. Sets arr to the
 * GLSL array base and lo and hi to the rows. */
static int parse_matrix(Tr *t, const char *s, char *arr, size_t arrn, int *lo, int *hi)
{
    const char *p = s + strlen("state.matrix.");
    int which, index = 0;
    if      (!strncmp(p, "mvp", 3))        { which = MAT_MVP; p += 3; }
    else if (!strncmp(p, "modelview", 9))  { which = MAT_MODELVIEW; p += 9; }
    else if (!strncmp(p, "projection", 10)){ which = MAT_PROJECTION; p += 10; }
    else if (!strncmp(p, "texture", 7))    { which = MAT_TEXTURE; p += 7; }
    else if (!strncmp(p, "program", 7))    { which = MAT_PROGRAM; p += 7; }
    else return fail(t, "matrix %s", s);
    if (*p == '[') { index = atoi(p + 1); p = strchr(p, ']'); if (!p) return fail(t, "matrix %s", s); p++; }
    int mod = 0;
    if      (!strncmp(p, ".invtrans", 9))  { mod = 3; p += 9; }
    else if (!strncmp(p, ".inverse", 8))   { mod = 1; p += 8; }
    else if (!strncmp(p, ".transpose", 10)){ mod = 2; p += 10; }
    *lo = 0; *hi = 3;
    if (!strncmp(p, ".row[", 5)) {
        p += 5;
        *lo = *hi = atoi(p);
        const char *dd = strstr(p, "..");
        const char *rb = strchr(p, ']');
        if (dd && rb && dd < rb) *hi = atoi(dd + 2);
        p = rb ? rb + 1 : p + strlen(p);
    }
    if (*p) return fail(t, "matrix suffix %s", p);
    int slot = mat_slot(t, which, index, mod);
    snprintf(arr, arrn, "%s_m%d", t->stage ? "f" : "v", slot);
    return 1;
}

/* One binding -> one vec4 GLSL rvalue. For array bindings (env ranges and
 * whole matrices) see parse_array_binding. */
static int binding_expr(Tr *t, const char *b, char *out, size_t n)
{
    const char *P = t->stage ? "f" : "v";
    int i;
    char c;
    if (sscanf(b, "program.env[%d%c", &i, &c) == 2 && c == ']') {
        if (i > t->env_hi) t->env_hi = i;
        snprintf(out, n, "%s_env[%d]", P, i); return 1;
    }
    if (sscanf(b, "program.local[%d%c", &i, &c) == 2 && c == ']') {
        if (i > t->local_hi) t->local_hi = i;
        snprintf(out, n, "%s_loc[%d]", P, i); return 1;
    }
    if (!strncmp(b, "state.matrix.", 13)) {
        char arr[32]; int lo, hi;
        if (!parse_matrix(t, b, arr, sizeof arr, &lo, &hi)) return 0;
        if (lo != hi) return fail(t, "matrix used as a single vector: %s", b);
        snprintf(out, n, "%s[%d]", arr, lo); return 1;
    }
    if (!strcmp(b, "state.fog.color")) { snprintf(out, n, "gl_Fog.color"); return 1; }
    if (!strcmp(b, "state.fog.params")) {
        snprintf(out, n, "vec4(gl_Fog.density, gl_Fog.start, gl_Fog.end, gl_Fog.scale)"); return 1;
    }
    if (t->stage == 0) {
        if (!strcmp(b, "vertex.position")) { snprintf(out, n, "gl_Vertex"); return 1; }
        if (!strcmp(b, "vertex.normal"))   { snprintf(out, n, "vec4(gl_Normal, 1.0)"); return 1; }
        if (!strcmp(b, "vertex.color") || !strcmp(b, "vertex.color.primary")) { snprintf(out, n, "gl_Color"); return 1; }
        if (!strcmp(b, "vertex.color.secondary")) { snprintf(out, n, "gl_SecondaryColor"); return 1; }
        if (!strcmp(b, "vertex.fogcoord")) { snprintf(out, n, "vec4(gl_FogCoord, 0.0, 0.0, 1.0)"); return 1; }
        if (!strcmp(b, "vertex.texcoord")) { snprintf(out, n, "gl_MultiTexCoord0"); return 1; }
        if (sscanf(b, "vertex.texcoord[%d%c", &i, &c) == 2 && c == ']' && i < 8) {
            snprintf(out, n, "gl_MultiTexCoord%d", i); return 1;
        }
        if (sscanf(b, "vertex.attrib[%d%c", &i, &c) == 2 && c == ']' && i < 16) {
            if (i == 0) { snprintf(out, n, "gl_Vertex"); return 1; }
            t->attrib_used[i] = 1;
            /* NVIDIA's driver aliases generic attributes onto the classic
             * ones (2 normal, 3 colour, 4 secondary colour, 5 fog, 8-15 the
             * texture coordinates), and Cg's NV profiles read texcoords as
             * vertex.attrib[8]: Let's Go Jungle sets them with glTexCoord and
             * glMultiTexCoord. GLSL keeps the two apart, so the program reads
             * the generic one while the game has its array enabled (u_gen,
             * arb_attrib_array) and the classic one otherwise. */
            static const char *const classic[16] = {
                NULL, NULL, "vec4(gl_Normal, 1.0)", "gl_Color", "gl_SecondaryColor",
                "vec4(gl_FogCoord, 0.0, 0.0, 1.0)", NULL, NULL,
                "gl_MultiTexCoord0", "gl_MultiTexCoord1", "gl_MultiTexCoord2", "gl_MultiTexCoord3",
                "gl_MultiTexCoord4", "gl_MultiTexCoord5", "gl_MultiTexCoord6", "gl_MultiTexCoord7",
            };
            if (classic[i]) {
                t->gen_alias = 1;
                snprintf(out, n, "(u_gen[%d] > 0.5 ? va_%d : %s)", i, i, classic[i]);
                return 1;
            }
            snprintf(out, n, "va_%d", i); return 1;
        }
    } else {
        if (!strcmp(b, "fragment.color") || !strcmp(b, "fragment.color.primary")) { snprintf(out, n, "gl_Color"); return 1; }
        if (!strcmp(b, "fragment.color.secondary")) { snprintf(out, n, "gl_SecondaryColor"); return 1; }
        if (!strcmp(b, "fragment.fogcoord")) { snprintf(out, n, "vec4(gl_FogFragCoord, 0.0, 0.0, 1.0)"); return 1; }
        if (!strcmp(b, "fragment.position")) { snprintf(out, n, "gl_FragCoord"); return 1; }
        if (!strcmp(b, "fragment.texcoord")) { snprintf(out, n, "gl_TexCoord[0]"); return 1; }
        if (sscanf(b, "fragment.texcoord[%d%c", &i, &c) == 2 && c == ']' && i < 8) {
            snprintf(out, n, "gl_TexCoord[%d]", i); return 1;
        }
    }
    /* literals */
    char *end;
    double v = strtod(b, &end);
    if (end != b && !*trim(end)) { snprintf(out, n, "vec4(%.9g)", v); return 1; }
    if (b[0] == '{') {
        double f[4] = { 0, 0, 0, 1 };
        int k = 0;
        const char *q = b + 1;
        while (k < 4) {
            f[k] = strtod(q, &end);
            if (end == q) break;
            k++;
            q = end;
            while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n') q++;
        }
        /* fewer than four: y and z default to 0, w to 1 */
        snprintf(out, n, "vec4(%.9g, %.9g, %.9g, %.9g)", f[0], f[1], f[2], f[3]); return 1;
    }
    return fail(t, "binding %s", b);
}

/* result.* -> an lvalue vec4 (temporaries copied to the built-ins at the end) */
static int output_expr(Tr *t, const char *b, char *out, size_t n)
{
    int i; char c;
    if (t->stage == 0) {
        if (!strcmp(b, "result.position")) { snprintf(out, n, "gl_Position"); return 1; }
        if (!strcmp(b, "result.color") || !strcmp(b, "result.color.primary") ||
            !strcmp(b, "result.color.front") || !strcmp(b, "result.color.front.primary")) {
            snprintf(out, n, "gl_FrontColor"); return 1; }
        if (!strcmp(b, "result.color.secondary") || !strcmp(b, "result.color.front.secondary")) {
            snprintf(out, n, "gl_FrontSecondaryColor"); return 1; }
        if (!strcmp(b, "result.color.back") || !strcmp(b, "result.color.back.primary")) {
            snprintf(out, n, "gl_BackColor"); return 1; }
        if (!strcmp(b, "result.color.back.secondary")) { snprintf(out, n, "gl_BackSecondaryColor"); return 1; }
        if (!strcmp(b, "result.fogcoord")) { snprintf(out, n, "o_fog"); return 1; }
        if (!strcmp(b, "result.pointsize")) { snprintf(out, n, "o_psize"); return 1; }
        if (!strcmp(b, "result.texcoord")) { snprintf(out, n, "gl_TexCoord[0]"); return 1; }
        if (sscanf(b, "result.texcoord[%d%c", &i, &c) == 2 && c == ']' && i < 8) {
            snprintf(out, n, "gl_TexCoord[%d]", i); return 1; }
    } else {
        if (!strcmp(b, "result.color")) { snprintf(out, n, "o_col"); return 1; }
        if (!strcmp(b, "result.depth")) { t->writes_depth = 1; snprintf(out, n, "o_dep"); return 1; }
    }
    return fail(t, "output %s", b);
}

/* The next item of a comma list, splitting only at the top level: an item
 * can be a constant vector, "{ 1.442695, 0.059683103, 1.5 }", whose own
 * commas are not separators. NULL at the end. */
static char *next_item(char **cur)
{
    char *s = *cur;
    if (!s || !*s) return NULL;
    int depth = 0;
    char *q = s;
    for (; *q; q++) {
        if (*q == '{') depth++;
        else if (*q == '}') depth--;
        else if (*q == ',' && depth == 0) break;
    }
    if (*q) { *q = 0; *cur = q + 1; } else *cur = q;
    return s;
}

/* PARAM name[N] = { ... } : alias a contiguous env range or a single matrix,
 * otherwise build a local array. */
static int declare_param_array(Tr *t, const char *name, const char *init)
{
    char tmp[8192];
    snprintf(tmp, sizeof tmp, "%s", init);
    char *s = trim(tmp);
    if (*s == '{') s++;
    char *e = s + strlen(s);
    if (e > s && e[-1] == '}') e[-1] = 0;
    s = trim(s);
    /* the whole list is one item? */
    int lo, hi; char c;
    if (!strchr(s, ',') && sscanf(s, "program.env[%d..%d%c", &lo, &hi, &c) == 3 && c == ']') {
        Sym *y = sym_add(t, name, SYM_ARRAY);
        if (!y) return 0;
        snprintf(y->expr, sizeof y->expr, "%s_env", t->stage ? "f" : "v");
        y->base = lo; y->len = hi - lo + 1;
        if (hi > t->env_hi) t->env_hi = hi;
        return 1;
    }
    if (!strchr(s, ',') && !strncmp(s, "state.matrix.", 13)) {
        char arr[32];
        if (!parse_matrix(t, s, arr, sizeof arr, &lo, &hi)) return 0;
        Sym *y = sym_add(t, name, SYM_ARRAY);
        if (!y) return 0;
        snprintf(y->expr, sizeof y->expr, "%s", arr);
        y->base = lo; y->len = hi - lo + 1;
        return 1;
    }
    /* general list: a local array */
    Sym *y = sym_add(t, name, SYM_ARRAY);
    if (!y) return 0;
    snprintf(y->expr, sizeof y->expr, "pa_%s", name);
    y->base = 0;
    int k = 0;
    Buf init_code = {0};
    char *cur = s;
    for (char *item; (item = next_item(&cur)); ) {
        item = trim(item);
        if (sscanf(item, "program.env[%d..%d%c", &lo, &hi, &c) == 3 && c == ']') {
            for (int j = lo; j <= hi; j++) {
                bput(&init_code, "  pa_%s[%d] = %s_env[%d];\n", name, k++, t->stage ? "f" : "v", j);
                if (j > t->env_hi) t->env_hi = j;
            }
        } else if (sscanf(item, "program.local[%d..%d%c", &lo, &hi, &c) == 3 && c == ']') {
            for (int j = lo; j <= hi; j++) {
                bput(&init_code, "  pa_%s[%d] = %s_loc[%d];\n", name, k++, t->stage ? "f" : "v", j);
                if (j > t->local_hi) t->local_hi = j;
            }
        } else if (!strncmp(item, "state.matrix.", 13)) {
            char arr[32];
            if (!parse_matrix(t, item, arr, sizeof arr, &lo, &hi)) return 0;
            for (int j = lo; j <= hi; j++) bput(&init_code, "  pa_%s[%d] = %s[%d];\n", name, k++, arr, j);
        } else {
            char ex[160];
            if (!binding_expr(t, item, ex, sizeof ex)) return 0;
            bput(&init_code, "  pa_%s[%d] = %s;\n", name, k++, ex);
        }
    }
    y->len = k;
    bput(&t->decl, "vec4 pa_%s[%d];\n", name, k ? k : 1);
    /* initialised at the top of main */
    bput(&t->body, "%s", init_code.p ? init_code.p : "");
    free(init_code.p);
    return 1;
}

/* ---- operands ---- */

static int swz_index(char c)
{
    switch (c) {
    case 'x': case 'r': return 0;
    case 'y': case 'g': return 1;
    case 'z': case 'b': return 2;
    case 'w': case 'a': return 3;
    }
    return -1;
}

/* Split "name[idx].sw" into name, index text and swizzle text. */
static void split_ref(char *s, char **name, char **idx, char **sw)
{
    *idx = NULL; *sw = NULL; *name = s;
    char *lb = strchr(s, '[');
    char *last_dot = strrchr(s, '.');
    if (lb) {
        char *rb = strchr(lb, ']');
        if (rb) {
            *lb = 0; *rb = 0; *idx = lb + 1;
            if (rb[1] == '.') *sw = rb + 2;
            return;
        }
    }
    /* trailing ".xyzw"-style suffix on a plain name */
    if (last_dot) {
        int ok = 1;
        for (char *q = last_dot + 1; *q; q++) if (swz_index(*q) < 0) ok = 0;
        size_t L = strlen(last_dot + 1);
        if (ok && L >= 1 && L <= 4) { *last_dot = 0; *sw = last_dot + 1; }
    }
}

static int index_expr(Tr *t, const char *idx, int base, char *out, size_t n)
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", idx);
    char *s = trim(tmp);
    if (isdigit((unsigned char)*s)) { snprintf(out, n, "%d", base + atoi(s)); return 1; }
    /* addr.x [+|- k] */
    char a[64]; char comp = 'x'; int k = 0;
    char *dot = strchr(s, '.');
    if (!dot) return fail(t, "index %s", idx);
    snprintf(a, sizeof a, "%.*s", (int)(dot - s), s);
    comp = dot[1];
    const char *rest = dot + 2;
    while (*rest == ' ') rest++;
    if (*rest == '+') k = atoi(rest + 1);
    else if (*rest == '-') k = -atoi(rest + 1);
    Sym *y = sym_find(t, trim(a));
    if (!y || y->kind != SYM_ADDR) return fail(t, "address register %s", a);
    snprintf(out, n, "(%d + a_%s.%c)", base + k, y->name, comp);
    return 1;
}

/* A source operand -> a vec4 expression. */
static int src_expr(Tr *t, const char *src_in, char *out, size_t n)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", src_in);
    char *s = trim(buf);
    int neg = 0, absv = 0;
    if (*s == '-') { neg = 1; s = trim(s + 1); }
    else if (*s == '+') s = trim(s + 1);
    if (*s == '|') {
        absv = 1; s++;
        char *e = strrchr(s, '|');
        if (e) *e = 0;
        s = trim(s);
    }
    char base[200];
    char *nm, *idx, *sw;
    if (*s == '{' || isdigit((unsigned char)*s) || *s == '.') {
        if (!binding_expr(t, s, base, sizeof base)) return 0;
        sw = NULL;
    } else if (!strncmp(s, "vertex.", 7) || !strncmp(s, "fragment.", 9) ||
               !strncmp(s, "program.", 8) || !strncmp(s, "state.", 6)) {
        /* inline binding, maybe with a swizzle after it */
        char *dot = strrchr(s, '.');
        sw = NULL;
        if (dot) {          /* a last segment made only of swizzle letters is a swizzle */
            int ok = 1; size_t L = strlen(dot + 1);
            for (char *q = dot + 1; *q; q++) if (swz_index(*q) < 0) ok = 0;
            if (ok && L >= 1 && L <= 4) { *dot = 0; sw = dot + 1; }
        }
        if (!binding_expr(t, s, base, sizeof base)) return 0;
    } else {
        split_ref(s, &nm, &idx, &sw);
        Sym *y = sym_find(t, trim(nm));
        if (!y) return fail(t, "unknown name %s", nm);
        if (y->kind == SYM_ARRAY) {
            if (!idx) return fail(t, "array %s without index", nm);
            char ie[128];
            if (!index_expr(t, idx, y->base, ie, sizeof ie)) return 0;
            snprintf(base, sizeof base, "%s[%s]", y->expr, ie);
        } else if (y->kind == SYM_TEMP) {
            snprintf(base, sizeof base, "t_%s", y->name);
        } else if (y->kind == SYM_EXPR || y->kind == SYM_OUT) {
            snprintf(base, sizeof base, "%s", y->expr);
        } else return fail(t, "%s used as a source", nm);
    }
    char e[256];
    if (sw && *sw) {
        size_t L = strlen(sw);
        char s4[5];
        for (int k = 0; k < 4; k++) s4[k] = "xyzw"[swz_index(sw[k < (int)L ? k : (int)L - 1])];
        s4[4] = 0;
        snprintf(e, sizeof e, "(%s).%s", base, s4);
    } else snprintf(e, sizeof e, "(%s)", base);
    if (absv) { char a[300]; snprintf(a, sizeof a, "abs%s", e); snprintf(e, sizeof e, "%s", a); }
    if (neg) snprintf(out, n, "(-%s)", e); else snprintf(out, n, "%s", e);
    return 1;
}

/* Destination: name[.mask] [(cond)] */
typedef struct { char lv[200]; char mask[5]; char cond[16]; char condsw[5]; int addr; } Dst;

static int dst_parse(Tr *t, const char *d_in, Dst *d)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", d_in);
    char *s = trim(buf);
    memset(d, 0, sizeof *d);
    char *lp = strchr(s, '(');
    if (lp) {
        char *rp = strchr(lp, ')');
        if (rp) *rp = 0;
        char *cs = trim(lp + 1);
        char *dot = strchr(cs, '.');
        if (dot) { *dot = 0; snprintf(d->condsw, sizeof d->condsw, "%s", dot + 1); }
        snprintf(d->cond, sizeof d->cond, "%s", cs);
        *lp = 0;
        s = trim(s);
    }
    char *nm, *idx, *sw;
    if (!strncmp(s, "result.", 7)) {
        char *dot = strrchr(s, '.');
        sw = NULL;
        if (dot) {
            int ok = 1; size_t L = strlen(dot + 1);
            for (char *q = dot + 1; *q; q++) if (!strchr("xyzw", *q)) ok = 0;
            if (ok && L >= 1 && L <= 4) { *dot = 0; sw = dot + 1; }
        }
        if (!output_expr(t, s, d->lv, sizeof d->lv)) return 0;
    } else {
        split_ref(s, &nm, &idx, &sw);
        Sym *y = sym_find(t, trim(nm));
        if (!y) return fail(t, "unknown destination %s", nm);
        if (y->kind == SYM_TEMP) snprintf(d->lv, sizeof d->lv, "t_%s", y->name);
        else if (y->kind == SYM_OUT) snprintf(d->lv, sizeof d->lv, "%s", y->expr);
        else if (y->kind == SYM_ADDR) { snprintf(d->lv, sizeof d->lv, "a_%s", y->name); d->addr = 1; }
        else return fail(t, "cannot write %s", nm);
    }
    snprintf(d->mask, sizeof d->mask, "%s", sw && *sw ? sw : "xyzw");
    for (char *q = d->mask; *q; q++) { int k = swz_index(*q); if (k < 0) return fail(t, "mask %s", d->mask); *q = "xyzw"[k]; }
    return 1;
}

static void cond_test(const char *cond, const char *csw, char *out, size_t n)
{
    const char *op = !strcmp(cond, "EQ") ? "==" : !strcmp(cond, "NE") ? "!=" :
                     !strcmp(cond, "LT") ? "<"  : !strcmp(cond, "LE") ? "<=" :
                     !strcmp(cond, "GT") ? ">"  : !strcmp(cond, "GE") ? ">=" : NULL;
    char s4[5] = "xyzw";
    if (csw && *csw) {
        size_t L = strlen(csw);
        for (int k = 0; k < 4; k++) s4[k] = "xyzw"[swz_index(csw[k < (int)L ? k : (int)L - 1])];
    }
    if (!strcmp(cond, "TR")) snprintf(out, n, "bvec4(true)");
    else if (!strcmp(cond, "FL")) snprintf(out, n, "bvec4(false)");
    else snprintf(out, n, "bvec4(cc.%c %s 0.0, cc.%c %s 0.0, cc.%c %s 0.0, cc.%c %s 0.0)",
                  s4[0], op, s4[1], op, s4[2], op, s4[3], op);
}

/* ---- instructions ---- */

static const char *const k_ops[] = {
    "ABS","ADD","ARL","ARR","ARA","BRA","CAL","CMP","COS","DP3","DP4","DPH","DST","EX2",
    "EXP","FLR","FRC","KIL","LG2","LIT","LOG","LRP","MAD","MAX","MIN","MOV","MUL","POW",
    "RCC","RCP","RET","RSQ","SCS","SEQ","SFL","SGE","SGT","SIN","SLE","SLT","SNE","SSG",
    "STR","SUB","SWZ","TEX","TXB","TXP","XPD","NRM","END", NULL
};

static void block_split(Tr *t)
{
    if (!t->has_flow) return;
    t->block++;
    bput(&t->body, "    } else if (pc == %d) {\n", t->block);
}

static int label_index(Tr *t, const char *name)
{
    for (int i = 0; i < t->nlabel; i++) if (!strcmp(t->label[i], name)) return i;
    return -1;
}

/* Block number for each label, computed in a first pass. */
static int g_label_block[64];

static int emit_instr(Tr *t, char *stmt)
{
    char *s = trim(stmt);
    char opw[32];
    int k = 0;
    while (s[k] && !isspace((unsigned char)s[k]) && k < 31) { opw[k] = s[k]; k++; }
    opw[k] = 0;
    char *args = trim(s + k);

    /* opcode, C (condition-update) suffix, _SAT */
    int sat = 0, setcc = 0;
    char op[32];
    snprintf(op, sizeof op, "%s", opw);
    char *us = strstr(op, "_SAT");
    if (us) { sat = 1; *us = 0; }
    int known = 0;
    for (int i = 0; k_ops[i]; i++) if (!strcmp(op, k_ops[i])) known = 1;
    if (!known && strlen(op) > 3 && op[strlen(op) - 1] == 'C') {
        op[strlen(op) - 1] = 0;
        for (int i = 0; k_ops[i]; i++) if (!strcmp(op, k_ops[i])) known = 1;
        if (known) setcc = 1;
        else op[strlen(op)] = 'C';
    }
    /* NV_fragment_program precision: MULR, MOVH, ADDXC - R (fp32), H (fp16)
     * and X (fixed point) all compute in full precision here */
    if (!known) {
        size_t L = strlen(op);
        int cc = L > 4 && op[L - 1] == 'C';
        char last = op[L - 1 - cc];
        if (L - cc > 3 && (last == 'R' || last == 'H' || last == 'X')) {
            char base[32];
            snprintf(base, sizeof base, "%.*s", (int)(L - 1 - cc), op);
            for (int i = 0; k_ops[i]; i++) if (!strcmp(base, k_ops[i])) known = 1;
            if (known) { snprintf(op, sizeof op, "%s", base); setcc = cc; }
        }
    }
    if (!known) return fail(t, "opcode %s", opw);

    if (!strcmp(op, "END")) {
        if (t->has_flow) bput(&t->body, "      pc = -1;\n");
        return 1;
    }

    /* arguments: comma separated, but not inside {} or [] or () */
    char *a[6] = {0};
    int na = 0, depth = 0;
    char *start = args;
    for (char *q = args; ; q++) {
        if (*q == '{' || *q == '[' || *q == '(') depth++;
        else if (*q == '}' || *q == ']' || *q == ')') depth--;
        if ((*q == ',' && depth == 0) || !*q) {
            int end = !*q;
            *q = 0;
            if (na < 6) a[na++] = trim(start);
            if (end) break;
            start = q + 1;
        }
    }

    /* control flow */
    if (!strcmp(op, "BRA") || !strcmp(op, "CAL") || !strcmp(op, "RET")) {
        char lab[64] = "", cond[16] = "TR", csw[5] = "";
        char *x = a[0] ? a[0] : (char *)"";
        char *lp = strchr(x, '(');
        if (lp) {
            char *rp = strchr(lp, ')'); if (rp) *rp = 0;
            char *cs = trim(lp + 1);
            char *dot = strchr(cs, '.');
            if (dot) { *dot = 0; snprintf(csw, sizeof csw, "%s", dot + 1); }
            snprintf(cond, sizeof cond, "%s", cs);
            *lp = 0;
        }
        snprintf(lab, sizeof lab, "%s", trim(x));
        char test[200];
        cond_test(cond, csw, test, sizeof test);
        int next = t->block + 1;
        if (!strcmp(op, "RET")) {
            bput(&t->body, "      if (any(%s)) { if (sp > 0) { sp--; pc = stk[sp]; } else pc = -1; } else pc = %d;\n",
                 test, next);
        } else {
            int li = label_index(t, lab);
            if (li < 0) return fail(t, "label %s", lab);
            if (!strcmp(op, "BRA"))
                bput(&t->body, "      if (any(%s)) pc = %d; else pc = %d;\n", test, g_label_block[li], next);
            else
                bput(&t->body, "      if (any(%s)) { stk[sp] = %d; sp++; pc = %d; } else pc = %d;\n",
                     test, next, g_label_block[li], next);
        }
        block_split(t);
        return 1;
    }

    if (!strcmp(op, "KIL")) {
        char e0[300];
        if (!src_expr(t, a[0], e0, sizeof e0)) return 0;
        bput(&t->body, "      if (any(lessThan(%s, vec4(0.0)))) discard;\n", e0);
        return 1;
    }

    Dst d;
    if (!a[0] || !dst_parse(t, a[0], &d)) return fail(t, "destination in %s", s);

    char e[4][300] = {{0}};
    int nsrc = na - 1;
    int is_tex = !strcmp(op, "TEX") || !strcmp(op, "TXP") || !strcmp(op, "TXB");
    int is_swz = !strcmp(op, "SWZ");
    for (int i = 0; i < nsrc && i < 4; i++) {
        if (is_tex && i >= 1) break;
        if (is_swz && i >= 1) break;
        if (!src_expr(t, a[i + 1], e[i], sizeof e[i])) return 0;
    }
    char r[1200];
#define X(i) e[i]
    if      (!strcmp(op, "MOV")) snprintf(r, sizeof r, "%s", X(0));
    else if (!strcmp(op, "ABS")) snprintf(r, sizeof r, "abs(%s)", X(0));
    else if (!strcmp(op, "ADD")) snprintf(r, sizeof r, "%s + %s", X(0), X(1));
    else if (!strcmp(op, "SUB")) snprintf(r, sizeof r, "%s - %s", X(0), X(1));
    else if (!strcmp(op, "MUL")) snprintf(r, sizeof r, "%s * %s", X(0), X(1));
    else if (!strcmp(op, "MAD")) snprintf(r, sizeof r, "%s * %s + %s", X(0), X(1), X(2));
    else if (!strcmp(op, "MIN")) snprintf(r, sizeof r, "min(%s, %s)", X(0), X(1));
    else if (!strcmp(op, "MAX")) snprintf(r, sizeof r, "max(%s, %s)", X(0), X(1));
    else if (!strcmp(op, "DP3")) snprintf(r, sizeof r, "vec4(dot(%s.xyz, %s.xyz))", X(0), X(1));
    else if (!strcmp(op, "DP4")) snprintf(r, sizeof r, "vec4(dot(%s, %s))", X(0), X(1));
    else if (!strcmp(op, "DPH")) snprintf(r, sizeof r, "vec4(dot(%s.xyz, %s.xyz) + %s.w)", X(0), X(1), X(1));
    else if (!strcmp(op, "DST")) snprintf(r, sizeof r, "vec4(1.0, %s.y * %s.y, %s.z, %s.w)", X(0), X(1), X(0), X(1));
    else if (!strcmp(op, "SLT")) snprintf(r, sizeof r, "vec4(lessThan(%s, %s))", X(0), X(1));
    else if (!strcmp(op, "SGE")) snprintf(r, sizeof r, "vec4(greaterThanEqual(%s, %s))", X(0), X(1));
    else if (!strcmp(op, "SGT")) snprintf(r, sizeof r, "vec4(greaterThan(%s, %s))", X(0), X(1));
    else if (!strcmp(op, "SLE")) snprintf(r, sizeof r, "vec4(lessThanEqual(%s, %s))", X(0), X(1));
    else if (!strcmp(op, "SEQ")) snprintf(r, sizeof r, "vec4(equal(%s, %s))", X(0), X(1));
    else if (!strcmp(op, "SNE")) snprintf(r, sizeof r, "vec4(notEqual(%s, %s))", X(0), X(1));
    else if (!strcmp(op, "SFL")) snprintf(r, sizeof r, "vec4(0.0)");
    else if (!strcmp(op, "STR")) snprintf(r, sizeof r, "vec4(1.0)");
    else if (!strcmp(op, "SSG")) snprintf(r, sizeof r, "sign(%s)", X(0));
    else if (!strcmp(op, "FLR")) snprintf(r, sizeof r, "floor(%s)", X(0));
    else if (!strcmp(op, "FRC")) snprintf(r, sizeof r, "fract(%s)", X(0));
    else if (!strcmp(op, "RCP")) snprintf(r, sizeof r, "vec4(1.0 / %s.x)", X(0));
    else if (!strcmp(op, "RCC")) snprintf(r, sizeof r,
        "vec4(sign(%s.x) * clamp(1.0 / abs(%s.x), 1.884467e-19, 5.42101e+19))", X(0), X(0));
    else if (!strcmp(op, "RSQ")) snprintf(r, sizeof r, "vec4(inversesqrt(abs(%s.x)))", X(0));
    else if (!strcmp(op, "EX2")) snprintf(r, sizeof r, "vec4(exp2(%s.x))", X(0));
    else if (!strcmp(op, "LG2")) snprintf(r, sizeof r, "vec4(log2(abs(%s.x)))", X(0));
    else if (!strcmp(op, "POW")) snprintf(r, sizeof r, "vec4(pow(%s.x, %s.x))", X(0), X(1));
    else if (!strcmp(op, "COS")) snprintf(r, sizeof r, "vec4(cos(%s.x))", X(0));
    else if (!strcmp(op, "SIN")) snprintf(r, sizeof r, "vec4(sin(%s.x))", X(0));
    else if (!strcmp(op, "SCS")) snprintf(r, sizeof r, "vec4(cos(%s.x), sin(%s.x), 0.0, 0.0)", X(0), X(0));
    else if (!strcmp(op, "EXP")) snprintf(r, sizeof r,
        "vec4(exp2(floor(%s.x)), fract(%s.x), exp2(%s.x), 1.0)", X(0), X(0), X(0));
    else if (!strcmp(op, "LOG")) snprintf(r, sizeof r,
        "vec4(floor(log2(abs(%s.x))), abs(%s.x) / exp2(floor(log2(abs(%s.x)))), log2(abs(%s.x)), 1.0)",
        X(0), X(0), X(0), X(0));
    else if (!strcmp(op, "LIT")) snprintf(r, sizeof r,
        "arb_lit(%s)", X(0));
    else if (!strcmp(op, "LRP")) snprintf(r, sizeof r, "mix(%s, %s, %s)", X(2), X(1), X(0));
    else if (!strcmp(op, "CMP")) snprintf(r, sizeof r,
        "mix(%s, %s, vec4(lessThan(%s, vec4(0.0))))", X(2), X(1), X(0));
    else if (!strcmp(op, "XPD")) snprintf(r, sizeof r, "vec4(cross(%s.xyz, %s.xyz), 1.0)", X(0), X(1));
    else if (!strcmp(op, "NRM")) snprintf(r, sizeof r, "vec4(normalize(%s.xyz), 1.0)", X(0));
    else if (!strcmp(op, "ARL") || !strcmp(op, "ARR") || !strcmp(op, "ARA")) {
        if (!d.addr) return fail(t, "%s to a non-address register", op);
        const char *fn = !strcmp(op, "ARR") ? "floor(%s + 0.5)" : "floor(%s)";
        char inner[400];
        snprintf(inner, sizeof inner, fn, X(0));
        if (!strcmp(op, "ARA")) return fail(t, "ARA");
        for (const char *m = d.mask; *m; m++)
            bput(&t->body, "      %s.%c = int((%s).%c);\n", d.lv, *m, inner, *m);
        return 1;
    } else if (is_swz) {
        /* SWZ dst, src, c0, c1, c2, c3 - each -?[xyzw01] */
        char comp[4][64];
        for (int i = 0; i < 4; i++) {
            const char *c = a[i + 2] ? trim(a[i + 2]) : "0";
            int neg = *c == '-'; if (*c == '-' || *c == '+') c++;
            if (*c == '0' || *c == '1') snprintf(comp[i], sizeof comp[i], "%s%c.0", neg ? "-" : "", *c);
            else snprintf(comp[i], sizeof comp[i], "%s%s.%c", neg ? "-" : "", X(0), "xyzw"[swz_index(*c) < 0 ? 0 : swz_index(*c)]);
        }
        snprintf(r, sizeof r, "vec4(%s, %s, %s, %s)", comp[0], comp[1], comp[2], comp[3]);
    } else if (is_tex) {
        /* TEX dst, coord, texture[n], 2D */
        int unit = 0;
        const char *tx = a[2] ? a[2] : "texture[0]";
        if (sscanf(tx, " texture[%d]", &unit) != 1) unit = 0;
        const char *tg = a[3] ? a[3] : "2D";
        int target = !strcmp(tg, "1D") ? 1 : !strcmp(tg, "2D") ? 2 : !strcmp(tg, "3D") ? 3 :
                     !strcmp(tg, "CUBE") ? 4 : !strcmp(tg, "RECT") ? 5 :
                     !strcmp(tg, "SHADOW2D") ? 6 : 0;   /* ARB_fragment_program_shadow: a depth compare */
        if (!target || unit < 0 || unit >= 16) return fail(t, "texture %s %s", tx, tg);
        /* SHADOW2D on a unit the program also reads as 2D: the texture has no
         * depth compare (Let's Go Jungle's exposure reads its luminance
         * texture both ways), and NVIDIA then returns the texel itself */
        int shadow_as_2d = target == 6 && t->tex_target[unit] == 2;
        if (shadow_as_2d) target = 2;
        if (t->tex_target[unit] && t->tex_target[unit] != target)
            return fail(t, "texture unit %d used with two targets", unit);
        t->tex_target[unit] = target;
        int proj = !strcmp(op, "TXP"), bias = !strcmp(op, "TXB");
        static const char *const fnb[] = { "", "texture1D", "texture2D", "texture3D", "textureCube", "texture2DRect", "shadow2D" };
        static const char *const sw[] = { "", ".x", ".xy", ".xyz", ".xyz", ".xy", ".xyz" };
        static const char *const sm[] = { "", "s1_", "s2_", "s3_", "sc_", "sr_", "ss_" };
        if (target == 5) {
            /* Rectangle textures are sampled in texels; the game's render
             * targets are, and stereo.c draws them at the eye's size. */
#ifdef LINDBERGH_GL4ES
            /* multiview: the render target's layer for the view being drawn */
            if (gl4es_mv_shaders && proj) snprintf(r, sizeof r, "texture2D(sr_%d, vec3(%s.xy / %s.w * u_rect_norm[%d], gl4es_layer))", unit, X(0), X(0), unit);
            else if (gl4es_mv_shaders) snprintf(r, sizeof r, "texture2D(sr_%d, vec3(%s.xy * u_rect_norm[%d], gl4es_layer))", unit, X(0), unit);
            else if (proj) snprintf(r, sizeof r, "texture2D(sr_%d, %s.xy / %s.w * u_rect_norm[%d])", unit, X(0), X(0), unit);
            else      snprintf(r, sizeof r, "texture2D(sr_%d, %s.xy * u_rect_norm[%d])", unit, X(0), unit);
#else
            if (proj) snprintf(r, sizeof r, "texture2DRectProj(sr_%d, vec4(%s.xy * u_rect_scale[%d], %s.zw))", unit, X(0), unit, X(0));
            else      snprintf(r, sizeof r, "texture2DRect(sr_%d, %s.xy * u_rect_scale[%d])", unit, X(0), unit);
#endif
        } else if (proj && target != 4)
            snprintf(r, sizeof r, "%sProj(%s%d, %s)", fnb[target], sm[target], unit, X(0));
        else if (bias)
            snprintf(r, sizeof r, "%s(%s%d, %s%s, %s.w)", fnb[target], sm[target], unit, X(0), sw[target], X(0));
        else if (shadow_as_2d)
            snprintf(r, sizeof r, "texture2D(s2_%d, %s.xy).xxxx", unit, X(0));
        else
            snprintf(r, sizeof r, "%s(%s%d, %s%s)", fnb[target], sm[target], unit, X(0), sw[target]);
    } else return fail(t, "opcode %s not translated", op);
#undef X

    bput(&t->body, "      r = %s%s%s;\n", sat ? "clamp(" : "", r, sat ? ", 0.0, 1.0)" : "");
    if (d.cond[0]) {
        char test[200];
        cond_test(d.cond, d.condsw, test, sizeof test);
        bput(&t->body, "      m = %s;\n", test);
        for (const char *m = d.mask; *m; m++)
            bput(&t->body, "      if (m.%c) %s.%c = r.%c;\n", *m, d.lv, *m, *m);
    } else {
        bput(&t->body, "      %s.%s = r.%s;\n", d.lv, d.mask, d.mask);
    }
    if (setcc) bput(&t->body, "      cc.%s = r.%s;\n", d.mask, d.mask);
    return 1;
}

/* Split the program into statements with their labels, comments removed. */
typedef struct { char *labels[4]; int nl; char *text; } Stmt;

static int split_program(char *src, Stmt *st, int maxst)
{
    /* strip comments */
    for (char *p = src; *p; p++) {
        if (*p == '#') { while (*p && *p != '\n') *p++ = ' '; if (!*p) break; }
    }
    int n = 0;
    char *save = NULL;
    for (char *s = strtok_r(src, ";", &save); s && n < maxst; s = strtok_r(NULL, ";", &save)) {
        Stmt *x = &st[n];
        memset(x, 0, sizeof *x);
        s = trim(s);
        /* leading labels "name:" */
        for (;;) {
            char *colon = strchr(s, ':');
            if (!colon) break;
            int ident = colon > s;
            for (char *q = s; q < colon; q++) if (!isalnum((unsigned char)*q) && *q != '_') ident = 0;
            if (!ident || x->nl >= 4) break;
            *colon = 0;
            x->labels[x->nl++] = s;
            s = trim(colon + 1);
        }
        /* NV_fragment_program2's precision on temporaries ("SHORT TEMP H0"):
         * everything is full precision here */
        if ((!strncmp(s, "SHORT", 5) || !strncmp(s, "LONG", 4))) {
            char *q = trim(s + (s[0] == 'S' ? 5 : 4));
            if (!strncmp(q, "TEMP", 4) && isspace((unsigned char)q[4])) s = q;
        }
        x->text = s;
        n++;
    }
    return n;
}

static int is_decl(const char *s, const char *kw)
{
    size_t L = strlen(kw);
    return !strncmp(s, kw, L) && isspace((unsigned char)s[L]);
}

/* Translate one program to a GLSL shader. Returns malloc'd source or NULL. */
/* every write of result.depth copies fragment.position.z */
static int depth_passthrough(const char *body)
{
    static const char pat[] = "r = (gl_FragCoord).zzzz;\n      o_dep.z = r.z;";
    int writes = 0, copies = 0;
    for (const char *p = body; p && (p = strstr(p, "o_dep")); p += 5) writes++;
    for (const char *p = body; p && (p = strstr(p, pat)); p += sizeof pat - 1) copies++;
    return writes > 0 && writes == copies;
}

static GLuint g_tr_id;      /* the program being translated, for LINDBERGH_FP_ONLY */
static char *translate(int stage, const char *src_in, Tr *t)
{
    memset(t, 0, sizeof *t);
    t->stage = stage;
    t->env_hi = t->local_hi = -1;
    char *src = strdup(src_in);
    {   /* "texture [2]": the grammar allows space before a subscript; the
         * parsers below expect none (the plane's air shimmer was rejected) */
        char *w = src;
        for (char *r = src; *r; r++) {
            if (*r == ' ' || *r == '\t') {
                char *q = r; while (*q == ' ' || *q == '\t') q++;
                if (*q == '[') { r = q - 1; continue; }
            }
            *w++ = *r;
        }
        *w = 0;
    }
    char *p = src;
    while (*p && *p != '\n') p++;              /* drop the !!ARBxp1.0 line */
    static Stmt st[4096];
    int n = split_program(p, st, 4096);

    /* pass 1: declarations, options, labels and the block layout */
    int block = 0;
    for (int i = 0; i < n; i++) {
        char *s = st[i].text;
        for (int l = 0; l < st[i].nl; l++) {
            if (t->nlabel >= 64) { fail(t, "too many labels"); goto out; }
            snprintf(t->label[t->nlabel], 64, "%s", st[i].labels[l]);
            t->has_flow = 1;
            t->nlabel++;
        }
        if (!strncmp(s, "BRA", 3) || !strncmp(s, "CAL", 3) || !strncmp(s, "RET", 3)) t->has_flow = 1;
    }
    /* label -> block number: a block starts at each labelled statement and
     * after each branch, call or return */
    if (t->has_flow) {
        int li = 0;
        block = 0;
        int started = 1;      /* block 0 started at the top */
        for (int i = 0; i < n; i++) {
            if (st[i].nl) {
                if (!started) block++;
                for (int l = 0; l < st[i].nl; l++) g_label_block[li++] = block;
            }
            started = 0;
            const char *s = st[i].text;
            if (!strncmp(s, "BRA", 3) || !strncmp(s, "CAL", 3) || !strncmp(s, "RET", 3)) {
                block++;
                started = 1;
            }
        }
    }

    bput(&t->body, "");
    for (int i = 0; i < n; i++) {
        char *s = st[i].text;
        if (!*s) continue;
        if (is_decl(s, "OPTION")) {
            char *o = trim(s + 6);
            if (!strcmp(o, "ARB_position_invariant")) t->pos_invariant = 1;
            else if (!strcmp(o, "ARB_fog_linear")) t->fog_mode = 1;
            else if (!strcmp(o, "ARB_fog_exp")) t->fog_mode = 2;
            else if (!strcmp(o, "ARB_fog_exp2")) t->fog_mode = 3;
            continue;    /* precision hints and NV_vertex_program2 need nothing */
        }
        if (is_decl(s, "TEMP") || is_decl(s, "ADDRESS")) {
            int addr = s[0] == 'A';
            char *save = NULL;
            for (char *nm = strtok_r(s + (addr ? 7 : 4), ",", &save); nm; nm = strtok_r(NULL, ",", &save)) {
                nm = trim(nm);
                if (!sym_add(t, nm, addr ? SYM_ADDR : SYM_TEMP)) goto out;
                bput(&t->decl, addr ? "ivec4 a_%s = ivec4(0);\n" : "vec4 t_%s = vec4(0.0);\n", nm);
            }
            continue;
        }
        if (is_decl(s, "ATTRIB") || is_decl(s, "PARAM") || is_decl(s, "OUTPUT") || is_decl(s, "ALIAS")) {
            int kind = s[0] == 'O' ? 2 : s[0] == 'A' && s[1] == 'L' ? 3 : s[0] == 'P' ? 1 : 0;
            char *eq = strchr(s, '=');
            if (!eq) { fail(t, "declaration %s", s); goto out; }
            *eq = 0;
            char *nm = trim(s + (kind == 0 ? 6 : kind == 1 ? 5 : kind == 2 ? 6 : 5));
            char *val = trim(eq + 1);
            char *br = strchr(nm, '[');
            if (kind == 1 && br) {
                *br = 0;
                if (!declare_param_array(t, trim(nm), val)) goto out;
                continue;
            }
            if (kind == 1 && !strncmp(val, "state.matrix.", 13) && !strstr(val, ".row[")) {
                if (!declare_param_array(t, nm, val)) goto out;
                continue;
            }
            if (kind == 3) {
                Sym *o = sym_find(t, val);
                if (!o) { fail(t, "alias of %s", val); goto out; }
                Sym *y = sym_add(t, nm, o->kind);
                if (!y) goto out;
                Sym keep = *o;
                snprintf(keep.name, sizeof keep.name, "%s", nm);
                *y = keep;
                if (o->kind == SYM_TEMP || o->kind == SYM_ADDR) {
                    /* aliases of temps share storage: point at the original name */
                    y->kind = SYM_OUT;
                    snprintf(y->expr, sizeof y->expr, "%s_%s", o->kind == SYM_TEMP ? "t" : "a", o->name);
                }
                continue;
            }
            Sym *y = sym_add(t, nm, kind == 2 ? SYM_OUT : SYM_EXPR);
            if (!y) goto out;
            if (kind == 2) { if (!output_expr(t, val, y->expr, sizeof y->expr)) goto out; }
            else if (!binding_expr(t, val, y->expr, sizeof y->expr)) goto out;
            continue;
        }
        /* an instruction: pass 2. Declarations may follow it, so keep going. */
    }
    if (t->err[0]) goto out;

    /* pass 2: instructions */
    if (t->has_flow) bput(&t->body, "  for (int it = 0; it < 4096; it++) {\n    if (pc < 0) break;\n    if (pc == 0) {\n");
    for (int i = 0; i < n; i++) {
        char *s = st[i].text;
        if (st[i].nl && t->has_flow) {
            /* this statement starts a block if the previous one did not just end one */
            int want = g_label_block[label_index(t, st[i].labels[0])];
            if (want != t->block) {
                bput(&t->body, "      pc = %d;\n", want);
                t->block = want - 1;
                block_split(t);
            }
        }
        if (!*s || is_decl(s, "OPTION") || is_decl(s, "TEMP") || is_decl(s, "ADDRESS") ||
            is_decl(s, "ATTRIB") || is_decl(s, "PARAM") || is_decl(s, "OUTPUT") || is_decl(s, "ALIAS"))
            continue;
        if (!emit_instr(t, s)) goto out;
    }
    if (t->has_flow) bput(&t->body, "      pc = -1;\n    }\n  }\n");

    /* assemble */
    {
        Buf g = {0};
        bput(&g, "#version 120\n");
        int rect = 0;
        for (int u = 0; u < 16; u++) if (t->tex_target[u] == 5) rect = 1;
#ifdef LINDBERGH_GL4ES
        /* GLES has no rectangle textures: gl4es keeps them as 2D ones, which
         * are sampled in normalised coordinates - u_rect_norm[unit] is the
         * game's texel size of what is bound there. */
        if (rect) bput(&g, "uniform vec2 u_rect_norm[16];\n");
#else
        if (rect) bput(&g, "#extension GL_ARB_texture_rectangle : enable\n"
                           "uniform vec2 u_rect_scale[16];\n");    /* per unit: render targets drawn larger (stereo.c) */
#endif
        const char *P = stage ? "f" : "v";
        if (t->env_hi >= 0) bput(&g, "uniform vec4 %s_env[%d];\n", P, t->env_hi + 1);
        if (t->local_hi >= 0) bput(&g, "uniform vec4 %s_loc[%d];\n", P, t->local_hi + 1);
        for (int m = 0; m < t->nmat; m++) bput(&g, "uniform vec4 %s_m%d[4];\n", P, m);
#ifdef LINDBERGH_GL4ES
        /* multiview: one matrix per view, and render targets as two layers */
        if (!stage && !t->nmat && !t->pos_invariant) bput(&g, gl4es_mv_shaders ? "uniform mat4 u_reproj[2];\n" : "uniform mat4 u_reproj;\n");
        const char *sty[] = { "", "sampler1D s1_", "sampler2D s2_", "sampler3D s3_",
                              "samplerCube sc_", gl4es_mv_shaders ? "sampler2DArray sr_" : "sampler2D sr_",
                              "sampler2DShadow ss_" };
#else
        if (!stage && !t->nmat && !t->pos_invariant) bput(&g, "uniform mat4 u_reproj;\n");
        static const char *const sty[] = { "", "sampler1D s1_", "sampler2D s2_", "sampler3D s3_",
                                           "samplerCube sc_", "sampler2DRect sr_", "sampler2DShadow ss_" };
#endif
        for (int u = 0; u < 16; u++) if (t->tex_target[u]) bput(&g, "uniform %s%d;\n", sty[t->tex_target[u]], u);
        for (int a = 1; a < 16; a++) if (t->attrib_used[a]) bput(&g, "attribute vec4 va_%d;\n", a);
        if (!stage && t->gen_alias) bput(&g, "uniform float u_gen[16];\n");
        bput(&g, "vec4 arb_lit(vec4 s) {\n"
                 "  float d = max(s.x, 0.0), sp = max(s.y, 0.0), e = clamp(s.w, -128.0, 128.0);\n"
                 "  return vec4(1.0, d, s.x > 0.0 ? (sp > 0.0 ? pow(sp, e) : 0.0) : 0.0, 1.0);\n}\n");
        bput(&g, "%s", t->decl.p ? t->decl.p : "");
        /* gl4es puts its own declarations above the line before main(): make
         * that line a global, never the end of a function like arb_lit. */
        bput(&g, "const float arb_one = 1.0;\n");
        bput(&g, "void main() {\n  vec4 r = vec4(0.0), cc = vec4(0.0);\n  bvec4 m = bvec4(false);\n");
        if (t->has_flow) bput(&g, "  int pc = 0, sp = 0;\n  int stk[16];\n");
        if (stage == 0) {
            bput(&g, "  vec4 o_fog = vec4(0.0), o_psize = vec4(1.0);\n");
            bput(&g, "  gl_FrontColor = vec4(0.0); gl_FrontSecondaryColor = vec4(0.0);\n"
                     "  gl_BackColor = vec4(0.0); gl_BackSecondaryColor = vec4(0.0);\n");
            if (t->pos_invariant) bput(&g, "  gl_Position = ftransform();\n");
        } else {
            bput(&g, "  vec4 o_col = vec4(0.0), o_dep = vec4(gl_FragCoord.z);\n");
        }
        bput(&g, "%s", t->body.p ? t->body.p : "");
        if (stage == 0) {
            bput(&g, "  gl_FogFragCoord = o_fog.x;\n  gl_PointSize = o_psize.x;\n");
            /* A program that takes its whole transform from the game's own
             * constants has the game's projection baked in: swap it for the
             * eye's (stereo.c; identity when not in stereo). */
#ifdef LINDBERGH_GL4ES
            /* multiview: gl4es clips to the game's screen from gl4es_mvPre */
            if (!t->nmat && !t->pos_invariant && gl4es_mv_shaders) bput(&g, "  gl4es_mvPre = gl_Position;\n  gl_Position = u_reproj[gl_ViewID_OVR] * gl_Position;\n");
            else
#endif
            if (!t->nmat && !t->pos_invariant) bput(&g, "  gl_Position = u_reproj * gl_Position;\n");
        } else {
            if (t->fog_mode) {
                const char *f = t->fog_mode == 1 ? "(gl_Fog.end - gl_FogFragCoord) * gl_Fog.scale" :
                                t->fog_mode == 2 ? "exp(-gl_Fog.density * gl_FogFragCoord)" :
                                "exp(-(gl_Fog.density * gl_FogFragCoord) * (gl_Fog.density * gl_FogFragCoord))";
                bput(&g, "  o_col.rgb = mix(gl_Fog.color.rgb, o_col.rgb, clamp(%s, 0.0, 1.0));\n", f);
            }
            {   /* LINDBERGH_FP_SOLID=1: every fragment flat green - do the
                 * draws rasterise at all? */
                static int mode = -1;
                if (mode < 0) { const char *v = getenv("LINDBERGH_FP_SOLID"); mode = v && *v ? atoi(v) : 0; }
                /* LINDBERGH_FP_ONLY=id: only that fragment program */
                const char *only = getenv("LINDBERGH_FP_ONLY");
                int solid = only && *only && (GLuint)atoi(only) != g_tr_id ? 0 : mode;
                if (solid == 1) bput(&g, "  gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);\n");
                /* =2: unit 0 fetched raw at texcoord 0 - is the texture black? */
                else if (solid == 2 && t->tex_target[0] == 2) bput(&g, "  gl_FragColor = vec4(texture2D(s2_0, gl_TexCoord[0].xy).rgb, 1.0);\n");
                else if (solid == 2 && t->tex_target[0] == 5) bput(&g, "  gl_FragColor = vec4(texture2DRect(sr_0, gl_TexCoord[0].xy).rgb, 1.0);\n");
                /* =3: texcoord 0 as colour - are the coordinates zero? */
                else if (solid == 3) bput(&g, "  gl_FragColor = vec4(fract(gl_TexCoord[0].xy), 0.5, 1.0);\n");
                /* =9: the program's id as a colour (r = id % 8, g = id / 8 % 8, b = id / 64 % 8, in sevenths) */
                else if (solid == 9) bput(&g, "  gl_FragColor = vec4(%d.0 / 7.0, %d.0 / 7.0, %d.0 / 7.0, 1.0);\n", (int)(g_tr_id % 8), (int)(g_tr_id / 8 % 8), (int)(g_tr_id / 64 % 8));
                /* =1N: unit 0 at texcoord N; =2N: texcoord N itself as colour */
                else if (solid >= 10 && solid < 18 && t->tex_target[0] == 2) bput(&g, "  gl_FragColor = vec4(texture2D(s2_0, gl_TexCoord[%d].xy).rgb, 1.0);\n", solid - 10);
                else if (solid >= 20 && solid < 28) bput(&g, "  gl_FragColor = vec4(gl_TexCoord[%d].xyz, 1.0);\n", solid - 20);
                else bput(&g, "  gl_FragColor = o_col;\n");
            }
            /* The game's programs write the depth they were given
             * (MOV result.depth.z, fragment.position.z): that changes nothing
             * but costs the GPU its early depth test, and through mediump on
             * GLSL ES 3.00 it rounds the depth away - skip it. */
            if (t->writes_depth && !depth_passthrough(t->body.p)) bput(&g, "  gl_FragDepth = o_dep.z;\n");
        }
        bput(&g, "}\n");
        free(src);
        free(t->decl.p); free(t->body.p);
        t->decl.p = t->body.p = NULL;
        return g.p;
    }
out:
    free(src);
    free(t->decl.p); free(t->body.p);
    t->decl.p = t->body.p = NULL;
    if (!t->err[0]) snprintf(t->err, sizeof t->err, "unknown");
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* linked programs, cached per (vertex, fragment) pair                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    GLuint vid, fid; unsigned vgen, fgen;
    GLuint prog;
    int    ok;
    MatRef mat[2][16]; int nmat[2];
    int    env_hi[2], local_hi[2];
    GLint  u_env[2], u_loc[2], u_mat[2][16], u_rect, u_reproj, u_gen;
    GLint  n_rect;                          /* u_rect_norm's length as the driver keeps it */
    unsigned rect_units;                /* texture units sampled as rectangles */
    unsigned up_env[2], up_loc[2];      /* generations last uploaded */
} Linked;

#define MAX_LINKED 1024
static Linked g_linked[MAX_LINKED];
static int g_nlinked;

static int g_dump = -1;

static GLuint compile(GLenum type, const char *src, int stage, GLuint id)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = "";
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "[arb] %s program %u: translated GLSL does not compile:\n%s\n----\n%s\n----\n",
                stage ? "fragment" : "vertex", id, log, src);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* An array uniform's length as the driver keeps it: drivers may shorten an
 * array to the last element the code uses (Adreno does), and an upload longer
 * than that is refused outright. want is the length the translation declared. */
static GLint array_len(GLuint prog, const char *name, GLint want)
{
    GLint n = 0;
    size_t k = strlen(name);
    glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
    for (GLint i = 0; i < n; i++) {
        char nm[64]; GLint sz = 0; GLenum ty;
        glGetActiveUniform(prog, (GLuint)i, sizeof nm, NULL, &sz, &ty, nm);
        if (!strncmp(nm, name, k) && (nm[k] == 0 || nm[k] == '[')) return sz > 0 && sz < want ? sz : want;
    }
    return want;
}

#ifdef LINDBERGH_GL4ES
/* ---- compiled programs kept on disk ----
 * Building a program pair takes the Quest's driver 30-110 ms, a visible hitch
 * the first time each scene draws. A built program's binary is kept in
 * ../shadercache (beside the game, in the app's own folder), named by a hash
 * of both stages' GLSL and the driver's own name and version; the next start
 * loads it instead. A binary the driver no longer takes is built again. */
static uint64_t fnv(uint64_t h, const char *p)
{
    for (; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ull;
    return (h ^ 0xff) * 1099511628211ull;
}

static void cache_path(char *out, size_t n, const char *v, const char *f)
{
    static uint64_t drv;
    if (!drv) {
        drv = fnv(1469598103934665603ull, (const char *)glGetString(GL_RENDERER));
        drv = fnv(drv, (const char *)glGetString(GL_VERSION));
        drv = fnv(drv, "arbprog 1");
#ifdef LINDBERGH_GL4ES
        if (gl4es_mv_shaders) drv = fnv(drv, "multiview 9");   /* gl4es makes other shaders of the same GLSL */
        if (gl4es_mv_shaders && getenv("LIBGL_MV_NOCLIP")) drv = fnv(drv, "noclip");
#endif
        mkdir("../shadercache", 0755);
    }
    uint64_t h = fnv(fnv(drv, v ? v : ""), f ? f : "");
    snprintf(out, n, "../shadercache/%016llx.bin", (unsigned long long)h);
}

static int cache_load(GLuint prog, const char *path)
{
    static int off = -1;     /* LINDBERGH_ARB_NOCACHE=1: build every program afresh, to see what fails */
    if (off < 0) { const char *v = getenv("LINDBERGH_ARB_NOCACHE"); off = v && *v == '1'; }
    if (off) return 0;
    FILE *fl = fopen(path, "rb");
    if (!fl) return 0;
    GLenum fmt = 0;
    long len = 0;
    void *data = NULL;
    if (fread(&fmt, sizeof fmt, 1, fl) == 1 && !fseek(fl, 0, SEEK_END) && (len = ftell(fl) - (long)sizeof fmt) > 0 &&
        !fseek(fl, sizeof fmt, SEEK_SET) && (data = malloc((size_t)len)) && fread(data, 1, (size_t)len, fl) != (size_t)len) {
        free(data); data = NULL;
    }
    fclose(fl);
    if (!data) return 0;
    glProgramBinary(prog, fmt, data, (GLsizei)len);
    free(data);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    while (glGetError() != GL_NO_ERROR) {}
    return ok;
}

static void cache_save(GLuint prog, const char *path)
{
    GLint len = 0;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0) return;
    void *data = malloc((size_t)len);
    if (!data) return;
    GLsizei got = 0;
    GLenum fmt = 0;
    glGetProgramBinary(prog, len, &got, &fmt, data);
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *fl = got > 0 ? fopen(tmp, "wb") : NULL;
    if (fl) {
        int ok = fwrite(&fmt, sizeof fmt, 1, fl) == 1 && fwrite(data, 1, (size_t)got, fl) == (size_t)got;
        if (fclose(fl) == 0 && ok) rename(tmp, path); else remove(tmp);
    }
    free(data);
}
#endif

extern unsigned g_frame;
static struct timespec g_link_t0;
static Linked *link_pair(GLuint vid, GLuint fid)
{
    ArbProg *vp = vid ? prog_get(0, vid, 0) : NULL;
    ArbProg *fp = fid ? prog_get(1, fid, 0) : NULL;
    unsigned vgen = vp ? vp->gen : 0, fgen = fp ? fp->gen : 0;
    for (int i = 0; i < g_nlinked; i++) {
        Linked *l = &g_linked[i];
        if (l->vid == vid && l->fid == fid && l->vgen == vgen && l->fgen == fgen) return l;
    }
    clock_gettime(CLOCK_MONOTONIC, &g_link_t0);    /* translate, compile and link */
    if (g_nlinked >= MAX_LINKED) {          /* full: start over */
        for (int i = 0; i < g_nlinked; i++) if (g_linked[i].prog) glDeleteProgram(g_linked[i].prog);
        g_nlinked = 0;
    }
    Linked *l = &g_linked[g_nlinked++];
    memset(l, 0, sizeof *l);
    l->vid = vid; l->fid = fid; l->vgen = vgen; l->fgen = fgen;
    l->up_env[0] = l->up_env[1] = l->up_loc[0] = l->up_loc[1] = ~0u;
    if (g_dump < 0) { const char *d = getenv("LINDBERGH_ARB_DUMP"); g_dump = d && *d && *d != '0'; }

    GLuint sh[2] = { 0, 0 };
    char *glsl_s[2] = { NULL, NULL };
    ArbProg *pp[2] = { vp, fp };
    GLuint ids[2] = { vid, fid };
    for (int st = 0; st < 2; st++) {
        if (!pp[st]) continue;
        if (pp[st]->failed || !pp[st]->src) { l->ok = 0; return l; }
        static Tr t;
        g_tr_id = ids[st];
        char *glsl = translate(st, pp[st]->src, &t);
        if (!glsl) {
            pp[st]->failed = 1;
            fprintf(stderr, "[arb] %s program %u not translated (%s); using the driver's path\n",
                    st ? "fragment" : "vertex", ids[st], t.err);
            if (g_dump > 0) fprintf(stderr, "[arb] its source:\n%s\n", pp[st]->src);
            l->ok = 0;
            return l;
        }
        if (g_dump) fprintf(stderr, "[arb] %s program %u ->\n%s\n", st ? "fragment" : "vertex", ids[st], glsl);
        glsl_s[st] = glsl;
        memcpy(l->mat[st], t.mat, sizeof t.mat);
        for (int u = 0; u < 16; u++) if (t.tex_target[u] == 5) l->rect_units |= 1u << u;
        l->nmat[st] = t.nmat;
        l->env_hi[st] = t.env_hi;
        l->local_hi[st] = t.local_hi;
        if (st == 0) {
            /* generic attributes at the indices the program named */
            l->prog = l->prog ? l->prog : glCreateProgram();
            for (int a = 1; a < 16; a++) if (t.attrib_used[a]) {
                char nm[16]; snprintf(nm, sizeof nm, "va_%d", a);
                glBindAttribLocation(l->prog, (GLuint)a, nm);
            }
        }
    }
    if (!l->prog) l->prog = glCreateProgram();
    GLint ok = 0;
    struct timespec t_link, t_tr;
    clock_gettime(CLOCK_MONOTONIC, &t_tr);
#ifdef LINDBERGH_GL4ES
    char path[512] = "";
    if (glsl_s[0] && glsl_s[1]) cache_path(path, sizeof path, glsl_s[0], glsl_s[1]);
    if (*path) {
        /* gl4es converts a shader when its source is set and keeps it for the
         * variants it builds later (alpha test and the like): attach the
         * shaders with their source, unbuilt, and load the program */
        for (int st = 0; st < 2; st++) {
            sh[st] = glCreateShader(st ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
            const char *src = glsl_s[st];
            glShaderSource(sh[st], 1, &src, NULL);
            glAttachShader(l->prog, sh[st]);
        }
        clock_gettime(CLOCK_MONOTONIC, &t_link);
        ok = cache_load(l->prog, path);
    }
#endif
    if (!ok) {
        for (int st = 0; st < 2; st++) {
            if (!glsl_s[st]) continue;
            int attached = sh[st] != 0;
            if (attached) {
                glCompileShader(sh[st]);
                GLint c_ok = 0;
                glGetShaderiv(sh[st], GL_COMPILE_STATUS, &c_ok);
                if (!c_ok) {
                    char log[4096] = "";
                    glGetShaderInfoLog(sh[st], sizeof log, NULL, log);
                    fprintf(stderr, "[arb] %s program %u: translated GLSL does not compile:\n%s\n----\n%s\n----\n",
                            st ? "fragment" : "vertex", ids[st], log, glsl_s[st]);
                    glDeleteShader(sh[st]); sh[st] = 0;
                }
            } else sh[st] = compile(st ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, glsl_s[st], st, ids[st]);
            if (!sh[st]) {
                pp[st]->failed = 1; l->ok = 0;
                for (int k = 0; k < 2; k++) { if (sh[k]) glDeleteShader(sh[k]); free(glsl_s[k]); }
                return l;
            }
            if (!attached) glAttachShader(l->prog, sh[st]);
        }
        clock_gettime(CLOCK_MONOTONIC, &t_link);
        glLinkProgram(l->prog);
        glGetProgramiv(l->prog, GL_LINK_STATUS, &ok);
#ifdef LINDBERGH_GL4ES
        if (ok && *path) cache_save(l->prog, path);
#endif
    }
    for (int st = 0; st < 2; st++) { if (sh[st]) glDeleteShader(sh[st]); free(glsl_s[st]); }
    if (getenv("LINDBERGH_LATENCY")) {        /* linking waits on the driver */
        static double total; static unsigned n;
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - g_link_t0.tv_sec) * 1e3 + (t1.tv_nsec - g_link_t0.tv_nsec) / 1e6;
        total += ms; n++;
        double lk = (t1.tv_sec - t_link.tv_sec) * 1e3 + (t1.tv_nsec - t_link.tv_nsec) / 1e6;
        double tr = (t_tr.tv_sec - g_link_t0.tv_sec) * 1e3 + (t_tr.tv_nsec - g_link_t0.tv_nsec) / 1e6;
        if (ms > 5) fprintf(stderr, "[lat] linking v%u/f%u took %.0f ms: translating %.0f, linking %.0f (%u links, %.0f ms so far)\n", vid, fid, ms, tr, lk, n, total);
    }
    if (!ok) {
        char log[2048] = "";
        glGetProgramInfoLog(l->prog, sizeof log, NULL, log);
        fprintf(stderr, "[arb] pair v%u/f%u does not link: %s\n", vid, fid, log);
        l->ok = 0;
        return l;
    }
    GLint cur = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
    glUseProgram(l->prog);
    for (int st = 0; st < 2; st++) {
        const char *P = st ? "f" : "v";
        char nm[32];
        snprintf(nm, sizeof nm, "%s_env", P); l->u_env[st] = glGetUniformLocation(l->prog, nm);
        if (l->u_env[st] >= 0) {
            int want = l->env_hi[st] + 1;
            l->env_hi[st] = array_len(l->prog, nm, want) - 1;
            if (l->env_hi[st] + 1 < want && getenv("LINDBERGH_ARB_ARRAYS"))
                fprintf(stderr, "[arb] pair v%u/f%u: %s %d of %d kept\n", vid, fid, nm, l->env_hi[st] + 1, want);
        }
        snprintf(nm, sizeof nm, "%s_loc", P); l->u_loc[st] = glGetUniformLocation(l->prog, nm);
        if (l->u_loc[st] >= 0) l->local_hi[st] = array_len(l->prog, nm, l->local_hi[st] + 1) - 1;
        for (int m = 0; m < l->nmat[st]; m++) {
            snprintf(nm, sizeof nm, "%s_m%d", P, m);
            l->u_mat[st][m] = glGetUniformLocation(l->prog, nm);
        }
    }
#ifdef LINDBERGH_GL4ES
    l->u_rect = glGetUniformLocation(l->prog, "u_rect_norm");
    l->n_rect = l->u_rect >= 0 ? array_len(l->prog, "u_rect_norm", 16) : 0;
#else
    l->u_rect = glGetUniformLocation(l->prog, "u_rect_scale");
#endif
    l->u_reproj = glGetUniformLocation(l->prog, "u_reproj");
    l->u_gen = glGetUniformLocation(l->prog, "u_gen");
#ifdef LINDBERGH_GL4ES
    if (gl4es_mv_shaders && l->u_reproj >= 0) {
        static int shown;
        if (shown++ < 5) fprintf(stderr, "[arbprog] u_reproj at %d, %d matrices as the driver keeps it\n", l->u_reproj, array_len(l->prog, "u_reproj", 2));
    }
#endif
    static const char *const pre[] = { "s1_", "s2_", "s3_", "sc_", "sr_", "ss_" };
    for (int u = 0; u < 16; u++)
        for (int k = 0; k < 6; k++) {
            char nm[16]; snprintf(nm, sizeof nm, "%s%d", pre[k], u);
            GLint loc = glGetUniformLocation(l->prog, nm);
            if (loc >= 0) glUniform1i(loc, u);
        }
    glUseProgram((GLuint)cur);
    l->ok = 1;
    return l;
}

/* ---- matrices, from the GL stacks ---- */

static void mat_mul(const float *a, const float *b, float *o)   /* column-major a*b */
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            o[c * 4 + r] = s;
        }
}

static int mat_inv(const float *m, float *o)
{
    float inv[16];
    inv[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    float det = m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
    if (det == 0) { memcpy(o, m, 64); return 0; }
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return 1;
}

static void stack_matrix(int which, int index, float *m)
{
    GLint mode = 0, active = 0;
    switch (which) {
    case MAT_MODELVIEW:  glGetFloatv(GL_MODELVIEW_MATRIX, m); return;
    case MAT_PROJECTION: glGetFloatv(GL_PROJECTION_MATRIX, m); return;
    case MAT_MVP: {
        float a[16], b[16];
        glGetFloatv(GL_PROJECTION_MATRIX, a);
        glGetFloatv(GL_MODELVIEW_MATRIX, b);
        mat_mul(a, b, m);
        return;
    }
    case MAT_TEXTURE:
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0 + (GLenum)index);
        glGetFloatv(GL_TEXTURE_MATRIX, m);
        glActiveTexture((GLenum)active);
        return;
    case MAT_PROGRAM:
        glGetIntegerv(GL_MATRIX_MODE, &mode);
        glMatrixMode(GL_MATRIX0_ARB + (GLenum)index);
        glGetFloatv(GL_CURRENT_MATRIX_ARB, m);
        glMatrixMode((GLenum)mode);
        return;
    }
}


void arb_attrib_array(unsigned index, int on)
{
    if (index < 16) g_gen_on[index] = on ? 1.0f : 0.0f;
}

static void upload(Linked *l)
{
    if (l->u_gen >= 0) glUniform1fv(l->u_gen, 16, g_gen_on);
#ifdef LINDBERGH_GL4ES
    if (l->u_reproj >= 0 && gl4es_mv_shaders) {
        float m[32];
        stereo_reproject_view(0, m); stereo_reproject_view(1, m + 16);
        glUniformMatrix4fv(l->u_reproj, 2, GL_FALSE, m);
    } else
#endif
    if (l->u_reproj >= 0) { float m[16]; stereo_reproject(m); glUniformMatrix4fv(l->u_reproj, 1, GL_FALSE, m); }
#ifdef LINDBERGH_GL4ES
    if (l->u_rect >= 0) {
        float rx, ry, norm[16][2];
        stereo_rect_scale(&rx, &ry);
        GLint active = GL_TEXTURE0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        for (int u = 0; u < 16; u++) {
            norm[u][0] = norm[u][1] = 1;
            if (!(l->rect_units & (1u << u))) continue;
            GLint w = 0, h = 0;
            glActiveTexture(GL_TEXTURE0 + u);
            glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE_ARB, 0, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE_ARB, 0, GL_TEXTURE_HEIGHT, &h);
            /* only render targets were drawn larger; a texture the game
             * filled itself (the 256x1 colour table of the final pass) is
             * still its own size, and scaling its lookups washed every
             * colour out */
            GLint bound = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB, &bound);
            int big = stereo_is_render_target((GLuint)bound);
            if (getenv("LINDBERGH_RECT_LOG")) {
                static unsigned seen[256]; static int ns;
                unsigned key = (unsigned)bound * 2u + (unsigned)big;
                int k = 0; while (k < ns && seen[k] != key) k++;
                if (k == ns && ns < 256) { seen[ns++] = key;
                    fprintf(stderr, "[rect] frame %u: unit %d texture %d is %dx%d, %s\n", g_frame, u, bound, w, h,
                            big ? "a render target (scaled)" : "not a render target"); }
            }
            if (w > 0 && h > 0) { norm[u][0] = (big ? rx : 1) / (float)w; norm[u][1] = (big ? ry : 1) / (float)h; }
        }
        glActiveTexture((GLenum)active);
        glUniform2fv(l->u_rect, l->n_rect, &norm[0][0]);
        if (gl4es_mv_trace)
            fprintf(stderr, "[arb] u_rect_norm at %d, %d of them: %g %g / %g %g, error %#x\n", l->u_rect, l->n_rect,
                    norm[0][0], norm[0][1], norm[3][0], norm[3][1], glGetError());
    }
#else
    if (l->u_rect >= 0) {
        /* only render targets were drawn larger; a texture the game filled
         * itself (the 256x1 colour table of the final pass) is its own size,
         * and scaling its lookups washed every colour out */
        float rx, ry, sc[16][2];
        stereo_rect_scale(&rx, &ry);
        GLint active = GL_TEXTURE0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        for (int u = 0; u < 16; u++) {
            sc[u][0] = sc[u][1] = 1;
            if (!(l->rect_units & (1u << u)) || (rx == 1 && ry == 1)) continue;
            GLint bound = 0;
            glActiveTexture(GL_TEXTURE0 + u);
            glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB, &bound);
            if (stereo_is_render_target((GLuint)bound)) { sc[u][0] = rx; sc[u][1] = ry; }
        }
        glActiveTexture((GLenum)active);
        glUniform2fv(l->u_rect, 16, &sc[0][0]);
    }
#endif
    for (int st = 0; st < 2; st++) {
        GLuint id = st ? l->fid : l->vid;
        if (!id) continue;
        if (l->u_env[st] >= 0 && l->up_env[st] != g_env_gen[st]) {
            glUniform4fv(l->u_env[st], l->env_hi[st] + 1, &g_env[st][0][0]);
            l->up_env[st] = g_env_gen[st];
        }
        ArbProg *p = prog_get(st, id, 0);
        if (p && l->u_loc[st] >= 0 && l->up_loc[st] != p->local_gen) {
            glUniform4fv(l->u_loc[st], l->local_hi[st] + 1, &p->local[0][0]);
            l->up_loc[st] = p->local_gen;
        }
        for (int i = 0; i < l->nmat[st]; i++) {
            if (l->u_mat[st][i] < 0) continue;
            float m[16], o[16], rows[16];
            stack_matrix(l->mat[st][i].which, l->mat[st][i].index, m);
            int mod = l->mat[st][i].mod;
            if (mod == 1 || mod == 3) { mat_inv(m, o); memcpy(m, o, 64); }
            /* rows of M (or of its transpose) */
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    rows[r * 4 + c] = (mod == 2 || mod == 3) ? m[r * 4 + c] : m[c * 4 + r];
            glUniform4fv(l->u_mat[st][i], 4, rows);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* the hooks hle_gl_posix.c calls                                            */
/* ------------------------------------------------------------------------ */

static int g_arb_off = -1;      /* LINDBERGH_ARB_GLSL=0 leaves it all to the driver */

int arb_active(void)
{
    if (g_arb_off < 0) { const char *v = getenv("LINDBERGH_ARB_GLSL"); g_arb_off = v && *v == '0'; }
    return !g_arb_off;
}

void arb_bind(GLenum target, GLuint id)
{
    int st = stage_of(target);
    if (st >= 0) g_bound[st] = id;
}

void arb_set_source(GLenum target, const char *src, int len)
{
    int st = stage_of(target);
    if (st < 0 || !src || len <= 0) return;
    ArbProg *p = prog_get(st, g_bound[st], 1);
    if (!p) return;
    free(p->src);
    p->src = strndup(src, (size_t)len);
    p->gen++;
    p->failed = 0;
}

void arb_delete(GLenum target, GLuint id)
{
    (void)target;
    for (int st = 0; st < 2; st++) {
        ArbProg *p = prog_get(st, id, 0);
        if (p) { free(p->src); free(p); g_prog[st][id] = NULL; }
    }
}

void arb_env(GLenum target, GLuint index, const float *v)
{
    int st = stage_of(target);
    if (st < 0 || index >= MAX_ENV) return;
    /* about half of the game's sets repeat the value already there, and a
     * change means the whole array goes to the driver again at the next draw */
    if (!memcmp(g_env[st][index], v, 16)) return;
    memcpy(g_env[st][index], v, 16);
    g_env_gen[st]++;
}

void arb_local(GLenum target, GLuint index, const float *v)
{
    int st = stage_of(target);
    if (st < 0 || index >= MAX_LOCAL) return;
    ArbProg *p = prog_get(st, g_bound[st], 1);
    if (!p || !memcmp(p->local[index], v, 16)) return;
    memcpy(p->local[index], v, 16);
    p->local_gen++;
}

/* glEnable / glDisable / glIsEnabled for the two program targets. Returns 1
 * when the cap is ours. */
int arb_enable(GLenum cap, int on)
{
    int st = stage_of(cap);
    if (st < 0 || !arb_active()) return 0;
    g_enabled[st] = on;
    return 1;
}

int arb_is_enabled(GLenum cap, int *out)
{
    int st = stage_of(cap);
    if (st < 0 || !arb_active()) return 0;
    *out = g_enabled[st];
    return 1;
}

/* glPushAttrib(GL_ENABLE_BIT) saves the enables the driver no longer sees. */
static int g_attr_stack[64][2], g_attr_sp;
void arb_push_attrib(GLbitfield mask)
{
    if (g_attr_sp < 64) {
        g_attr_stack[g_attr_sp][0] = (mask & GL_ENABLE_BIT) ? g_enabled[0] | 2 : -1;
        g_attr_stack[g_attr_sp][1] = (mask & GL_ENABLE_BIT) ? g_enabled[1] | 2 : -1;
    }
    g_attr_sp++;
}
void arb_pop_attrib(void)
{
    if (g_attr_sp <= 0) return;
    g_attr_sp--;
    if (g_attr_sp < 64)
        for (int st = 0; st < 2; st++)
            if (g_attr_stack[g_attr_sp][st] >= 0) g_enabled[st] = g_attr_stack[g_attr_sp][st] & 1;
}

void arb_use_glsl(GLuint prog) { g_game_glsl = prog; }

/* Before every draw: the game's own GLSL program wins, as it does in GL;
 * otherwise the enabled assembly stages, translated. */
#ifdef LINDBERGH_GL4ES
/* Units given a rectangle enable for the last program draw, undone at the
 * next draw - after glEnd for glBegin. */
static unsigned g_rect_forced;

static void rect_enables(unsigned want)
{
    if (want == g_rect_forced) return;
    GLint active = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    for (int u = 0; u < 16; u++) {
        unsigned bit = 1u << u;
        if ((want ^ g_rect_forced) & bit) {
            glActiveTexture(GL_TEXTURE0 + u);
            if (want & bit) { if (glIsEnabled(GL_TEXTURE_RECTANGLE_ARB)) want &= ~bit; else glEnable(GL_TEXTURE_RECTANGLE_ARB); }
            else glDisable(GL_TEXTURE_RECTANGLE_ARB);
        }
    }
    glActiveTexture((GLenum)active);
    g_rect_forced = want;
}

/* The game set a rectangle enable itself: it is the game's now. */
void arb_rect_enable_seen(void)
{
    GLint active = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    g_rect_forced &= ~(1u << ((unsigned)active - GL_TEXTURE0));
}
#endif

double g_arb_draw_ns;                   /* LINDBERGH_GLPROF: time in arb_before_draw */
static void before_draw(void);
void arb_before_draw(void)
{
    extern int stereo_glprof_on(void);
    if (!stereo_glprof_on()) { before_draw(); return; }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    before_draw();
    clock_gettime(CLOCK_MONOTONIC, &b);
    g_arb_draw_ns += (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
}

static void before_draw(void)
{
#ifdef LINDBERGH_GL4ES
    rect_enables(0);
#endif
    if (!arb_active()) return;
    if (g_game_glsl) { glUseProgram(g_game_glsl); return; }
    GLuint vid = g_enabled[0] ? g_bound[0] : 0;
    GLuint fid = g_enabled[1] ? g_bound[1] : 0;
    if (!vid && !fid) { glUseProgram(0); glDisable(GL_VERTEX_PROGRAM_ARB); glDisable(GL_FRAGMENT_PROGRAM_ARB); return; }
    {   /* LINDBERGH_DUMP_VP=<id>: on the first draws with that vertex program,
         * report the pipeline state and save texture unit 0 as a .ppm/.pgm
         * pair (colour, alpha) in the working directory. */
        static long want = -2; static int shots;
        if (want == -2) { const char *v = getenv("LINDBERGH_DUMP_VP"); want = v ? strtol(v, NULL, 0) : -1; }
        if (want >= 0 && (long)vid == want && shots < 3) {
            GLint tex = 0, w = 0, h = 0, ifmt = 0, bs = 0, bd = 0, at = 0, bl = 0, tenv = 0, unit = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &unit); glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
            glGetIntegerv(GL_BLEND_SRC, &bs); glGetIntegerv(GL_BLEND_DST, &bd); bl = glIsEnabled(GL_BLEND);
            at = glIsEnabled(GL_ALPHA_TEST); glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &tenv);
            {
                GLint mf = 0, maxl = 0, base = 0, nl = 0, lw = 1;
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mf);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxl);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base);
                for (nl = 0; nl < 16 && lw > 0; nl++) { lw = 0; glGetTexLevelParameteriv(GL_TEXTURE_2D, nl, GL_TEXTURE_WIDTH, &lw); }
                for (int e = 0; e < 48; e += 4)
                    fprintf(stderr, "[dumpvp]   env %2d: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", e,
                            g_env[0][e][0], g_env[0][e][1], g_env[0][e][2], g_env[0][e][3],
                            g_env[0][e+1][0], g_env[0][e+1][1], g_env[0][e+1][2], g_env[0][e+1][3],
                            g_env[0][e+2][0], g_env[0][e+2][1], g_env[0][e+2][2], g_env[0][e+2][3],
                            g_env[0][e+3][0], g_env[0][e+3][1], g_env[0][e+3][2], g_env[0][e+3][3]);
                fprintf(stderr, "[dumpvp]   tex0 min filter %#x base %d max level %d, levels present %d\n", mf, base, maxl, nl - 1);
            }
            fprintf(stderr, "[dumpvp] vp %u fp %u (enabled %d/%d) tex0 %d %dx%d ifmt %#x blend %d %#x/%#x alphatest %d texenv %#x 2D %d\n",
                    vid, fid, g_enabled[0], g_enabled[1], tex, w, h, ifmt, bl, bs, bd, at, tenv, glIsEnabled(GL_TEXTURE_2D));
            if (w > 0 && h > 0 && w <= 2048 && h <= 2048) {
                unsigned char *px = malloc((size_t)w * h * 4);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                /* LINDBERGH_DUMP_DIR: where the images go (not the game's folder) */
                const char *dir = getenv("LINDBERGH_DUMP_DIR");
                if (!dir || !*dir) dir = ".";
                char nm[512]; snprintf(nm, sizeof nm, "%s/dumpvp_%d.ppm", dir, shots);
                FILE *f = fopen(nm, "wb"); fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (int i = 0; i < w * h; i++) fwrite(px + 4 * i, 1, 3, f);
                fclose(f);
                snprintf(nm, sizeof nm, "%s/dumpvp_%d_a.pgm", dir, shots);
                f = fopen(nm, "wb"); fprintf(f, "P5\n%d %d\n255\n", w, h);
                for (int i = 0; i < w * h; i++) fwrite(px + 4 * i + 3, 1, 1, f);
                fclose(f); free(px);
            }
            {
                GLint cu = 0; glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &cu);
                glClientActiveTexture(GL_TEXTURE0);
                GLint te = glIsEnabled(GL_TEXTURE_COORD_ARRAY), ts = 0, tt = 0, tst = 0; GLvoid *tp = 0;
                glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE, &ts); glGetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE, &tt);
                glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE, &tst); glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &tp);
                glClientActiveTexture((GLenum)cu);
                GLint ve = glIsEnabled(GL_VERTEX_ARRAY), vs = 0, vt = 0, vst = 0; GLvoid *vp = 0;
                glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &vs); glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &vt);
                glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &vst); glGetPointerv(GL_VERTEX_ARRAY_POINTER, &vp);
                GLint ce = glIsEnabled(GL_COLOR_ARRAY), a7 = 0, a7s = 0, a7t = 0, a7st = 0, vb = 0; GLvoid *a7p = 0;
                glGetVertexAttribiv(7, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a7); glGetVertexAttribiv(7, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a7s);
                glGetVertexAttribiv(7, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a7t); glGetVertexAttribiv(7, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a7st);
                glGetVertexAttribPointerv(7, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a7p); glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vb);
                fprintf(stderr, "[dumpvp]   vbo %d | vertex %d: %d x %#x stride %d @%p | tex0 %d: %d x %#x stride %d @%p | color %d | attr7 %d: %d x %#x stride %d @%p | client unit %#x\n",
                        vb, ve, vs, vt, vst, vp, te, ts, tt, tst, tp, ce, a7, a7s, a7t, a7st, a7p, cu);
                if (vb) {
                    unsigned char vbuf[28 * 8];
                    glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof vbuf, vbuf);
                    for (int k = 0; k < 8; k++) {
                        float f[7]; memcpy(f, vbuf + 28 * k, 28);
                        short mi; memcpy(&mi, vbuf + 28 * k + 24, 2);
                        fprintf(stderr, "[dumpvp]   v%d pos %.3f %.3f %.3f col %08x uv %.4f %.4f idx %d\n", k, f[0], f[1], f[2],
                                *(unsigned *)(vbuf + 28 * k + 12), f[4], f[5], mi);
                    }
                }
            }
            {
                char line[256]; int at = 0;
                for (int a = 0; a < 16; a++) {
                    GLint e = 0, sz = 0, st = 0, ty = 0; GLvoid *pt = 0;
                    glGetVertexAttribiv((GLuint)a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &e);
                    if (!e && !g_gen_on[a]) continue;
                    glGetVertexAttribiv((GLuint)a, GL_VERTEX_ATTRIB_ARRAY_SIZE, &sz);
                    glGetVertexAttribiv((GLuint)a, GL_VERTEX_ATTRIB_ARRAY_TYPE, &ty);
                    glGetVertexAttribiv((GLuint)a, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &st);
                    glGetVertexAttribPointerv((GLuint)a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pt);
                    at += snprintf(line + at, sizeof line - at, " a%d:%d/%d %dx%#x+%p", a, e, (int)g_gen_on[a], sz, ty, pt);
                    if (at > 200) break;
                }
                fprintf(stderr, "[dumpvp]   generic arrays (gl/ours):%s\n", line);
                for (int a = 8; a <= 9; a++) {
                    GLint e = 0, st = 0, ty = 0, buf = 0; GLvoid *pt = 0;
                    glGetVertexAttribiv((GLuint)a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &e);
                    glGetVertexAttribiv((GLuint)a, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &st);
                    glGetVertexAttribiv((GLuint)a, GL_VERTEX_ATTRIB_ARRAY_TYPE, &ty);
                    glGetVertexAttribiv((GLuint)a, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
                    glGetVertexAttribPointerv((GLuint)a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pt);
                    if (!e || !buf || ty != GL_FLOAT) continue;
                    GLint was = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &was);
                    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)buf);
                    for (int k = 0; k < 4; k++) {
                        float uv[2] = {0, 0};
                        glGetBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(uintptr_t)pt + (GLintptr)st * k, 8, uv);
                        fprintf(stderr, "[dumpvp]   attrib %d (buffer %d) vertex %d: %g %g\n", a, buf, k, uv[0], uv[1]);
                    }
                    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)was);
                }
            }
            glActiveTexture((GLenum)unit);
            shots++;
        }
    }
    Linked *l = link_pair(vid, fid);
    {   /* LINDBERGH_GL_TRACE=N (N > 1): each draw of frame N with its programs */
        extern unsigned g_frame;
        static long tf = -2;
        if (tf == -2) { const char *v = getenv("LINDBERGH_GL_TRACE"); tf = v ? atol(v) : 0; }
        /* LINDBERGH_DRAWLOG=1: every 500th frame's draws, whatever it shows */
        static int dl = -1;
        if (dl < 0) { const char *v = getenv("LINDBERGH_DRAWLOG"); dl = v && *v == '1'; }
        if ((tf > 1 && g_frame == (unsigned)tf) || (dl && g_frame % 500 == 0)) {
            GLint fb = 0, vp[4] = {0}; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb); glGetIntegerv(GL_VIEWPORT, vp);
            fprintf(stderr, "[draw] %u v%u f%u fb %d viewport %dx%d depth %d %s\n", g_frame, vid, fid, fb, vp[2], vp[3],
                    glIsEnabled(GL_DEPTH_TEST), l && l->ok ? "" : "(not linked)");
        }
    }
    static int probed;
    const char *dvp = getenv("LINDBERGH_DUMP_VP");
    if (dvp && (long)vid == strtol(dvp, NULL, 0) && probed < 3 && l && l->ok) {
        probed++;
        GLint loc8 = glGetAttribLocation(l->prog, "va_8"), loc3 = glGetAttribLocation(l->prog, "va_3");
        GLfloat ug[16] = {0};
        glUseProgram(l->prog); upload(l);
        GLint l8 = glGetUniformLocation(l->prog, "u_gen[8]"), l3 = glGetUniformLocation(l->prog, "u_gen[3]");
        if (l8 >= 0) glGetUniformfv(l->prog, l8, &ug[8]);
        if (l3 >= 0) glGetUniformfv(l->prog, l3, &ug[3]);
        fprintf(stderr, "[dumpvp]   va_8 at %d, va_3 at %d, u_gen loc %d: [3]=%g [8]=%g\n", loc8, loc3, l->u_gen, ug[3], ug[8]);
    }
    if (l && l->ok) {
        glDisable(GL_VERTEX_PROGRAM_ARB);
        glDisable(GL_FRAGMENT_PROGRAM_ARB);
        glUseProgram(l->prog);
        upload(l);
#ifdef LINDBERGH_GL4ES
        rect_enables(l->rect_units);
#endif
        return;
    }
    /* not translatable: the driver's own assembly path, as before */
    glUseProgram(0);
    if (vid) glEnable(GL_VERTEX_PROGRAM_ARB); else glDisable(GL_VERTEX_PROGRAM_ARB);
    if (fid) glEnable(GL_FRAGMENT_PROGRAM_ARB); else glDisable(GL_FRAGMENT_PROGRAM_ARB);
}

GLuint arb_bound(GLenum target)
{
    int st = stage_of(target);
    return st >= 0 ? g_bound[st] : 0;
}

const char *arb_get_source(GLenum target, GLuint id)
{
    int st = stage_of(target);
    ArbProg *p = st >= 0 ? prog_get(st, id, 0) : NULL;
    return p ? p->src : NULL;
}
