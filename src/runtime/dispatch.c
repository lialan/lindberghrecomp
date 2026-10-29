/*
 * dispatch.c - original address -> lifted function.
 *
 * Direct calls could have been emitted as plain C calls, but indirect ones
 * cannot: the game computes a target at run time (vtables, callback tables,
 * the task dispatchers arcade code is full of) and hands over an address from
 * the original image. Every call goes through here so both kinds land the
 * same way.
 *
 * The table is generated - recomp_funcs_list.h is the X-macro of every VA
 * that lifted, emitted in ascending order, which is what lets this binary
 * search instead of scanning a few thousand entries per indirect call.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lindbergh_rt.h"
#include "stereo.h"
#include "recomp_funcs_list.h"
#include "recomp_plt.h"

#ifndef SOLIB_FUNCS
#define SOLIB_FUNCS(X)
#define SOLIB_PLT(X)
#define SOLIB_ALIASES(X)
#endif

#define DECL(a) void L_##a(CPU *c);
LIFTED_FUNCS(DECL)
SOLIB_FUNCS(DECL)
#undef DECL

typedef struct { uint32_t va; void (*fn)(CPU *); } Entry;

#define ENT(a) { 0x##a##u, L_##a },
/* the libraries sit above the image, so appending keeps the table sorted */
static const Entry g_table[] = { LIFTED_FUNCS(ENT) SOLIB_FUNCS(ENT) };
#undef ENT

#define N (sizeof g_table / sizeof g_table[0])

static size_t find_index(uint32_t va)      /* +1, 0: none */
{
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_table[mid].va == va) return mid + 1;
        if (g_table[mid].va < va) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* PLT stubs, address -> import. In a non-PIE ELF the address OF an imported
 * function IS its PLT stub, so `mov eax, offset memalign` puts 0x080727d0 in
 * a register and every later indirect call through that pointer arrives here
 * as a plain address rather than as a `call <stub>` the lifter could rewrite.
 * Function pointers, vtable slots, callback tables installed at startup - they
 * all land in this table instead of in the lifted one. */
typedef struct { uint32_t va; HleId id; } PltEntry;

#define PLT_ENT(a, id) { (a), (id) },
static const PltEntry g_plt[] = { HLE_PLT(PLT_ENT) SOLIB_PLT(PLT_ENT) };
#undef PLT_ENT

#define NPLT (sizeof g_plt / sizeof g_plt[0])

static int find_plt(uint32_t va, HleId *out)
{
    size_t lo = 0, hi = NPLT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_plt[mid].va == va) { *out = g_plt[mid].id; return 1; }
        if (g_plt[mid].va < va) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* Guest call depth, per thread.
 *
 * A guest call becomes a host call: dispatch() invokes a lifted C function
 * which calls dispatch() again. So guest recursion costs host stack, and a
 * runaway one exhausts it. That failure is worth catching deliberately,
 * because Windows reports stack exhaustion inconsistently - sometimes
 * STATUS_STACK_OVERFLOW, sometimes a cookie check that ends in __fastfail,
 * which no handler can see. Counting is cheaper than diagnosing afterwards. */
#ifdef _MSC_VER
static __declspec(thread) unsigned g_depth;
static __declspec(thread) unsigned g_depth_max;
#else
static __thread unsigned g_depth;
static __thread unsigned g_depth_max;
#endif

#define DEPTH_LIMIT 20000

/* How an address resolved, cached: one 64-bit word per slot - the address
 * above, below an index (+1, 0 empty) into the lifted table, or with the top
 * bit set into the overrides - so a slot read while another thread writes it
 * is old or new, never half of each. */
#define DCACHE_N 8192
static uint64_t g_dcache[DCACHE_N];
void dispatch_cache_clear(void) { memset(g_dcache, 0, sizeof g_dcache); }   /* an override added */

/* The lifted body at an address, past any override: for an override that
 * wraps the game's own function. */
void (*dispatch_lifted(uint32_t va))(CPU *)
{
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_table[mid].va == va) return g_table[mid].fn;
        if (g_table[mid].va < va) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

unsigned guest_depth(void)     { return g_depth; }
unsigned guest_depth_max(void) { return g_depth_max; }

/* Did execution ever reach THIS function? Set LINDBERGH_WATCH to a comma-
 * separated list of hex addresses and each one reports the first time it is
 * dispatched. The question comes up constantly during bring-up - a branch went
 * the wrong way and something that should have run did not - and it is not
 * answerable from a backtrace, which only shows what DID run. */
