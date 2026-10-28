/*
 * crash_report.c - what the simulated machine was doing when it faulted.
 *
 * A debugger on a recompiled program shows you a C function named L_08411FF0
 * a million lines into a generated file, which is true and useless. What you
 * need is the guest's state: which lifted function was running, how it got
 * there, and what the registers held.
 *
 * pcrecomp has the equivalent for its global-register model in
 * runtime/recomp32/crash_report.c. That one reads g_eax and friends directly,
 * so it cannot serve the CPU-struct model - this is the same idea against a
 * `CPU *`. If a second CPU-struct target ever wants it, it should move up into
 * pcrecomp/runtime/recomp32_cpu/ beside cpu.h rather than be copied again.
 *
 * The handler reports and then declines the exception, so the process still
 * dies and a debugger still gets its turn. Nothing here allocates.
 */

#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "lindbergh_rt.h"

/* The CPU currently executing guest code. Thread-local: every guest thread
 * runs on its own CPU struct, so a single global would report whichever thread
 * happened to start last rather than the one that faulted. */
#ifdef _MSC_VER
static __declspec(thread) CPU *g_current;
#else
static __thread CPU *g_current;
#endif

void guest_set_current_cpu(CPU *c) { g_current = c; }

/* Recent dispatches, newest last. Written on the hot path, so it is one store
 * and one masked increment and nothing else - no timestamps, no names. */
#define TRACE_N 64
static uint32_t g_trace[TRACE_N];
static unsigned g_trace_i;

/* Persist the ring to a file, because the failure being chased kills the
 * process with __fastfail and no handler runs - so an in-memory ring is lost
 * exactly when it is wanted. Written every RING_FLUSH dispatches, which costs
 * one small write per few thousand calls and nothing at all unless
 * LINDBERGH_TRACE_FILE names somewhere to put it. */
#define RING_FLUSH 2048

static FILE *g_ringf;
static unsigned g_since_flush;

static void ring_persist(void)
{
    if (!g_ringf) {
        const char *path = getenv("LINDBERGH_TRACE_FILE");
        if (!path || !*path) { g_since_flush = 0; return; }
        g_ringf = fopen(path, "wb");
        if (!g_ringf) return;
    }
    fseek(g_ringf, 0, SEEK_SET);
    fwrite(&g_trace_i, 4, 1, g_ringf);
    fwrite(g_trace, 4, TRACE_N, g_ringf);
    fflush(g_ringf);
}

void guest_trace_dispatch(uint32_t va)
{
    /* How often the ring reaches the disc. Every few thousand calls is free
     * and enough to see roughly where a run died; chasing an exact
     * instruction wants LINDBERGH_TRACE_EVERY=1, which is slow and exact. */
    static unsigned every;
    if (!every) {
        const char *v = getenv("LINDBERGH_TRACE_EVERY");
        every = (v && *v) ? (unsigned)atoi(v) : RING_FLUSH;
        if (!every) every = RING_FLUSH;
    }
    g_trace[g_trace_i++ & (TRACE_N - 1)] = va;
    if (++g_since_flush >= every) {
        g_since_flush = 0;
        ring_persist();
    }
}

static const char *region(uint32_t va)
{
    if (va >= 0x08048000u && va < 0x08d00000u) return "image";
    if (va >= 0xbf000000u)                     return "stack";
    if (!va)                                   return "NULL";
    if (va < 0x1000u)                          return "near NULL";
    return "";
}

/* Walk the guest's frame pointers.
 *
 * The dispatch ring says what ran; it does not say who called whom, and for a
 * fault inside a leaf like _Rb_tree::find the caller is the entire question.
 * Lifted code keeps real frames - every function opens with push ebp / mov
 * ebp, esp - so the chain is the ordinary one: saved ebp at [ebp], return
 * address at [ebp+4].
 *
 * Addresses only, deliberately. The names live in the guest ELF's symbol
 * table, which this runtime has no reason to carry; tools/elf turns a column
 * of addresses into names in one pass, and that keeps the fault path free of
 * anything that could itself fail. */
static int readable(uint32_t va)
{
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof mbi)) {
        fprintf(stderr, "  (cannot query %#010x)\n", va);
        return 0;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        fprintf(stderr, "  (%#010x: state %#lx protect %#lx)\n",
                va, (unsigned long)mbi.State, (unsigned long)mbi.Protect);
        return 0;
    }
    return 1;
#else
    return va > 0x1000u;
#endif
}

