/*
 * hle_cg.c - NVIDIA Cg, answered from the disc.
 *
 * The game compiles its shaders through Cg at startup: hand cgCreateProgram
 * the Cg source, ask cgGetProgramString for the result, feed that to
 * glProgramStringARB. Implementing that honestly would mean shipping a Cg
 * compiler.
 *
 * It is not necessary, because the answers are already on the disc. Every
 * shader is there twice - shader/Cg/vs/vs.cg next to shader/Cg/vs/vs.asm_gl,
 * the second being the first as cgc 1.4 compiled it for profile vp40 in 2005.
 * So this matches the source it is given against the .cg files it can find and
 * returns the .asm_gl beside it: the same text the real Cg runtime would have
 * produced, produced by the same compiler, just earlier.
 *
 * If a source does not match anything, that is reported rather than papered
 * over - a silently empty shader draws nothing and says nothing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define strdup _strdup
#else
#include <dirent.h>
#include <sys/stat.h>
#ifndef MAX_PATH
#define MAX_PATH 1024
#endif
#endif

#include "lindbergh_rt.h"

#define CG_TOKEN_CTX  0x43470001u      /* "CG" */
/* Initial D 4 compiles past 500 programs during init, so 512 ran out and the
 * late shaders (dof among them) went unindexed. Running out is reported below. */
#define MAX_SHADERS   2048

typedef struct {
    char *path;         /* which file this came from, for diagnosis */
    char *body;         /* the .cg text, comments and whitespace stripped */
    char *defines;      /* the #define block the .asm_gl records, stripped */
    char *compiled;     /* the .asm_gl itself */
} Shader;

/* Strip comments and all whitespace. Two texts that differ only in formatting
 * are the same shader, and the engine hands over a PREPROCESSED source - its
 * includes already pasted in and its comments likely gone - so a byte compare
 * against the file on disc never matches. Reducing both to bare code makes the
 * file's own body findable inside the preprocessed whole. */
static char *strip_code(const char *src, size_t len)
{
    if (len == 0) len = strlen(src);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '/') {
            while (i < len && src[i] != '\n') i++;
            continue;
        }
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i++;
            continue;
        }
        if ((unsigned char)src[i] <= ' ') continue;
        out[o++] = src[i];
    }
    out[o] = 0;
    return out;
}

static Shader g_sh[MAX_SHADERS];
static int    g_sh_n;
static int    g_indexed;
static int    g_match_body;      /* matched on the shader's own body */
static int    g_match_defines;   /* matched on a defines set unique to one shader */

static char *read_all(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = (char *)malloc((size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) == (size_t)n) p[n] = 0;
    else { free(p); p = NULL; }
    fclose(f);
    if (p && len_out) *len_out = (size_t)n;
    return p;
}

/* Walk a directory tree for *.cg, and for each one remember the .asm_gl that
 * sits beside it. Depth-limited because the tree is shallow by construction
 * (shader/Cg/{vs,ps,inc}) and a runaway walk during startup is worse than a
 * missed shader. */
/* A shader's own code: everything after its last #include.
 *
 * Includes expand in place, so the preprocessed source the engine hands over
 * is [expanded includes][this file's own code]. That tail is what identifies
 * the shader - the part before it is shared by all of them, which is why
 * matching on the whole file found the wrong program nearly every time.
 *
 * Taken from the text already stripped of comments and whitespace: an
 * #include inside a comment (Let's Go Jungle keeps a commented-out DirectX one
 * at the end of its shaders) is not one, and cutting there left the rest of
 * the comment in the body, which then matched nothing. Returns a copy. */
static char *own_body(const char *src, size_t len)
{
    char *all = strip_code(src, len);
    if (!all) return NULL;
    char *last = NULL;
    for (char *p = strstr(all, "#include"); p; p = strstr(p + 1, "#include")) last = p;
    if (!last) return all;
    char *q = last + 8;
    if (*q == '"' || *q == '<') {
        char close = *q == '"' ? '"' : '>';
        char *r = strchr(q + 1, close);
        q = r ? r + 1 : q;
    }
    char *body = strdup(q);
    free(all);
    return body;
}

/* cgc stamps its invocation into the output:
 *
 *   # command line args: -q -profile vp40 -entry main #define MODE_GL (1) ...
 *
 * Everything from the first #define on is the variant key - the same block the
 * engine passes to cgCreateProgram as its args. Stripped of whitespace so the
 * two spellings compare equal. */
static char *asm_defines(const char *compiled)
{
    const char *line = strstr(compiled, "command line args:");
    if (!line) return strip_code("", 0);
    const char *hash = strstr(line, "#define");
    if (!hash) return strip_code("", 0);

    /* The block runs to the end of the comment run - every following line that
     * still begins with a '#' or whitespace before one. */
    const char *end = hash;
    while (*end) {
        const char *nl = strchr(end, '\n');
        if (!nl) { end += strlen(end); break; }
        const char *peek = nl + 1;
        while (*peek == ' ' || *peek == '	') peek++;
        if (*peek != '#') { end = nl; break; }
        end = nl + 1;
    }
    return strip_code(hash, (size_t)(end - hash));
}