extern unsigned g_frame;
static int watch_check(CPU *c, uint32_t va)
{
    static uint32_t list[16];
    static int n = -1, nargs;
    static unsigned hit[16];
    if (n < 0) {
        n = 0;
        const char *v = getenv("LINDBERGH_WATCH");
        while (v && *v && n < 16) {
            list[n++] = (uint32_t)strtoul(v, (char **)&v, 16);
            while (*v == ',' || *v == ' ') v++;
        }
        /* LINDBERGH_WATCH_ARGS=N: report the first N hits of each, with
         * their first four arguments (esp+0 is the return address here) */
        const char *a = getenv("LINDBERGH_WATCH_ARGS");
        nargs = a ? atoi(a) : 0;
    }
    for (int i = 0; i < n; i++)
        if (list[i] == va && hit[i] < (unsigned)(nargs ? nargs : 1)) {
            hit[i]++;
            /* LINDBERGH_WATCH_MEM=addr adds that guest dword to each report */
            static int wm = -1; static uint32_t wma;
            if (wm < 0) { const char *m = getenv("LINDBERGH_WATCH_MEM"); wm = m && *m; if (wm) wma = (uint32_t)strtoul(m, NULL, 16); }
            if (nargs && wm)
                fprintf(stderr, "[watch] %#010x mem[%#x]=%#x frame %u\n", va, wma, rd32(wma), g_frame);
            if (nargs)
                fprintf(stderr, "[watch] %#010x(%#x, %#x, %#x, %#x) from %#010x\n", va,
                        rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12),
                        rd32(c->esp + 16), rd32(c->esp));
            else
                fprintf(stderr, "[watch] reached %#010x\n", va);
            fflush(stderr);
            return nargs != 0;
        }
    return 0;
}

static int gl_token_call_entry(CPU *c, uint32_t va)
{
    uint32_t saved = c->esp;
    c->esp += 4;                       /* the return slot, as  would */
    if (gl_token_call(c, va)) return 1;
    c->esp = saved;
    return 0;
}

/* LINDBERGH_TRACE_RANGE=lo,hi (hex): the guest functions in that range each
 * frame called, printed whenever the set changes - which of the game's
 * screen filters run, say */
extern unsigned g_frame;
static void range_trace(uint32_t va)
{
    static int on = -1;
    static uint32_t lo, hi, cur[32], prev[32];
    static int ncur, nprev;
    static unsigned at;
    if (on < 0) {
        const char *v = getenv("LINDBERGH_TRACE_RANGE");
        on = v && *v;
        if (on) { lo = (uint32_t)strtoul(v, (char **)&v, 16); if (*v == ',') v++; hi = (uint32_t)strtoul(v, NULL, 16); }
    }
    if (!on) return;
    if (g_frame != at) {
        int same = ncur == nprev;
        for (int i = 0; same && i < ncur; i++) same = cur[i] == prev[i];
        if (!same) {
            fprintf(stderr, "[range] frame %u:", at);
            for (int i = 0; i < ncur; i++) fprintf(stderr, " %#x", cur[i]);
            fprintf(stderr, "\n");
        }
        memcpy(prev, cur, sizeof cur); nprev = ncur; ncur = 0; at = g_frame;
    }
    if (va < lo || va > hi) return;
    for (int i = 0; i < ncur; i++) if (cur[i] == va) return;
    if (ncur < 32) { int i = ncur++; while (i > 0 && cur[i - 1] > va) { cur[i] = cur[i - 1]; i--; } cur[i] = va; }
}