void guest_backtrace(CPU *c)
{
    if (!c) return;
    fprintf(stderr, "guest backtrace (ebp chain):\n");

    uint32_t ebp = c->ebp;
    for (int depth = 0; depth < 32; depth++) {
        if (!readable(ebp) || !readable(ebp + 4)) break;
        uint32_t saved = rd32(ebp);
        uint32_t ret   = rd32(ebp + 4);
        if (!ret) break;
        fprintf(stderr, "  %2d  %#010x\n", depth, ret);
        if (saved <= ebp) break;          /* frames grow upward; anything else is junk */
        ebp = saved;
    }
    fflush(stderr);
}

void guest_report_state(const char *why)
{
    fprintf(stderr, "\n==== %s ====\n", why);

    if (g_current) {
        CPU *c = g_current;
        fprintf(stderr, "eax %08X  ecx %08X  edx %08X  ebx %08X\n",
                c->eax, c->ecx, c->edx, c->ebx);
        fprintf(stderr, "esp %08X  ebp %08X  esi %08X  edi %08X\n",
                c->esp, c->ebp, c->esi, c->edi);
        fprintf(stderr, "flags cf=%u zf=%u sf=%u of=%u pf=%u af=%u\n",
                c->cf, c->zf, c->sf, c->of, c->pf, c->af);
    } else {
        fprintf(stderr, "(no CPU registered)\n");
    }

    guest_backtrace(g_current);
    fprintf(stderr, "last %d dispatches, oldest first:\n", TRACE_N);
    unsigned n = g_trace_i < TRACE_N ? g_trace_i : TRACE_N;
    for (unsigned k = n; k > 0; k--) {
        uint32_t va = g_trace[(g_trace_i - k) & (TRACE_N - 1)];
        fprintf(stderr, "  %2u  %#010x %s\n", n - k, va, region(va));
    }
    fflush(stderr);
}

#ifdef _WIN32
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    char why[160];

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        uint32_t addr = (uint32_t)er->ExceptionInformation[1];
        snprintf(why, sizeof why, "access violation: %s %#010x %s",
                 er->ExceptionInformation[0] ? "write to" : "read from",
                 addr, region(addr));
    } else {
        snprintf(why, sizeof why, "exception %08lX", (unsigned long)er->ExceptionCode);
    }
    guest_report_state(why);
    return EXCEPTION_CONTINUE_SEARCH;      /* still crash; let a debugger see it */
}

/* The CRT's invalid parameter handler.
 *
 * Hand a Microsoft CRT function something it refuses - a null string, a
 * closed FILE - and it does not return an error. It calls __fastfail, which
 * raises 0xC0000409 and takes the process down past every vectored exception
 * handler and every atexit hook. Nothing is printed, nothing is flushed, and
 * from the outside it is indistinguishable from a silent crash.
 *
 * Which is exactly what it looked like, for several builds. Taking the
 * handler means the guest state gets reported like any other fault. */
static void on_invalid_parameter(const wchar_t *expr, const wchar_t *func,
                                 const wchar_t *file, unsigned line,
                                 uintptr_t reserved)
{
    (void)reserved;
    fprintf(stderr, "\n[crt] invalid parameter in %ls (%ls:%u): %ls\n",
            func ? func : L"?", file ? file : L"?", line,
            expr ? expr : L"(no expression)");
    fflush(stderr);
    guest_report_state("invalid parameter passed to a host library function");
    exit(45);
}

static void on_abort_signal(int sig)
{
    (void)sig;
    fprintf(stderr, "\n[abort] something called abort()\n");
    fflush(stderr);
    guest_report_state("abort");
    _exit(49);
}

static void on_exit_note(void)
{
    fprintf(stderr, "[exit] process is exiting\n");
    fflush(stderr);
}

/* Per thread, because the CRT looks there first.
 *
 * _set_invalid_parameter_handler is process wide, but the UCRT consults a
 * thread-local handler before it, and a thread that never sets one gets the
 * default - which calls __fastfail. A game with a render thread and an audio
 * thread therefore dies silently on two of its three threads no matter what
 * the main thread installed. */
void guest_install_thread_handlers(void)
{
    _set_thread_local_invalid_parameter_handler(on_invalid_parameter);
    signal(SIGABRT, on_abort_signal);
}

