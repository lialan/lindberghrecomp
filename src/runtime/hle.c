/*
 * hle.c - the shared libraries the board provided.
 *
 * A Lindbergh game is dynamically linked: its calls to libc, libGL, libSDL,
 * ALSA and SEGA's own libsegaapi/libposixdrv all leave through the PLT, and
 * the lifter turns each of those into hle_call(c, HLE_<name>). There is no
 * ld.so here and no .so to load, so this is where those calls end up.
 *
 * An id with no implementation aborts naming itself. That is deliberate: a
 * silent no-op turns a missing import into a graphical glitch three hours
 * later, and the name printed here is the next thing to write.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lindbergh_rt.h"

#define HLE_NAME(id, name) name,
static const char *const g_names[] = { HLE_IMPORTS(HLE_NAME) SOLIB_IMPORTS(HLE_NAME) };
#undef HLE_NAME

const char *hle_name(HleId id)
{
    return (unsigned)id < HLE_COUNT ? g_names[id] : "<bad id>";
}

HleHandler g_hle_handlers[HLE_COUNT];
GlHook g_gl_hook;
unsigned char g_gl_ids[HLE_COUNT];

int hle_bind(const char *name, HleHandler fn)
{
    for (unsigned i = 0; i < HLE_COUNT; i++)
        if (strcmp(g_names[i], name) == 0) { g_hle_handlers[i] = fn; return 1; }
    return 0;   /* this game does not import it - nothing to bind, not a fault */
}

void hle_register_all(void)
{
    hle_register_libc();
    hle_register_libc2();
    hle_register_io();
    hle_register_libc3();
    hle_register_pthread();
    hle_register_window();
    hle_register_vidmode();
    hle_register_matrix();
    hle_register_console();
    hle_register_sega();
    hle_register_glut();
    hle_register_cxx();
    hle_register_dir();
    hle_register_scan();
    hle_register_gl();
    hle_register_cg();
    hle_register_zlib();
    hle_register_openal();
    hle_register_libc4();
    hle_register_segaapi();
#ifndef _WIN32
    hle_register_glx_posix();
#endif

    /* LINDBERGH_HLE_UNBOUND=1: name every import that still has no body, then
     * exit. The static half of the permissive survey: it needs no run, so it
     * also sees imports the start-up path never reaches. */
    const char *u = getenv("LINDBERGH_HLE_UNBOUND");
    if (u && *u == '1') {
        int n = 0;
        for (int i = 0; i < HLE_COUNT; i++)
            if (!g_hle_handlers[i]) { fprintf(stderr, "[unbound] %s\n", hle_name((HleId)i)); n++; }
        fprintf(stderr, "[unbound] %d of %d imports have no body\n", n, (int)HLE_COUNT);
        exit(0);
    }
    { void gl_arg_trace_install(void); gl_arg_trace_install(); }
}

/* Survey mode. Aborting on the first unimplemented import is the right default
 * - it stops at the first place the game went somewhere we cannot follow - but
 * during bring-up it means one rebuild per import, and a rebuild here is 80
 * translation units. With LINDBERGH_HLE_PERMISSIVE=1 an unbound import instead
 * reports itself once, returns 0, and lets the guest carry on, so a single run
 * enumerates everything the startup path reaches. The game is wrong from the
 * first such call - this is for finding the work, not for playing. */
static int permissive(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LINDBERGH_HLE_PERMISSIVE");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

static unsigned char *g_seen;

/* Name every import as it is entered. Heavier than the survey - one line per
 * call, not per distinct import - but it is the only way to see what was
 * running when the process dies in a way no handler can catch, which
 * __fastfail (STATUS_STACK_BUFFER_OVERRUN) does by design. */
static int tracing(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LINDBERGH_HLE_TRACE");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

/* Stop deliberately at the Nth import call and report, because the failure
 * this was written for kills the process with __fastfail - which bypasses
 * every exception handler by design, so the crash reporter never runs. When a
 * failure is deterministic, arriving just before it is as good as catching it.
 *   LINDBERGH_HLE_BREAK=46211 */
static void maybe_break(CPU *c, HleId id)
{
    static long counter, at = -1;
    if (at < 0) {
        const char *v = getenv("LINDBERGH_HLE_BREAK");
        at = (v && *v) ? atol(v) : 0;
    }
    if (!at) return;
    /* LINDBERGH_HLE_BREAK_NAME=nanosleep counts only that import, so the
     * break lands on the thread that makes it. */
    static const char *only = (const char *)-1;
    if (only == (const char *)-1) only = getenv("LINDBERGH_HLE_BREAK_NAME");
    if (only && *only && strcmp(hle_name(id), only) != 0) return;
    if (++counter >= at) {
        fprintf(stderr, "[break] import call #%ld is %s\n", counter, hle_name(id));
        guest_report_state("deliberate break");
        exit(42);
    }
}

void hle_call(CPU *c, HleId id)
{
    maybe_break(c, id);
    if (tracing()) {
        static int next_tid;
        static __thread int tid;
        if (!tid) tid = __sync_add_and_fetch(&next_tid, 1);
        fprintf(stderr, "[call] t%d %s\n", tid, hle_name(id));
        fflush(stderr);
    }
    if ((unsigned)id < HLE_COUNT && g_hle_handlers[id]) {
        /* GL calls can be routed through stereo.c, which records each frame
         * to draw it again for the second eye. */
        if (g_gl_hook && g_gl_ids[id]) { g_gl_hook(c, id, g_hle_handlers[id]); return; }
        g_hle_handlers[id](c);
        return;
    }
    if (permissive()) {
        if (!g_seen) g_seen = (unsigned char *)calloc(HLE_COUNT, 1);
        if (g_seen && !g_seen[id]) {
            g_seen[id] = 1;
            fprintf(stderr, "[hle] %s\n", hle_name(id));
            fflush(stderr);
        }
        c->eax = 0;
        return;
    }
    fprintf(stderr,
            "[hle] %s() is not implemented.\n"
            "      Bind it:  hle_bind(\"%s\", my_%s);\n"
            "      Or set LINDBERGH_HLE_PERMISSIVE=1 to list everything the\n"
            "      startup path wants in one run.\n",
            hle_name(id), hle_name(id), hle_name(id));
    abort();
}
