/*
 * hle_pthread.c - the threads the board provided.
 *
 * Let's Go Jungle sets up its mutexes and condition variables, spawns a worker
 * thread and waits on it, all before it opens the display - so none of this is
 * optional to get a picture.
 *
 * A guest thread gets its own CPU struct and its own guest stack. That is the
 * whole reason this project uses pcrecomp's CPU-struct lifter rather than its
 * global-register one: two machine states have to be live at once, and with
 * registers in globals they would be the same registers.
 *
 * Guest objects (pthread_mutex_t and friends) are opaque blobs of a fixed
 * size that the game allocates itself - on its stack, in its globals, inside
 * its own structs. Rather than model glibc's layout, this writes its own small
 * header into that space. The sizes on i386 glibc 2.3 are:
 *
 *     pthread_mutex_t     24 bytes
 *     pthread_cond_t      48
 *     pthread_attr_t      36
 *     pthread_mutexattr_t  4
 *     sem_t               16
 *
 * Everything here fits well inside the smallest of those, which is what makes
 * the approach safe. The magic word matters for a second reason: glibc lets a
 * mutex be initialised statically with PTHREAD_MUTEX_INITIALIZER and never
 * passed to pthread_mutex_init at all, so a lock on a zeroed blob has to work.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "lindbergh_rt.h"

#define GUEST_MAGIC 0x4C474A31u      /* "LGJ1" */

typedef struct {
    uint32_t magic;
    void    *host;
} GuestObj;

#ifdef _WIN32

/* One lock guards lazy creation of guest objects. Contended only on the first
 * touch of each object, so a single global is the right amount of machinery.
 * ponytail: per-object double-checked init if that ever shows up in a profile. */
static CRITICAL_SECTION g_init_lock;
static int g_init_ready;

static void init_once(void)
{
    if (!g_init_ready) { InitializeCriticalSection(&g_init_lock); g_init_ready = 1; }
}

/* Fetch the host object behind a guest blob, creating it if the guest never
 * called the _init function - which is legal for statically initialised
 * mutexes and condition variables. */
static void *obj_get(uint32_t va, size_t host_size, void (*ctor)(void *))
{
    GuestObj *g = (GuestObj *)GP(va);
    if (g->magic == GUEST_MAGIC && g->host)
        return g->host;

    init_once();
    EnterCriticalSection(&g_init_lock);
    if (g->magic != GUEST_MAGIC || !g->host) {
        void *h = calloc(1, host_size);
        if (h) ctor(h);
        g->host  = h;
        g->magic = GUEST_MAGIC;
    }
    LeaveCriticalSection(&g_init_lock);
    return g->host;
}

static void ctor_cs(void *p)   { InitializeCriticalSection((CRITICAL_SECTION *)p); }
static void ctor_cond(void *p) { InitializeConditionVariable((CONDITION_VARIABLE *)p); }

#define MUTEX(n)  ((CRITICAL_SECTION *)obj_get(A32(n), sizeof(CRITICAL_SECTION), ctor_cs))
#define COND(n)   ((CONDITION_VARIABLE *)obj_get(A32(n), sizeof(CONDITION_VARIABLE), ctor_cond))

/* ---- mutexes ----
 *
 * A CRITICAL_SECTION is always recursive, where a default pthread mutex is
 * not. The game calls pthread_mutexattr_settype, so it wants recursive here;
 * and for code that does not relock, the difference is invisible. Where it
 * would show is a program that relies on deadlocking itself, which is not a
 * thing anyone relies on. */
static void h_mutex_init(CPU *c)    { (void)MUTEX(0); RET(0); }
static void h_mutex_lock(CPU *c)    { EnterCriticalSection(MUTEX(0)); RET(0); }
static void h_mutex_unlock(CPU *c)  { LeaveCriticalSection(MUTEX(0)); RET(0); }
static void h_mutex_trylock(CPU *c) { RET(TryEnterCriticalSection(MUTEX(0)) ? 0 : 16); }

static void h_mutex_destroy(CPU *c)
{
    GuestObj *g = (GuestObj *)GP(A32(0));
    if (g->magic == GUEST_MAGIC && g->host) {
        DeleteCriticalSection((CRITICAL_SECTION *)g->host);
        free(g->host);
        g->host = NULL;
        g->magic = 0;
    }
    RET(0);
}