void guest_install_crash_handler(void)
{
    /* Unbuffered, before anything else can be lost.
     *
     * stderr is block buffered the moment it is redirected to a file, so a
     * process that dies without unwinding takes its last few thousand
     * characters with it - including, most of the time, the one line that
     * said what went wrong. Every report here ends in fflush for that reason,
     * but the lines that matter are often the ordinary ones just before. */
    setvbuf(stderr, NULL, _IONBF, 0);
    /* Who ends the process. A clean exit with nothing said looks exactly like
     * a crash from the outside, and it took a Windows exit code of zero to
     * notice that Ghost Squad was leaving rather than falling over. */
    atexit(on_exit_note);
    _set_invalid_parameter_handler(on_invalid_parameter);
    guest_install_thread_handlers();

    /* Stop abort() from becoming a fail-fast.
     *
     * By default the Microsoft CRT turns abort() into __fastfail, which the
     * kernel handles directly: no vectored handler, no atexit, no output, and
     * an exit code of 0xC0000409 that says only "something gave up". Clearing
     * _CALL_REPORTFAULT makes it an ordinary abort again, which SIGABRT can
     * catch and report like anything else. */
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, on_abort_signal);
    AddVectoredExceptionHandler(1, on_exception);
}
#else
/* POSIX: the same report from a signal handler. A fault in lifted code is a
 * host SIGSEGV/SIGBUS at an address inside the guest window, so the address is
 * reported in guest terms too. Runs on an alternate stack, so running out of
 * host stack (deep guest recursion) is still reported rather than silent. */
#include <unistd.h>
#include <dlfcn.h>
#if defined(__APPLE__)
#include <execinfo.h>
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

/* Which host function faulted: a fault inside a handler is a runtime bug, one
 * inside L_xxxxxxxx is the guest. dladdr names it without a debugger. */
static void host_report_pc(void *ucv)
{
    uintptr_t pc = 0, lr = 0;
#if defined(__APPLE__) && defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)ucv;
    pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
#elif defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)ucv;
    pc = (uintptr_t)uc->uc_mcontext.pc;
    lr = (uintptr_t)uc->uc_mcontext.regs[30];
#endif
    uintptr_t at[2] = { pc, lr };
    const char *what[2] = { "host pc", "host lr" };
    for (int i = 0; i < 2; i++) {
        Dl_info di;
        if (at[i] && dladdr((void *)at[i], &di) && di.dli_sname)
            fprintf(stderr, "%s %p = %s+%#lx\n", what[i], (void *)at[i], di.dli_sname,
                    (unsigned long)(at[i] - (uintptr_t)di.dli_saddr));
        else if (at[i])
            fprintf(stderr, "%s %p\n", what[i], (void *)at[i]);
    }
#if defined(__APPLE__)
    void *bt[24];
    int n = backtrace(bt, 24);
    backtrace_symbols_fd(bt, n, 2);
#endif
}

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    char why[200];
    uintptr_t a = (uintptr_t)si->si_addr;
#ifdef RECOMP_GUEST_BASE
    extern uint8_t *g_guest_base;
    uintptr_t base = (uintptr_t)g_guest_base;
    if (a >= base && a - base < (1ull << 32))
        snprintf(why, sizeof why, "signal %d (%s) at guest %#010x %s", sig,
                 sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" :
                 sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : "?",
                 (uint32_t)(a - base), region((uint32_t)(a - base)));
    else
#endif
        snprintf(why, sizeof why, "signal %d at host address %p (outside the guest window)",
                 sig, si->si_addr);
    guest_report_state(why);
    host_report_pc(uc);
    signal(sig, SIG_DFL);                 /* let the default action (core) happen */
    raise(sig);
}

static void on_abort_signal(int sig)
{
    (void)sig;
    fprintf(stderr, "\n[abort] something called abort()\n");
    guest_report_state("abort");
    _exit(49);
}

static void on_exit_note(void)
{
    fprintf(stderr, "[exit] process is exiting\n");
}

void guest_install_thread_handlers(void)
{
    /* each thread gets its own alternate signal stack */
    stack_t ss;
    ss.ss_size = 1u << 20;
    ss.ss_sp = malloc(ss.ss_size);
    ss.ss_flags = 0;
    if (ss.ss_sp) sigaltstack(&ss, NULL);
}

/* ---- the hang watchdog ----
 * A game that stops presenting frames without crashing is the silent failure
 * a crash handler never sees. Once the first frame is out, a host thread
 * watches the frame counter; if it stalls for LINDBERGH_WATCHDOG seconds
 * (default 10, 0 = off) the main thread is signalled and reports where it is -
 * guest registers, ebp chain and host stack - from its own context. Once per
 * stall; the game keeps running. */