/* The arguments the engine passes, reduced the same way. */
static char *args_defines(CPU *c, uint32_t argv)
{
    char buf[4096];
    size_t n = 0;
    for (int k = 0; k < 32 && argv; k++) {
        uint32_t p = rd32(argv + 4u * (uint32_t)k);
        if (!p) break;
        const char *a = (const char *)GP(p);
        size_t l = strlen(a);
        if (n + l >= sizeof buf) break;
        memcpy(buf + n, a, l);
        n += l;
    }
    buf[n] = 0;
    return strip_code(buf, n);
}

static void index_dir(const char *dir, int depth)
{
    if (depth > 4 || g_sh_n >= MAX_SHADERS) return;

    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s/*", dir);

#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (depth == 0) fprintf(stderr, "[cg] cannot open %s\n", dir);
        return;
    }
    do {
        const char *name = fd.cFileName;
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    (void)pat;
    DIR *dp = opendir(dir);
    if (!dp) {
        if (depth == 0) fprintf(stderr, "[cg] cannot open %s\n", dir);
        return;
    }
    struct dirent *de;
    while ((de = readdir(dp))) {
        const char *name = de->d_name;
        int is_dir;
        {
            char p2[MAX_PATH]; struct stat st;
            snprintf(p2, sizeof p2, "%s/%s", dir, name);
            is_dir = stat(p2, &st) == 0 && S_ISDIR(st.st_mode);
        }
#endif
        if (name[0] == '.') continue;

        char full[MAX_PATH];
        snprintf(full, sizeof full, "%s/%s", dir, name);

        if (is_dir) {
            index_dir(full, depth + 1);
            continue;
        }
        size_t n = strlen(name);
        if (n < 4 || strcmp(name + n - 3, ".cg") != 0) continue;

        char asm_path[MAX_PATH];
        snprintf(asm_path, sizeof asm_path, "%.*s.asm_gl",
                 (int)(strlen(full) - 3), full);

        size_t slen = 0, clen = 0;
        char *src = read_all(full, &slen);
        char *cmp = read_all(asm_path, &clen);
        if (src && cmp && g_sh_n < MAX_SHADERS) {
            g_sh[g_sh_n].path     = strdup(full);
            g_sh[g_sh_n].body     = own_body(src, slen);
            g_sh[g_sh_n].defines  = asm_defines(cmp);
            g_sh[g_sh_n].compiled = cmp;
            g_sh_n++;
            free(src);                 /* only the stripped body is kept */
        } else {
            free(src);
            free(cmp);
        }
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(dp);
#endif
}

static void index_shaders(void)
{
    if (g_indexed) return;
    g_indexed = 1;

    const char *tea = getenv("TEA_DIR");
    if (!tea || !*tea) {
        fprintf(stderr, "[cg] TEA_DIR is unset, so there is nowhere to look for\n"
                        "     precompiled shaders. Set it to the game directory.\n");
        return;
    }
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s/shader", tea);
    index_dir(dir, 0);
    snprintf(dir, sizeof dir, "%s/extraShader", tea);
    index_dir(dir, 0);
    fprintf(stderr, "[cg] indexed %d precompiled shaders\n", g_sh_n);
    if (g_sh_n >= MAX_SHADERS)
        fprintf(stderr, "[cg] shader table full at %d - the rest of the disc is "
                        "unindexed; raise MAX_SHADERS\n", MAX_SHADERS);
    {
        /* How discriminating is the defines key actually? If most shaders
         * share one value it is not a key at all, and the match degenerates to
         * "first body that appears" - which for shaders sharing includes picks
         * the wrong program and transforms every vertex off screen. */
        int uniq = 0;
        for (int i = 0; i < g_sh_n; i++) {
            int seen = 0;
            for (int j = 0; j < i; j++)
                if (g_sh[j].defines && g_sh[i].defines &&
                    strcmp(g_sh[i].defines, g_sh[j].defines) == 0) { seen = 1; break; }
            if (!seen) uniq++;
        }
        fprintf(stderr, "[cg] %d distinct define sets across %d shaders\n", uniq, g_sh_n);
        for (int i = 0; i < 2 && i < g_sh_n; i++)
            fprintf(stderr, "[cg]   defines[%d] = %.90s\n", i,
                    g_sh[i].defines ? g_sh[i].defines : "(none)");
    }
}


static void h_cgCreateContext(CPU *c)  { index_shaders(); RET(CG_TOKEN_CTX); }
static void h_cgIsContext(CPU *c)      { RET(A32(0) == CG_TOKEN_CTX); }
static void h_cgDestroyContext(CPU *c)
{
    (void)c;
    fprintf(stderr, "[cg] matches: %d by body+defines, %d by defines alone\n",
            g_match_body, g_match_defines);
}
static void h_cgSetErrorCallback(CPU *c) { (void)c; }
static void h_cgGetError(CPU *c)       { (void)c; RET(0); }           /* CG_NO_ERROR */
static void h_cgGetErrorString(CPU *c) { RET(guest_strdup_static("")); }
static void h_cgDestroyProgram(CPU *c) { (void)c; }