void dispatch(CPU *c, uint32_t va)
{
    guest_trace_dispatch(va);
    range_trace(va);
    int watched = watch_check(c, va);
    if (va >= stereo_state_lo && va <= stereo_state_hi) stereo_guest_state(va);

    if (++g_depth > g_depth_max) g_depth_max = g_depth;
    if (g_depth > DEPTH_LIMIT) {
        fprintf(stderr, "[dispatch] guest call depth %u at %#010x - runaway recursion\n",
                g_depth, va);
        guest_report_state("call depth limit");
        exit(43);
    }
    /* A GL entry point handed out by glXGetProcAddressARB. Same stack shape as
     * an indirect PLT hit: consume the return slot so the handler sees
     * argument 0 at esp+0. */
    if (gl_token_call_entry(c, va)) { g_depth--; return; }

    /* A symbol dlsym handed out for a library this runtime does not load.
     * Same stack shape, and doing nothing beats dispatching to address zero. */
    if (va >= 0xF1000000u) {
        uint32_t saved = c->esp;
        c->esp += 4;
        if (hle_stub_call(c, va)) { g_depth--; return; }
        c->esp = saved;
    }

    /* Most calls are to an address seen before: how it resolved is cached,
     * by address. The searches below - a scan of the overrides, a binary
     * search of ~10k lifted functions - were a fifth of the game thread. */
    uint64_t *sl = &g_dcache[(va >> 2) & (DCACHE_N - 1)];
    uint64_t e = __atomic_load_n(sl, __ATOMIC_RELAXED);
    if ((uint32_t)(e >> 32) == va && (uint32_t)e) {
        uint32_t k = (uint32_t)e;
        if (k & 0x80000000u) { (void)pop32(c); guest_override_at((int)(k & 0x7fffffffu) - 1)(c); }
        else g_table[k - 1].fn(c);
        g_depth--;
        if (watched) fprintf(stderr, "[watch] %#010x -> %#x\n", va, c->eax);
        return;
    }

    /* A host body standing in for a guest function. Checked first, so it wins
     * over the lifted version; the range test costs two compares. */
    int oi = guest_find_override_index(va);
    HleHandler ov = oi >= 0 ? guest_override_at(oi) : NULL;
    if (ov) __atomic_store_n(sl, (uint64_t)va << 32 | 0x80000000u | (uint32_t)(oi + 1), __ATOMIC_RELAXED);
    if (ov) { (void)pop32(c); ov(c); g_depth--;
              if (watched) fprintf(stderr, "[watch] %#010x -> %#x\n", va, c->eax);
              return; }

    size_t fi = find_index(va);
    void (*fn)(CPU *) = fi ? g_table[fi - 1].fn : NULL;
    if (fn && !ov) __atomic_store_n(sl, (uint64_t)va << 32 | (uint32_t)fi, __ATOMIC_RELAXED);
    if (fn) { fn(c); g_depth--;
              if (watched) fprintf(stderr, "[watch] %#010x -> %#x\n", va, c->eax);
              return; }

    /* An import reached indirectly. The stack here is one slot off from a
     * `call <stub>` site: esp points at the return address with the arguments
     * above it, where a handler expects argument 0 at esp+0. Consuming the
     * return slot - which is all a lifted `ret` does - lines them up and
     * leaves esp where the caller's own `add esp, N` expects it. Getting this
     * wrong reads every argument shifted by four bytes. */
    HleId id;
    if (find_plt(va, &id)) {
        (void)pop32(c);
        hle_call(c, id);
        g_depth--;
        return;
    }

    /* A library's own PLT stub for a function another library defines. */
    {
#define ALIAS(stub, target) if (va == stub) { dispatch(c, target); g_depth--; return; }
        SOLIB_ALIASES(ALIAS)
#undef ALIAS
    }

    /* Neither lifted nor an import: a function the bounds missed, or a jump
     * into the middle of one that did lift. Both are worth the address. */
    fprintf(stderr, "[dispatch] no lifted function at %#010x (called from esp=%#010x)\n",
            va, c->esp);
    abort();
}

/* Reverse of the PLT table: the stub address for a named import, or 0.
 *
 * This exists for glXGetProcAddressARB. The game asks the driver for an
 * extension entry point and then CALLS the pointer it gets back - as a guest
 * indirect call, through dispatch(). Handing it a real host function address
 * would be useless: dispatch would look that up in the lifted table, find
 * nothing, and abort.
 *
 * But the game also imports most of those extension functions through its own
 * PLT, so their stub addresses are already routable tokens with handlers
 * behind them. Returning the stub means the call goes back through the same
 * path as a direct one, with no extra machinery at all. */
uint32_t hle_plt_address(const char *name)
{
    for (size_t i = 0; i < NPLT; i++)
        if (strcmp(hle_name(g_plt[i].id), name) == 0)
            return g_plt[i].va;
    return 0;
}

void dispatch_jmp(CPU *c, uint32_t va)
{
    /* A tail call: the caller already popped its frame, so the callee returns
     * straight to our caller's caller. Same table, no return address pushed. */
    dispatch(c, va);
}

/* ---- calling back into guest code ----
 *
 * Needed whenever the host has to run something the game owns: the CRT's
 * constructors, a thread's start routine, a qsort comparator, an atexit
 * handler, a GL callback. The shape is the one a lifted function expects on
 * entry - arguments pushed right to left, then a return slot for its own `ret`
 * to pop - and cdecl says the caller cleans up, which is what restoring esp
 * afterwards does.
 *
 * The return address pushed is deliberately a value no code maps to. A lifted
 * `ret` only does `esp += 4; return;` and never jumps to it, so it is never
 * read; if it ever shows up in a diagnostic, something has gone wrong in a way
 * worth noticing.
 */
uint32_t guest_call(CPU *c, uint32_t fn, const uint32_t *args, int nargs)
{
    uint32_t saved = c->esp;
    for (int i = nargs - 1; i >= 0; i--)
        push32(c, args[i]);
    push32(c, 0xDEADBEEFu);
    dispatch(c, fn);
    c->esp = saved;
    return c->eax;
}