/* ---- condition variables ---- */
static void h_cond_init(CPU *c)      { (void)COND(0); RET(0); }
static void h_cond_signal(CPU *c)    { WakeConditionVariable(COND(0)); RET(0); }
static void h_cond_broadcast(CPU *c) { WakeAllConditionVariable(COND(0)); RET(0); }

static void h_cond_wait(CPU *c)
{
    /* The mutex must be reacquired before returning, which
     * SleepConditionVariableCS already guarantees. */
    SleepConditionVariableCS(COND(0), MUTEX(1), INFINITE);
    RET(0);
}

static void h_cond_timedwait(CPU *c)
{
    /* The guest passes an ABSOLUTE deadline as struct timespec { long sec;
     * long nsec; }, and Windows wants a relative millisecond count. A deadline
     * already past must not block. */
    uint32_t ts = A32(2);
    DWORD ms = INFINITE;
    if (ts) {
        int64_t deadline = (int64_t)(int32_t)rd32(ts) * 1000
                         + (int64_t)(int32_t)rd32(ts + 4) / 1000000;
        int64_t now = (int64_t)GetTickCount64();
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        int64_t wall = ((((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime)
                        / 10000) - 11644473600000LL;     /* 1601 epoch -> 1970 */
        (void)now;
        int64_t delta = deadline - wall;
        ms = delta <= 0 ? 0 : (DWORD)delta;
    }
    BOOL ok = SleepConditionVariableCS(COND(0), MUTEX(1), ms);
    RET(ok ? 0 : 110);                                   /* ETIMEDOUT */
}

static void h_cond_destroy(CPU *c)
{
    GuestObj *g = (GuestObj *)GP(A32(0));
    if (g->magic == GUEST_MAGIC && g->host) {            /* no destroy call exists */
        free(g->host);
        g->host = NULL;
        g->magic = 0;
    }
    RET(0);
}

/* ---- thread attributes ----
 *
 * Written into the guest's own pthread_attr_t storage rather than a side
 * table, because the guest allocates it (usually on its stack) and hands us
 * only the pointer. 36 bytes there, 20 used. */
typedef struct {
    uint32_t magic;
    uint32_t stacksize;
    int32_t  policy;
    int32_t  priority;
    int32_t  detached;
} GuestAttr;

#define GUEST_STACK_DEFAULT (2u << 20)

static GuestAttr *attr_of(uint32_t va)
{
    GuestAttr *a = (GuestAttr *)GP(va);
    if (a->magic != GUEST_MAGIC) {
        memset(a, 0, sizeof *a);
        a->magic = GUEST_MAGIC;
        a->stacksize = GUEST_STACK_DEFAULT;
    }
    return a;
}

static void h_attr_init(CPU *c)         { (void)attr_of(A32(0)); RET(0); }
static void h_attr_setstacksize(CPU *c) { attr_of(A32(0))->stacksize = A32(1); RET(0); }
static void h_attr_setschedpolicy(CPU *c) { attr_of(A32(0))->policy = AI32(1); RET(0); }
static void h_attr_setschedparam(CPU *c)
{
    /* struct sched_param is a single int. */
    if (A32(1)) attr_of(A32(0))->priority = (int32_t)rd32(A32(1));
    RET(0);
}

/* Nothing here honours a scheduling policy: the arcade board ran SCHED_RR at a
 * fixed priority and a desktop will not, so promising a range we cannot keep
 * would be worse than reporting the one we have. */
static void h_sched_get_priority_max(CPU *c) { RET(0); }
static void h_sched_get_priority_min(CPU *c) { RET(0); }

static void h_mutexattr_init(CPU *c)    { if (A32(0)) wr32(A32(0), 0); RET(0); }
/* glibc keeps the kind in the attribute's int: pthread_mutex_init reads it
 * there (POSIX; the Windows side is always recursive) */
static void h_mutexattr_settype(CPU *c) { if (A32(0)) wr32(A32(0), A32(1)); RET(0); }

/* ---- threads ----
 *
 * The start routine is a GUEST function pointer, so the host thread that runs
 * it needs a whole guest machine: its own CPU struct and its own guest stack.
 * The stack comes from the host allocator, which is fine and not a shortcut -
 * this is a 32-bit process, so every allocation is already inside the address
 * space the guest can name, and a guest pointer is a host pointer.
 */
typedef struct {
    uint32_t start;          /* guest void *(*)(void *) */
    uint32_t arg;
    void    *stack;
    uint32_t stacksize;
    HANDLE   handle;
    uint32_t retval;
} GuestThread;

static DWORD WINAPI thread_trampoline(LPVOID p)
{
    GuestThread *t = (GuestThread *)p;
    CPU cpu;

    memset(&cpu, 0, sizeof cpu);
    guest_set_current_cpu(&cpu);
    guest_install_thread_handlers();   /* the CRT checks per thread first */

    /* Stack grows down, so start at the top. Leave a little headroom and keep
     * esp 16-byte aligned, which the SSE the game is full of requires. */
    uint32_t top = (uint32_t)(uintptr_t)t->stack + t->stacksize - 64;
    cpu.esp = top & ~15u;

    uint32_t args[1] = { t->arg };
    t->retval = guest_call(&cpu, t->start, args, 1);
    return 0;
}

static void h_pthread_create(CPU *c)
{
    uint32_t out = A32(0), attr = A32(1);
    GuestThread *t = (GuestThread *)calloc(1, sizeof *t);
    if (!t) { RET(11); return; }                          /* EAGAIN */

    t->start     = A32(2);
    t->arg       = A32(3);
    t->stacksize = attr ? attr_of(attr)->stacksize : GUEST_STACK_DEFAULT;
    if (t->stacksize < (256u << 10)) t->stacksize = 256u << 10;
    t->stack = malloc(t->stacksize);
    if (!t->stack) { free(t); RET(11); return; }

    t->handle = CreateThread(NULL, 0, thread_trampoline, t, 0, NULL);
    if (!t->handle) { free(t->stack); free(t); RET(11); return; }

    /* pthread_t is an opaque word; hand back the record itself. */
    if (out) wr32(out, (uint32_t)(uintptr_t)t);
    fprintf(stderr, "[pthread] created thread at %#010x, %u KB stack\n",
            t->start, t->stacksize >> 10);
    RET(0);
}

static void h_pthread_join(CPU *c)
{
    GuestThread *t = (GuestThread *)GP(A32(0));
    if (t && t->handle) {
        WaitForSingleObject(t->handle, INFINITE);
        CloseHandle(t->handle);
        if (A32(1)) wr32(A32(1), t->retval);
        free(t->stack);
        free(t);
    }
    RET(0);
}

static void h_pthread_detach(CPU *c) { RET(0); }
static void h_pthread_self(CPU *c)   { RET(GetCurrentThreadId()); }
static void h_pthread_equal(CPU *c)  { RET(A32(0) == A32(1)); }
static void h_pthread_getschedparam(CPU *c) { RET(0); }
static void h_pthread_setschedparam(CPU *c) { RET(0); }

static void h_pthread_once(CPU *c)
{
    /* pthread_once_t is a guest int, zero until done. The routine takes no
     * arguments and the guest is not racing on it at this point. */
    uint32_t flag = A32(0), fn = A32(1);
    if (flag && !rd32(flag)) {
        wr32(flag, 1);
        guest_call(c, fn, NULL, 0);
    }
    RET(0);
}

/* ---- thread-local storage ---- */
static void h_key_create(CPU *c)
{
    DWORD k = TlsAlloc();
    if (k == TLS_OUT_OF_INDEXES) { RET(12); return; }     /* ENOMEM */
    if (A32(0)) wr32(A32(0), (uint32_t)k);
    RET(0);
}
static void h_getspecific(CPU *c) { RET(TlsGetValue(A32(0))); }
static void h_setspecific(CPU *c) { TlsSetValue(A32(0), (LPVOID)(uintptr_t)A32(1)); RET(0); }

/* ---- semaphores ---- */
static void ctor_sem(void *p) { *(HANDLE *)p = NULL; }

static HANDLE *sem_of(uint32_t va)
{
    return (HANDLE *)obj_get(va, sizeof(HANDLE), ctor_sem);
}

static void h_sem_init(CPU *c)
{
    HANDLE *h = sem_of(A32(0));
    if (*h) CloseHandle(*h);
    *h = CreateSemaphore(NULL, (LONG)A32(2), 0x7FFFFFFF, NULL);
    RET(*h ? 0 : -1);
}
static void h_sem_post(CPU *c) { ReleaseSemaphore(*sem_of(A32(0)), 1, NULL); RET(0); }
static void h_sem_wait(CPU *c) { WaitForSingleObject(*sem_of(A32(0)), INFINITE); RET(0); }
static void h_sem_destroy(CPU *c)
{
    GuestObj *g = (GuestObj *)GP(A32(0));
    if (g->magic == GUEST_MAGIC && g->host) {
        HANDLE *h = (HANDLE *)g->host;
        if (*h) CloseHandle(*h);
        free(g->host);
        g->host = NULL;
        g->magic = 0;
    }
    RET(0);
}

void hle_register_pthread(void)
{
    init_once();

    hle_bind("pthread_mutex_init", h_mutex_init);
    hle_bind("pthread_mutex_lock", h_mutex_lock);
    hle_bind("pthread_mutex_unlock", h_mutex_unlock);
    hle_bind("pthread_mutex_trylock", h_mutex_trylock);
    hle_bind("pthread_mutex_destroy", h_mutex_destroy);
    hle_bind("pthread_mutexattr_init", h_mutexattr_init);
    hle_bind("pthread_mutexattr_settype", h_mutexattr_settype);

    hle_bind("pthread_cond_init", h_cond_init);
    hle_bind("pthread_cond_signal", h_cond_signal);
    hle_bind("pthread_cond_broadcast", h_cond_broadcast);
    hle_bind("pthread_cond_wait", h_cond_wait);
    hle_bind("pthread_cond_timedwait", h_cond_timedwait);
    hle_bind("pthread_cond_destroy", h_cond_destroy);

    hle_bind("pthread_attr_init", h_attr_init);
    hle_bind("pthread_attr_setstacksize", h_attr_setstacksize);
    hle_bind("pthread_attr_setschedpolicy", h_attr_setschedpolicy);
    hle_bind("pthread_attr_setschedparam", h_attr_setschedparam);
    hle_bind("sched_get_priority_max", h_sched_get_priority_max);
    hle_bind("sched_get_priority_min", h_sched_get_priority_min);

    hle_bind("pthread_create", h_pthread_create);
    hle_bind("pthread_join", h_pthread_join);
    hle_bind("pthread_detach", h_pthread_detach);
    hle_bind("pthread_self", h_pthread_self);
    hle_bind("pthread_equal", h_pthread_equal);
    hle_bind("pthread_once", h_pthread_once);
    hle_bind("pthread_getschedparam", h_pthread_getschedparam);
    hle_bind("pthread_setschedparam", h_pthread_setschedparam);

    hle_bind("pthread_key_create", h_key_create);
    hle_bind("pthread_getspecific", h_getspecific);
    hle_bind("pthread_setspecific", h_setspecific);

    hle_bind("sem_init", h_sem_init);
    hle_bind("sem_post", h_sem_post);
    hle_bind("sem_wait", h_sem_wait);
    hle_bind("sem_destroy", h_sem_destroy);
}

#else   /* !_WIN32 */

/* POSIX, on a 64-bit host. The same shapes as above: a guest synchronisation
 * object holds a GuestObj pointing at the host one, created on first use; a
 * guest thread is a host thread with its own guest stack in the window. */
#include <errno.h>
#include <pthread.h>
#include <time.h>

static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

static void *obj_get(uint32_t va, size_t host_size, void (*ctor)(void *))
{
    GuestObj *g = (GuestObj *)GP(va);
    if (!g) return NULL;
    if (g->magic == GUEST_MAGIC && g->host)
        return g->host;
    pthread_mutex_lock(&g_init_lock);
    if (g->magic != GUEST_MAGIC || !g->host) {
        void *h = calloc(1, host_size);
        if (h) ctor(h);
        g->host  = h;
        g->magic = GUEST_MAGIC;
    }
    pthread_mutex_unlock(&g_init_lock);
    return g->host;
}

static void obj_drop(uint32_t va, void (*dtor)(void *))
{
    GuestObj *g = (GuestObj *)GP(va);
    if (g && g->magic == GUEST_MAGIC && g->host) {
        if (dtor) dtor(g->host);
        free(g->host);
        g->host = NULL;
        g->magic = 0;
    }
}

/* The kind the game asked for, as glibc keeps it (0 normal, 1 recursive,
 * 2 error-checking, 3 adaptive): a normal mutex may be unlocked by a thread
 * that did not lock it, and the game's sound library does that now and then -
 * as recursive, every mutex refused it (EPERM), and start-up failed in
 * "CRICRS_Leave : pthread_mutex_unlock failed". Read before the GuestObj,
 * 16 bytes here, covers glibc's __kind at offset 12. */
static __thread int t_mutex_kind;
static void ctor_mutex(void *p)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    int k = t_mutex_kind & 3;
    pthread_mutexattr_settype(&a, k == 1 ? PTHREAD_MUTEX_RECURSIVE : k == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init((pthread_mutex_t *)p, &a);
    pthread_mutexattr_destroy(&a);
}
static pthread_mutex_t *mutex_get(uint32_t va)
{
    GuestObj *g = (GuestObj *)GP(va);
    if (g && g->magic == GUEST_MAGIC && g->host) return (pthread_mutex_t *)g->host;
    t_mutex_kind = g ? (int)rd32(va + 12) : 0;      /* a static initialiser's kind */
    return (pthread_mutex_t *)obj_get(va, sizeof(pthread_mutex_t), ctor_mutex);
}
static void ctor_cond(void *p) { pthread_cond_init((pthread_cond_t *)p, NULL); }
static void dtor_mutex(void *p) { pthread_mutex_destroy((pthread_mutex_t *)p); }
static void dtor_cond(void *p)  { pthread_cond_destroy((pthread_cond_t *)p); }

#define MUTEX(n) mutex_get(A32(n))
#define COND(n)  ((pthread_cond_t *)obj_get(A32(n), sizeof(pthread_cond_t), ctor_cond))

/* Who holds each guest mutex, and what each thread waits for: the hang
 * watchdog prints them (guest_mutex_report), so a deadlock names its holder.
 * The game's sound library sometimes stalls start-up this way. */
#define MOWN 256
static struct { volatile uint32_t va; volatile unsigned long tid; volatile int depth; } g_mown[MOWN];
static __thread uint32_t t_waiting;
static __thread uint32_t t_cond_mutex; static __thread int t_cond_depth;  /* sleeping on a condition */
static int mown_slot(uint32_t va)
{
    unsigned h = (va >> 2) * 2654435761u % MOWN;
    for (unsigned i = 0; i < MOWN; i++, h = (h + 1) % MOWN) {
        if (g_mown[h].va == va) return (int)h;
        if (!g_mown[h].va && __sync_bool_compare_and_swap(&g_mown[h].va, 0, va)) return (int)h;
    }
    return -1;
}
/* pthread_mutex_init(m, attr): attr is glibc's int __mutexkind */
static void h_mutex_init(CPU *c)
{
    obj_drop(A32(0), dtor_mutex);
    t_mutex_kind = A32(1) ? (int)rd32(A32(1)) : 0;
    (void)obj_get(A32(0), sizeof(pthread_mutex_t), ctor_mutex);
    RET(0);
}
static void h_mutex_lock(CPU *c)
{
    uint32_t va = A32(0);
    pthread_mutex_t *m = MUTEX(0);
    t_waiting = va;
    int r = pthread_mutex_lock(m);
    t_waiting = 0;
    int k = mown_slot(va);
    if (!r && k >= 0) { g_mown[k].tid = (unsigned long)pthread_self(); g_mown[k].depth++; }
    RET(r);
}
static void h_mutex_unlock(CPU *c)
{
    int k = mown_slot(A32(0));
    if (k >= 0 && g_mown[k].depth > 0 && --g_mown[k].depth == 0) g_mown[k].tid = 0;
    RET(pthread_mutex_unlock(MUTEX(0)));
}
static void h_mutex_trylock(CPU *c)
{
    int r = pthread_mutex_trylock(MUTEX(0));
    int k = r ? -1 : mown_slot(A32(0));
    if (k >= 0) { g_mown[k].tid = (unsigned long)pthread_self(); g_mown[k].depth++; }
    RET(r ? 16 : 0);
}

void guest_mutex_report(void)
{
    if (t_waiting) {
        int k = mown_slot(t_waiting);
        fprintf(stderr, "waiting for guest mutex %#x, held by thread %#lx (depth %d)\n", t_waiting,
                k >= 0 ? g_mown[k].tid : 0, k >= 0 ? g_mown[k].depth : 0);
    }
    if (t_cond_mutex)
        fprintf(stderr, "sleeping on a condition with guest mutex %#x, locked %d deep\n", t_cond_mutex, t_cond_depth);
    for (int i = 0; i < MOWN; i++)
        if (g_mown[i].va && g_mown[i].depth > 0 && g_mown[i].tid == (unsigned long)pthread_self())
            fprintf(stderr, "holding guest mutex %#x (depth %d)\n", g_mown[i].va, g_mown[i].depth);
}
static void h_mutex_destroy(CPU *c) { obj_drop(A32(0), dtor_mutex); RET(0); }

static void h_cond_init(CPU *c)      { (void)COND(0); RET(0); }
static void h_cond_signal(CPU *c)    { pthread_cond_signal(COND(0)); RET(0); }
static void h_cond_broadcast(CPU *c) { pthread_cond_broadcast(COND(0)); RET(0); }
/* waiting on a condition lets its mutex go meanwhile: so does the record */
static int cond_release(uint32_t va, int *depth)
{
    int k = mown_slot(va);
    *depth = k >= 0 ? g_mown[k].depth : 0;
    if (k >= 0) { g_mown[k].depth = 0; g_mown[k].tid = 0; }
    t_cond_mutex = va; t_cond_depth = *depth;
    return k;
}
static void cond_reacquire(int k, int depth)
{
    if (k >= 0) { g_mown[k].tid = (unsigned long)pthread_self(); g_mown[k].depth = depth; }
    t_cond_mutex = 0;
}
static void h_cond_wait(CPU *c)
{
    int d, k = cond_release(A32(1), &d);
    pthread_cond_wait(COND(0), MUTEX(1));
    cond_reacquire(k, d);
    RET(0);
}
static void h_cond_timedwait(CPU *c)
{
    uint32_t ts = A32(2);
    int d, k = cond_release(A32(1), &d), r = 0;
    if (!ts) pthread_cond_wait(COND(0), MUTEX(1));
    else {
        struct timespec t = { (time_t)(int32_t)rd32(ts), (long)(int32_t)rd32(ts + 4) };
        r = pthread_cond_timedwait(COND(0), MUTEX(1), &t);
    }
    cond_reacquire(k, d);
    RET(r == ETIMEDOUT ? 110u : 0u);                    /* Linux ETIMEDOUT */
}
static void h_cond_destroy(CPU *c) { obj_drop(A32(0), dtor_cond); RET(0); }

typedef struct {
    uint32_t magic;
    uint32_t stacksize;
    int32_t  policy;
    int32_t  priority;
    int32_t  detached;
} GuestAttr;

#define GUEST_STACK_DEFAULT (2u << 20)

static GuestAttr *attr_of(uint32_t va)
{
    GuestAttr *a = (GuestAttr *)GP(va);
    if (a->magic != GUEST_MAGIC) {
        memset(a, 0, sizeof *a);
        a->magic = GUEST_MAGIC;
        a->stacksize = GUEST_STACK_DEFAULT;
    }
    return a;
}

static void h_attr_init(CPU *c)         { (void)attr_of(A32(0)); RET(0); }
static void h_attr_setstacksize(CPU *c) { attr_of(A32(0))->stacksize = A32(1); RET(0); }
static void h_attr_setschedpolicy(CPU *c) { attr_of(A32(0))->policy = AI32(1); RET(0); }
static void h_attr_setschedparam(CPU *c)
{
    if (A32(1)) attr_of(A32(0))->priority = (int32_t)rd32(A32(1));
    RET(0);
}
static void h_sched_get_priority_max(CPU *c) { RET(0); }
static void h_sched_get_priority_min(CPU *c) { RET(0); }
static void h_mutexattr_init(CPU *c)    { if (A32(0)) wr32(A32(0), 0); RET(0); }
/* glibc keeps the kind in the attribute's int: pthread_mutex_init reads it
 * there (POSIX; the Windows side is always recursive) */
static void h_mutexattr_settype(CPU *c) { if (A32(0)) wr32(A32(0), A32(1)); RET(0); }

/* The GuestThread lives in guest memory, because its address IS the guest's
 * pthread_t; its stack does too, because guest code addresses it. */
typedef struct {
    uint32_t  start;
    uint32_t  arg;
    void     *stack;
    uint32_t  stacksize;
    uint32_t  retval;
    pthread_t handle;
} GuestThread;

/* Host stack for a guest thread. Every guest call is a host call, so deep guest
 * recursion costs host stack; give it room. */
#define HOST_THREAD_STACK (64u << 20)

/* Every guest thread's host handle, for the hang watchdog to ask each where it is. */
static pthread_t g_threads[64];
static unsigned g_nthreads;
unsigned guest_threads(pthread_t *out, unsigned max)
{
    unsigned n = __atomic_load_n(&g_nthreads, __ATOMIC_ACQUIRE);
    if (n > max) n = max;
    memcpy(out, g_threads, n * sizeof *out);
    return n;
}

/* pthread_self: the pthread_t pthread_create handed out for a guest thread,
 * a distinct number for any other (main, audio). Unique per thread - the
 * game's recursive locks compare owners by it. */
static __thread uint32_t g_self;
static uint32_t g_self_next = 0xFFF00001u;   /* above any guest address */

static void *thread_trampoline(void *p)
{
    GuestThread *t = (GuestThread *)p;
    g_self = H2G(t);
    {
        unsigned i = __atomic_fetch_add(&g_nthreads, 1, __ATOMIC_ACQ_REL);
        if (i < 64) g_threads[i] = pthread_self(); else g_nthreads = 64;
    }
    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    guest_set_current_cpu(&cpu);
    guest_install_thread_handlers();
    uint32_t top = H2G(t->stack) + t->stacksize - 64;
    cpu.esp = top & ~15u;
    uint32_t args[1] = { t->arg };
    t->retval = guest_call(&cpu, t->start, args, 1);
    return NULL;
}

static void h_pthread_create(CPU *c)
{
    uint32_t out = A32(0), attr = A32(1);
    GuestThread *t = (GuestThread *)guest_alloc(sizeof *t, 16);
    if (!t) { RET(11); return; }                          /* EAGAIN */
    memset(t, 0, sizeof *t);
    t->start     = A32(2);
    t->arg       = A32(3);
    t->stacksize = attr ? attr_of(attr)->stacksize : GUEST_STACK_DEFAULT;
    if (t->stacksize < (256u << 10)) t->stacksize = 256u << 10;
    t->stack = guest_alloc(t->stacksize, 64);
    if (!t->stack) { guest_free(t); RET(11); return; }

    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, HOST_THREAD_STACK);
    int r = pthread_create(&t->handle, &a, thread_trampoline, t);
    pthread_attr_destroy(&a);
    if (r) { guest_free(t->stack); guest_free(t); RET(11); return; }

    if (out) wr32(out, H2G(t));
    fprintf(stderr, "[pthread] created thread at %#010x, %u KB stack\n",
            t->start, t->stacksize >> 10);
    RET(0);
}

static void h_pthread_join(CPU *c)
{
    GuestThread *t = (GuestThread *)GP(A32(0));
    if (t) {
        pthread_join(t->handle, NULL);
        if (A32(1)) wr32(A32(1), t->retval);
        guest_free(t->stack);
        guest_free(t);
    }
    RET(0);
}

static void h_pthread_self(CPU *c)
{
    if (!g_self) g_self = __atomic_fetch_add(&g_self_next, 1, __ATOMIC_RELAXED);
    RET(g_self);
}
static void h_pthread_detach(CPU *c) { RET(0); }
static void h_pthread_equal(CPU *c)  { RET(A32(0) == A32(1)); }
static void h_pthread_getschedparam(CPU *c) { RET(0); }
static void h_pthread_setschedparam(CPU *c) { RET(0); }

static void h_pthread_once(CPU *c)
{
    uint32_t flag = A32(0), fn = A32(1);
    if (flag && !rd32(flag)) {
        wr32(flag, 1);
        guest_call(c, fn, NULL, 0);
    }
    RET(0);
}

static void h_key_create(CPU *c)
{
    pthread_key_t k;
    if (pthread_key_create(&k, NULL)) { RET(12); return; }     /* ENOMEM */
    if (A32(0)) wr32(A32(0), (uint32_t)k);
    RET(0);
}
static void h_getspecific(CPU *c) { RET((uint32_t)(uintptr_t)pthread_getspecific((pthread_key_t)A32(0))); }
static void h_setspecific(CPU *c) { pthread_setspecific((pthread_key_t)A32(0), (void *)(uintptr_t)A32(1)); RET(0); }

/* A counting semaphore from a mutex and a condition: macOS has no unnamed
 * sem_init. */
typedef struct { pthread_mutex_t m; pthread_cond_t cv; uint32_t count; } HostSem;
static void ctor_sem(void *p)
{
    HostSem *s = (HostSem *)p;
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->cv, NULL);
}
static void dtor_sem(void *p)
{
    HostSem *s = (HostSem *)p;
    pthread_mutex_destroy(&s->m);
    pthread_cond_destroy(&s->cv);
}
#define SEM(n) ((HostSem *)obj_get(A32(n), sizeof(HostSem), ctor_sem))