/* Every profile the game asks about is one cgc already compiled for, and the
 * host driver still supports the ARB programs that came out. */
static void h_cgGLIsProfileSupported(CPU *c) { RET(1); }

/* cgCreateProgram(ctx, type, source, profile, entry, args). The returned
 * handle is an index into the shader table, offset so that zero stays the
 * failure value Cg uses. */
static void h_cgCreateProgram(CPU *c)
{
    index_shaders();
    const char *src = ASTR(2);
    if (src) {
        /* Two things identify a program: which shader it is, and which of its
         * #define permutations was asked for. The source says the first - the
         * file's own body survives preprocessing and can be found inside the
         * pasted-together whole - and the args say the second. Either alone is
         * ambiguous: many shaders share a define set, and one shader has many. */
        char *want_defs = args_defines(c, A32(5));
        char *whole = strip_code(src, strlen(src));
        int hit = -1;
        size_t hit_len = 0;
        if (want_defs && whole) {
            /* The longest body that ends the source is the shader: a short
             * one can be the tail of a longer one. Where two files share a
             * body, the one compiled with the define set asked for wins. */
            size_t wl = strlen(whole);
            for (int i = 0; i < g_sh_n; i++) {
                if (!g_sh[i].body) continue;
                size_t bl = strlen(g_sh[i].body);
                if (!bl || bl > wl || memcmp(whole + wl - bl, g_sh[i].body, bl) != 0) continue;
                int defs = g_sh[i].defines && strcmp(g_sh[i].defines, want_defs) == 0;
                int hit_defs = hit >= 0 && g_sh[hit].defines && strcmp(g_sh[hit].defines, want_defs) == 0;
                if (bl > hit_len || (bl == hit_len && defs && !hit_defs)) { hit = i; hit_len = bl; }
            }
            if (hit >= 0) {
                g_match_body++;
                if (getenv("LINDBERGH_FBSTATS") && g_match_body <= 14)
                    fprintf(stderr, "[cg] match %2d -> %s\n", g_match_body,
                            g_sh[hit].path ? g_sh[hit].path : "?");
            }
            /* A define set that matches exactly one shader needs no second
             * opinion - and preprocessing can rewrite a body past recognition. */
            if (hit < 0) {
                int only = -1, seen = 0;
                for (int i = 0; i < g_sh_n; i++)
                    if (g_sh[i].defines && strcmp(g_sh[i].defines, want_defs) == 0) {
                        only = i; seen++;
                    }
                if (seen == 1) { hit = only; g_match_defines++; }
            }
        }
        free(want_defs);
        free(whole);
        if (hit >= 0) {
            static int reported;
            if (!reported && (g_match_body + g_match_defines) == 50) {
                reported = 1;
                fprintf(stderr, "[cg] first 50 matches: %d by body, %d by defines alone\n",
                        g_match_body, g_match_defines);
            }
            RET(hit + 1); return;
        }
    }
    fprintf(stderr, "[cg] no precompiled match for a %zu-byte shader source\n",
            src ? strlen(src) : 0);
    {   /* LINDBERGH_CG_DUMP=dir: write each unmatched source there */
        static int n;
        const char *d = getenv("LINDBERGH_CG_DUMP");
        if (d && *d && src) {
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s/unmatched_%03d.cg", d, n++);
            FILE *f = fopen(path, "wb");
            if (f) { fputs(src, f); fclose(f); }
        }
    }
    RET(0);
}

/* cgGetProgramString(program, string_type) - the game wants the compiled
 * output, which is the .asm_gl read at index time. */
static void h_cgGetProgramString(CPU *c)
{
    int i = (int)A32(0) - 1;
    /* copied into guest memory once per program: the game may ask again */
    static uint32_t copy[MAX_SHADERS + 1];
    if (i < 0 || i >= g_sh_n) i = MAX_SHADERS;
    if (!copy[i]) copy[i] = guest_strdup_static(i < MAX_SHADERS ? g_sh[i].compiled : "");
    RET(copy[i]);
}

void hle_register_cg(void)
{
    hle_bind("cgCreateContext", h_cgCreateContext);
    hle_bind("cgIsContext", h_cgIsContext);
    hle_bind("cgDestroyContext", h_cgDestroyContext);
    hle_bind("cgSetErrorCallback", h_cgSetErrorCallback);
    hle_bind("cgGetError", h_cgGetError);
    hle_bind("cgGetErrorString", h_cgGetErrorString);
    hle_bind("cgGLIsProfileSupported", h_cgGLIsProfileSupported);
    hle_bind("cgCreateProgram", h_cgCreateProgram);
    hle_bind("cgGetProgramString", h_cgGetProgramString);
    hle_bind("cgDestroyProgram", h_cgDestroyProgram);
}
