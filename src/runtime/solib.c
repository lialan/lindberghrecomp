/*
 * solib.c - dlopen and dlsym for the libraries tools/recomp/solib.py lifted.
 *
 * Their images were linked and relocated at lift time for fixed addresses in
 * the guest window, so loading one is a copy and running its constructors.
 * All of them load together on the first dlopen of any: they were linked
 * against each other.
 */

#include <stdio.h>
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
    push32(c, 0xDEADBEEFu);
    dispatch(c, fn);
    return 1;
}
#else
int solib_dlopen(CPU *c, const char *path) { (void)c; (void)path; return 0; }
uint32_t solib_dlsym(const char *name) { (void)name; return 0; }
uint32_t solib_export(const char *name) { (void)name; return 0; }
int solib_call(CPU *c, const char *name) { (void)c; (void)name; return 0; }
#endif
