/*
 * solib.c - dlopen and dlsym for the libraries tools/recomp/solib.py lifted.
 *
 * Their images were linked and relocated at lift time for fixed addresses in
 * the guest window, so loading one is a copy and running its constructors.
 * All of them load together on the first dlopen of any: they were linked
 * against each other.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lindbergh_rt.h"

#if __has_include("recomp_solib.h")
#include "recomp_solib.h"
#include "solib.h"

static int g_loaded;

static void load_all(CPU *c)
{
    if (g_loaded) return;
    g_loaded = 1;
    memset(G2H(SOLIB_DATA_BASE), 0, SOLIB_DATA_SIZE);
#ifdef SOLIB_DATA
    {   /* data imports: what the runtime has for them, else zero */
#define FILL(name, addr) { uint32_t v; if (hle_libc_data_import(name, &v)) wr32(addr, v); \
                           else fprintf(stderr, "[solib] data import %s left zero\n", name); }
        SOLIB_DATA(FILL)
#undef FILL
    }
#endif
    for (int i = 0; i < g_solib_count; i++) {
        const SolibImage *im = &g_solib_images[i];
        memcpy(G2H(im->base), im->image, im->size);
        fprintf(stderr, "[solib] %s loaded at %#010x (%u KB)\n", im->name, im->base, im->size >> 10);
    }
    /* constructors, dependencies first (the order solib.py was given) */
    for (int i = 0; i < g_solib_count; i++) {
        const SolibImage *im = &g_solib_images[i];
        if (im->init) guest_call(c, im->init, NULL, 0);
        for (int k = 0; k < im->init_count; k++) {
            uint32_t fn = rd32(im->init_array + 4u * (uint32_t)k);
            if (fn && fn != 0xFFFFFFFFu) guest_call(c, fn, NULL, 0);
        }
    }
}

int solib_dlopen(CPU *c, const char *path)
{
    if (!path) return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int i = 0; i < g_solib_count; i++)
        if (!strcmp(g_solib_images[i].name, base)) { load_all(c); return 1; }
    return 0;
}

uint32_t solib_dlsym(const char *name)
{
    if (!g_loaded || !name) return 0;
    return solib_export(name);
}

uint32_t solib_export(const char *name)
{
    if (!name) return 0;
#define EXP(n, a) if (!strcmp(name, n)) return a;
    SOLIB_EXPORTS(EXP)
#undef EXP
    return 0;
}

/* A main executable may link a lifted library through its PLT, without
 * ever calling dlopen. HLE has already consumed the caller's return slot;
 * restore one for the lifted callee, keeping its original arguments. */
int solib_call(CPU *c, const char *name)
{
    uint32_t fn = solib_export(name);
    if (!fn) return 0;
    CPU saved = *c;
    load_all(c);
    *c = saved;
    /* LINDBERGH_SOLIB_TRACE=1: each call into a lifted library, its first
     * six stack words, a string's start where one points to text, and what
     * it returned in eax */
    static int trace = -1;
    if (trace < 0) { const char *v = getenv("LINDBERGH_SOLIB_TRACE"); trace = v && *v == '1'; }
    uint32_t at = c->esp;
    if (trace) {
        fprintf(stderr, "[solib] -> %s(", name);
        for (int i = 0; i < 6; i++) {
            uint32_t a = rd32(at + 4u * (uint32_t)i);
            fprintf(stderr, "%s%#x", i ? ", " : "", a);
            if ((a >= 0x08048000u && a < 0x0A000000u) || (a >= 0x20000000u && a < 0xB4400000u)) {
                const unsigned char *s = (const unsigned char *)G2H(a);
                if (s[0] >= ' ' && s[0] < 0x7F && s[1] >= ' ' && s[1] < 0x7F)
                    fprintf(stderr, " \"%.24s\"", (const char *)s);
            }
        }
        fprintf(stderr, ")\n");
        if (!strcmp(name, "cgCreateProgram")) {      /* its compiler options, a NULL-ended list */
            uint32_t args = rd32(at + 20);
            for (int i = 0; args && i < 80; i++) {
                uint32_t s = rd32(args + 4u * (uint32_t)i);
                fprintf(stderr, "[solib]    args[%d] = %#x%s%s%s\n", i, s, s ? " \"" : "",
                        s ? (const char *)G2H(s) : "", s ? "\"" : "");
                if (!s) break;
            }
        }
    }
    push32(c, 0xDEADBEEFu);
    dispatch(c, fn);
    if (trace) fprintf(stderr, "[solib] <- %s = %#x\n", name, c->eax);
    if (trace && !strcmp(name, "cgCreateProgram") && !c->eax) {   /* what the compiler said */
        CPU k = *c;
        uint32_t ctx = rd32(at), fn = solib_export("cgGetLastListing");
        uint32_t s = fn ? guest_call(&k, fn, &ctx, 1) : 0;
        fprintf(stderr, "[solib]    profile %#x, listing:\n%s\n", rd32(at + 12), s ? (const char *)G2H(s) : "(none)");
        uint32_t src = rd32(at + 8);
        if (src) fprintf(stderr, "[solib]    source:\n%.3000s\n", (const char *)G2H(src));
    }
    if (trace && !strcmp(name, "cgCreateContext")) {   /* is the profile table there? */
        CPU k = *c;
        uint32_t s = guest_strdup_static("vp40");
        uint32_t gp = solib_export("cgGetProfile"), gs = solib_export("cgGetProfileString");
        uint32_t id = gp ? guest_call(&k, gp, &s, 1) : 0;
        uint32_t vp = 0x1b59, str = gs ? guest_call(&k, gs, &vp, 1) : 0;
        fprintf(stderr, "[solib]    cgGetProfile(\"vp40\") = %#x, cgGetProfileString(0x1b59) = %#x %s\n",
                id, str, str ? (const char *)G2H(str) : "(null)");
    }
    return 1;
}
#else
int solib_dlopen(CPU *c, const char *path) { (void)c; (void)path; return 0; }
uint32_t solib_dlsym(const char *name) { (void)name; return 0; }
uint32_t solib_export(const char *name) { (void)name; return 0; }
int solib_call(CPU *c, const char *name) { (void)c; (void)name; return 0; }
#endif