#include <pthread.h>
extern unsigned g_frame;
/* Android's runtime keeps SIGUSR1 blocked in every thread for itself */
#ifdef __ANDROID__
#define WATCHDOG_SIG SIGUSR2
#include <unwind.h>
static _Unwind_Reason_Code unwind_one(struct _Unwind_Context *c, void *arg)
{
    int *n = arg;
    uintptr_t pc = _Unwind_GetIP(c);
    if (!pc || (*n)++ >= 40) return _URC_END_OF_STACK;
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_fname) {
        const char *f = strrchr(di.dli_fname, '/');
        fprintf(stderr, "  #%02d %s+%#lx %s\n", *n - 1, f ? f + 1 : di.dli_fname,
                (unsigned long)(pc - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "");
    } else
        fprintf(stderr, "  #%02d %p\n", *n - 1, (void *)pc);
    return _URC_NO_REASON;
}
#else
#define WATCHDOG_SIG SIGUSR1
#endif
static pthread_t g_main_thread;

#ifndef _WIN32
unsigned guest_threads(pthread_t *out, unsigned max);
#endif
static void on_watchdog_signal(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    char why[96];
    snprintf(why, sizeof why, "watchdog: no frame presented - %s thread %#lx is here",
             pthread_equal(pthread_self(), g_main_thread) ? "main" : "guest", (unsigned long)pthread_self());
    guest_report_state(why);
    host_report_pc(uc);
#ifdef __ANDROID__
    int n = 0;      /* offsets in the unstripped libmain.so (build-quest) name them */
    _Unwind_Backtrace(unwind_one, &n);
#endif
}

static void *watchdog(void *arg)
{
    unsigned secs = (unsigned)(uintptr_t)arg, last = 0, still = 0, told = 0;
    for (;;) {
        sleep(1);
        unsigned f = g_frame;
        if (!f) continue;                       /* still loading */
        if (f != last) { last = f; still = 0; told = 0; continue; }
        if (++still >= secs && !told) {
            told = 1;
            fprintf(stderr, "\n[watchdog] frame %u has not advanced for %u s\n", f, still);
            pthread_kill(g_main_thread, WATCHDOG_SIG);
#ifndef _WIN32
            /* and every other guest thread: a stall is often one waiting on another */
            pthread_t th[64];
            unsigned n = guest_threads(th, 64);
            for (unsigned i = 0; i < n; i++) { sleep(1); pthread_kill(th[i], WATCHDOG_SIG); }
#endif
        }
    }
    return NULL;
}

void guest_install_crash_handler(void)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    atexit(on_exit_note);
    guest_install_thread_handlers();
    g_main_thread = pthread_self();
    {
        const char *v = getenv("LINDBERGH_WATCHDOG");
        unsigned secs = v && *v ? (unsigned)atoi(v) : 10u;
        if (secs) {
            struct sigaction wa;
            memset(&wa, 0, sizeof wa);
            wa.sa_sigaction = on_watchdog_signal;
            wa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
            sigaction(WATCHDOG_SIG, &wa, NULL);
            sigset_t un;
            sigemptyset(&un); sigaddset(&un, WATCHDOG_SIG);
            pthread_sigmask(SIG_UNBLOCK, &un, NULL);
            pthread_t t;
            pthread_create(&t, NULL, watchdog, (void *)(uintptr_t)secs);
            pthread_detach(t);
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    signal(SIGABRT, on_abort_signal);
}
#endif

/* An instruction the lifter could not translate, reached at run time.
 *
 * The lifter used to emit a bare abort() here, and that cost four wrong
 * diagnoses: MSVC's abort() calls __fastfail(FAST_FAIL_FATAL_APP_EXIT), which
 * raises 0xC0000409 - a status named STATUS_STACK_BUFFER_OVERRUN, past every
 * exception handler, with no message. So an unimplemented instruction looked
 * exactly like memory corruption, and the one thing it could not look like was
 * itself.
 *
 * Saying what it was, where it was, and how the program got there costs four
 * lines and removes the whole confusion. */
void lindbergh_todo(uint32_t va, const char *text)
{
    fprintf(stderr, "\n[recomp] unimplemented instruction at %#010x: %s\n", va, text);
    guest_report_state("unimplemented instruction");
    exit(44);
}

/* Ending the game at the platform's request. On Android exit() runs the
 * libraries' teardown - OpenAL's among them - while the game's sound thread
 * is still calling into it, which aborted on a destroyed mutex; the nvram is
 * written as the game writes it, so there is nothing left to save: end the
 * process at once. */
void host_quit(void)
{
#ifdef __ANDROID__
    fprintf(stderr, "[exit] quitting\n");
    fflush(NULL);
    _exit(0);
#else
    exit(0);
#endif
}