static void h_sem_init(CPU *c)
{
    HostSem *s = SEM(0);
    pthread_mutex_lock(&s->m);
    s->count = A32(2);
    /* The game's I/O thread can reach its first sem_wait before the main
     * thread has initialised the semaphore; it must be woken by the count
     * set here, or it sleeps for good and the I/O board is never polled. */
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
    RET(0);
}
static void h_sem_post(CPU *c)
{
    HostSem *s = SEM(0);
    pthread_mutex_lock(&s->m);
    s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->m);
    RET(0);
}
static void h_sem_wait(CPU *c)
{
    HostSem *s = SEM(0);
    pthread_mutex_lock(&s->m);
    int waited = 0;
    while (!s->count) {
        /* A thread that has waited seconds on a semaphore has usually lost
         * a post; say which one, so a stalled game thread can be traced. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        if (pthread_cond_timedwait(&s->cv, &s->m, &ts) == ETIMEDOUT && !waited++)
            fprintf(stderr, "[pthread] sem_wait(%#x) blocked over 2 s\n", A32(0));
    }
    if (waited) fprintf(stderr, "[pthread] sem_wait(%#x) went through after %d s\n", A32(0), waited * 2);
    s->count--;
    pthread_mutex_unlock(&s->m);
    RET(0);
}
static void h_sem_destroy(CPU *c) { obj_drop(A32(0), dtor_sem); RET(0); }

/* sem_open(name, oflag[, mode, value]): a named semaphore is a sem_t the
 * library allocates. Nothing else on this host shares the name, so a fresh
 * one per open is the whole of it. */
static void h_sem_open(CPU *c)
{
    void *p = guest_alloc(16, 16);
    memset(p, 0, 16);
    uint32_t va = H2G(p);
    HostSem *s = (HostSem *)obj_get(va, sizeof(HostSem), ctor_sem);
    pthread_mutex_lock(&s->m);
    s->count = (A32(1) & 0x40) ? A32(3) : 0;     /* O_CREAT carries the value */
    pthread_mutex_unlock(&s->m);
    RET(va);
}
static void h_sem_close(CPU *c) { RET(0); }

/* pthread_cancel: the game cancels its worker threads only on exit. */
static void h_pthread_cancel(CPU *c)
{
    fprintf(stderr, "[pthread] pthread_cancel(%#x) ignored\n", A32(0));
    RET(0);
}

void hle_register_pthread(void)
{
    hle_bind("pthread_mutex_init", h_mutex_init);
    hle_bind("sem_open", h_sem_open);
    hle_bind("sem_close", h_sem_close);
    hle_bind("pthread_cancel", h_pthread_cancel);
    hle_bind("pthread_mutex_lock", h_mutex_lock);
    hle_bind("pthread_mutex_unlock", h_mutex_unlock);
    hle_bind("pthread_mutex_trylock", h_mutex_trylock);
    hle_bind("pthread_mutex_destroy", h_mutex_destroy);
    hle_bind("pthread_mutexattr_init", h_mutexattr_init);
    hle_bind("pthread_mutexattr_settype", h_mutexattr_settype);
    hle_bind("pthread_cond_init", h_cond_init);
    hle_bind("pthread_cond_signal", h_cond_signal);
    hle_bind("pthread_cond_broadcast", h_cond_broadcast);
    hle_bind("pthread_cond_wait", h_cond_wait);
    hle_bind("pthread_cond_timedwait", h_cond_timedwait);
    hle_bind("pthread_cond_destroy", h_cond_destroy);
    hle_bind("pthread_attr_init", h_attr_init);
    hle_bind("pthread_attr_setstacksize", h_attr_setstacksize);
    hle_bind("pthread_attr_setschedpolicy", h_attr_setschedpolicy);
    hle_bind("pthread_attr_setschedparam", h_attr_setschedparam);
    hle_bind("sched_get_priority_max", h_sched_get_priority_max);
    hle_bind("sched_get_priority_min", h_sched_get_priority_min);
    hle_bind("pthread_create", h_pthread_create);
    hle_bind("pthread_join", h_pthread_join);
    hle_bind("pthread_detach", h_pthread_detach);
    hle_bind("pthread_self", h_pthread_self);
    hle_bind("pthread_equal", h_pthread_equal);
    hle_bind("pthread_once", h_pthread_once);
    hle_bind("pthread_getschedparam", h_pthread_getschedparam);
    hle_bind("pthread_setschedparam", h_pthread_setschedparam);
    hle_bind("pthread_key_create", h_key_create);
    hle_bind("pthread_getspecific", h_getspecific);
    hle_bind("pthread_setspecific", h_setspecific);
    hle_bind("sem_init", h_sem_init);
    hle_bind("sem_post", h_sem_post);
    hle_bind("sem_wait", h_sem_wait);
    hle_bind("sem_destroy", h_sem_destroy);
}
#endif
