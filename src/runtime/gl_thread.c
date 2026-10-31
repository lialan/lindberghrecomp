/* gl_thread.c - the game's GL calls run on a thread of their own.
 *
 * The game draws on the thread that runs its logic, so a frame costs the sum
 * of both. On the Quest that is about 13 ms of game code plus 12 ms of GL -
 * gl4es turning fixed-function and ARB-program calls into GLES, and the
 * driver - for a busy scene: 25 ms, 40 fps. With the GL on a worker, the game
 * thread copies each call into a queue and goes on; the worker makes the
 * calls, one frame behind. The frame then costs the larger of the two.
 *
 * A call goes into the queue only when its arguments can be copied: values,
 * or arrays of a size known from the call itself (a matrix, a uniform
 * vector, glMultiDrawElements' counts and offsets). Vertex arrays in buffer
 * objects are offsets and copy as values; a draw from client memory, which
 * the driver reads at the draw, waits instead. So does every call this file
 * does not know, and every call that answers (glGet*, glGen*, glMapBuffer):
 * the game thread hands it to the worker and waits for it. A busy frame of
 * The House of the Dead 4 Special makes 35,000 GL calls and 6 that answer.
 *
 * The worker starts at the first swap, after all the setup the game does on
 * its own thread, and takes the GL context from it. host_swap() then queues
 * the frame's end - the second eye, the headset or the window - as one job,
 * and waits only for the frame before it, so the game is never more than a
 * frame ahead.
 *
 * LINDBERGH_GL_THREAD=1 turns it on (or a game's profile); =0 off. */
#include "lindbergh_rt.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define RING_SIZE   (16u << 20)        /* bytes of guest memory: two busy frames */
#define ARGS        16u                /* dwords of the guest stack copied per call */

enum { R_ASYNC = 1, R_SYNC, R_JOB, R_WRAP };

/* One record in the ring: this header, then the copied arguments, then any
 * arrays they point at - all in guest memory, so a handler reads them through
 * the usual A32/APTR at the address the copy has. */
typedef struct {
    uint32_t size;                     /* bytes, header included */
    uint32_t kind;
    uint32_t id;
    uint32_t seq;
    HleHandler fn;
    union { CPU *c; void (*job)(void *); } u;
    void *arg;
} Rec;
#define HDR ((uint32_t)((sizeof(Rec) + 15u) & ~15u))

static uint8_t *g_ring;                /* host address */
static uint32_t g_ring_va;             /* guest address */
/* bytes published, bytes done - each on a cache line of its own: they are
 * written by different threads, tens of thousands of times a frame */
static _Alignas(128) _Atomic uint64_t g_head;
static _Alignas(128) _Atomic uint64_t g_tail;
static _Alignas(128) uint64_t g_head_mine;   /* bytes written: the game thread's, published as g_head */
static _Alignas(128) _Atomic uint32_t g_done_seq;    /* the last sync record or job done */
static uint32_t g_seq;                 /* the game thread's numbering */
static GlHook g_prev;                  /* the hook the worker runs: stereo.c's, or none */
static int g_on;
static pthread_t g_thread;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static _Alignas(128) _Atomic int g_worker_sleeps;
static _Alignas(128) _Atomic int g_game_sleeps;
static void (*g_take_context)(void);   /* the worker's first act */

int glq_active(void) { return g_on; }

/* ---- what may go into the queue ---- */

enum {
    C_SYNC = 0,     /* hand to the worker and wait */
    C_VAL,          /* values only */
    C_BLOB,         /* one array of a fixed size: arg a, b bytes */
    C_VEC,          /* arg a points at count (arg b) elements of c bytes */
    C_BIND,         /* glBindBuffer: tracked here */
    C_ENABLE_CS, C_DISABLE_CS, C_CLIENT_TEX, C_ENABLE_VA, C_DISABLE_VA,
    C_PTR,          /* an array pointer: arg a; slot by kind b */
    C_DRAW,         /* glDrawArrays */
    C_DRAW_EL,      /* indices at arg a */
    C_MULTI_EL,     /* glMultiDrawElements */
    C_LOCK,         /* glLockArraysEXT: reads the arrays now */
    C_UNLOCK,       /* glUnlockArraysEXT */
    C_DIRECT,       /* glXSwapBuffers: here, on the game thread (host_swap queues its GL) */
    C_RAW,          /* the rest of GLX: on the worker, waited for, past stereo.c's hook */
};
typedef struct { uint8_t kind, a, b, c; } Cls;
static Cls g_cls[HLE_ID_MAX];
static uint8_t g_cls_ok[HLE_ID_MAX];

/* the client arrays as the game set them: on, and whether in client memory */
enum { S_VERTEX, S_NORMAL, S_COLOR, S_SECONDARY, S_FOG, S_TEX0, S_ATTR0 = S_TEX0 + 8, S_N = S_ATTR0 + 16 };
static uint8_t g_slot_on[S_N], g_slot_client[S_N];
static uint32_t g_array_buf, g_element_buf;
/* Each client array's pointer call as the game made it (and the client
 * texture unit it was for): a locked range of it is copied (lock_copy) and
 * the call made again for the copy, then for the game's own memory at unlock */
static struct { uint8_t set, unit, copied; HleId id; HleHandler fn; uint32_t args[16]; uint8_t a; } g_ptr[S_N];
static HleId g_cat_id; static HleHandler g_cat_fn;   /* glClientActiveTexture, for a texture unit's pointer */
static unsigned g_client_unit;

static const struct { const char *name; uint8_t kind, a, b, c; } T[] = {
    /* values */
    { "glEnable", C_VAL }, { "glDisable", C_VAL }, { "glMatrixMode", C_VAL }, { "glLoadIdentity", C_VAL },
    { "glPushMatrix", C_VAL }, { "glPopMatrix", C_VAL }, { "glTranslatef", C_VAL }, { "glScalef", C_VAL },
    { "glRotatef", C_VAL }, { "glBindTexture", C_VAL }, { "glActiveTexture", C_VAL }, { "glActiveTextureARB", C_VAL },
    { "glTexParameteri", C_VAL }, { "glTexParameterf", C_VAL }, { "glTexEnvf", C_VAL }, { "glTexEnvi", C_VAL },
    { "glFogf", C_VAL }, { "glFogi", C_VAL }, { "glDepthMask", C_VAL }, { "glDepthFunc", C_VAL },
    { "glColorMask", C_VAL }, { "glShadeModel", C_VAL }, { "glPolygonMode", C_VAL }, { "glAlphaFunc", C_VAL },
    { "glBlendFunc", C_VAL }, { "glBlendEquation", C_VAL }, { "glBlendEquationEXT", C_VAL }, { "glBlendColor", C_VAL },
    { "glBlendColorEXT", C_VAL }, { "glBlendFuncSeparate", C_VAL }, { "glBlendFuncSeparateEXT", C_VAL },
    { "glCullFace", C_VAL }, { "glFrontFace", C_VAL }, { "glPolygonOffset", C_VAL }, { "glLineWidth", C_VAL },
    { "glPointSize", C_VAL }, { "glViewport", C_VAL }, { "glScissor", C_VAL }, { "glDepthRange", C_VAL },
    { "glClear", C_VAL }, { "glClearColor", C_VAL }, { "glClearDepth", C_VAL }, { "glClearStencil", C_VAL },
    { "glStencilMask", C_VAL }, { "glStencilFunc", C_VAL }, { "glStencilOp", C_VAL },
    { "glLightModeli", C_VAL }, { "glLightModelf", C_VAL }, { "glLightf", C_VAL }, { "glLighti", C_VAL },
    { "glMaterialf", C_VAL }, { "glMateriali", C_VAL }, { "glColorMaterial", C_VAL }, { "glHint", C_VAL },
    { "glColor4ub", C_VAL }, { "glColor4f", C_VAL }, { "glColor3f", C_VAL }, { "glColor3ub", C_VAL },
    { "glNormal3f", C_VAL }, { "glVertex2f", C_VAL }, { "glVertex3f", C_VAL }, { "glVertex2i", C_VAL },
    { "glTexCoord2f", C_VAL }, { "glMultiTexCoord2f", C_VAL }, { "glMultiTexCoord2fARB", C_VAL },
    { "glSecondaryColor3f", C_VAL }, { "glSecondaryColor3fEXT", C_VAL },
    { "glVertexAttrib1fARB", C_VAL }, { "glVertexAttrib2fARB", C_VAL }, { "glVertexAttrib3fARB", C_VAL },
    { "glVertexAttrib4fARB", C_VAL }, { "glBegin", C_VAL }, { "glEnd", C_VAL },
    { "glUseProgramObjectARB", C_VAL }, { "glUseProgram", C_VAL }, { "glBindProgramARB", C_VAL },
    { "glUniform1iARB", C_VAL }, { "glUniform1fARB", C_VAL }, { "glUniform2fARB", C_VAL },
    { "glUniform3fARB", C_VAL }, { "glUniform4fARB", C_VAL }, { "glUniform1i", C_VAL }, { "glUniform1f", C_VAL },
    { "glProgramEnvParameter4fARB", C_VAL }, { "glProgramLocalParameter4fARB", C_VAL },
    { "glCopyTexSubImage2D", C_VAL }, { "glPixelStorei", C_VAL }, { "glTexGeni", C_VAL },
    { "glBindFramebufferEXT", C_VAL }, { "glBindFramebuffer", C_VAL }, { "glBindRenderbufferEXT", C_VAL },
    { "glFlush", C_VAL }, { "glUnlockArraysEXT", C_UNLOCK },
    /* one array, its size fixed by the call */
    { "glMaterialfv", C_BLOB, 2, 16 }, { "glLightfv", C_BLOB, 2, 16 }, { "glFogfv", C_BLOB, 1, 16 },
    { "glLightModelfv", C_BLOB, 1, 16 }, { "glColor4fv", C_BLOB, 0, 16 }, { "glColor3fv", C_BLOB, 0, 12 },
    { "glSecondaryColor3fv", C_BLOB, 0, 12 }, { "glSecondaryColor3fvEXT", C_BLOB, 0, 12 },
    { "glVertex3fv", C_BLOB, 0, 12 }, { "glVertex2fv", C_BLOB, 0, 8 }, { "glNormal3fv", C_BLOB, 0, 12 },
    { "glTexCoord2fv", C_BLOB, 0, 8 }, { "glLoadMatrixf", C_BLOB, 0, 64 }, { "glMultMatrixf", C_BLOB, 0, 64 },
    { "glTexEnvfv", C_BLOB, 2, 16 }, { "glTexParameterfv", C_BLOB, 2, 16 },
    { "glProgramEnvParameter4fvARB", C_BLOB, 2, 16 }, { "glProgramLocalParameter4fvARB", C_BLOB, 2, 16 },
    { "glVertexAttrib3fvARB", C_BLOB, 1, 12 }, { "glVertexAttrib4fvARB", C_BLOB, 1, 16 },
    /* count elements */
    { "glUniform1fvARB", C_VEC, 2, 1, 4 }, { "glUniform2fvARB", C_VEC, 2, 1, 8 },
    { "glUniform3fvARB", C_VEC, 2, 1, 12 }, { "glUniform4fvARB", C_VEC, 2, 1, 16 },
    { "glUniform1ivARB", C_VEC, 2, 1, 4 }, { "glUniform4fv", C_VEC, 2, 1, 16 },
    { "glUniformMatrix3fvARB", C_VEC, 3, 1, 36 }, { "glUniformMatrix4fvARB", C_VEC, 3, 1, 64 },
    { "glUniformMatrix4fv", C_VEC, 3, 1, 64 },
    /* the client arrays */
    { "glBindBuffer", C_BIND }, { "glBindBufferARB", C_BIND },
    { "glEnableClientState", C_ENABLE_CS }, { "glDisableClientState", C_DISABLE_CS },
    { "glClientActiveTexture", C_CLIENT_TEX }, { "glClientActiveTextureARB", C_CLIENT_TEX },
    { "glEnableVertexAttribArrayARB", C_ENABLE_VA }, { "glDisableVertexAttribArrayARB", C_DISABLE_VA },
    { "glEnableVertexAttribArray", C_ENABLE_VA }, { "glDisableVertexAttribArray", C_DISABLE_VA },
    { "glVertexPointer", C_PTR, 3, S_VERTEX }, { "glNormalPointer", C_PTR, 2, S_NORMAL },
    { "glColorPointer", C_PTR, 3, S_COLOR }, { "glSecondaryColorPointer", C_PTR, 3, S_SECONDARY },
    { "glSecondaryColorPointerEXT", C_PTR, 3, S_SECONDARY }, { "glFogCoordPointer", C_PTR, 2, S_FOG },
    { "glFogCoordPointerEXT", C_PTR, 2, S_FOG }, { "glTexCoordPointer", C_PTR, 3, S_TEX0 },
    { "glVertexAttribPointerARB", C_PTR, 5, S_ATTR0 }, { "glVertexAttribPointer", C_PTR, 5, S_ATTR0 },
    { "glDrawArrays", C_DRAW }, { "glDrawElements", C_DRAW_EL, 3 }, { "glDrawRangeElements", C_DRAW_EL, 5 },
    { "glMultiDrawElements", C_MULTI_EL }, { "glMultiDrawElementsEXT", C_MULTI_EL },
    { "glLockArraysEXT", C_LOCK },
};

static const Cls *cls(HleId id)
{
    if (g_cls_ok[id]) return &g_cls[id];
    const char *n = hle_name(id);
    Cls k = { C_SYNC, 0, 0, 0 };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(n, T[i].name)) { k.kind = T[i].kind; k.a = T[i].a; k.b = T[i].b; k.c = T[i].c; break; }
    if (!strncmp(n, "glX", 3)) k.kind = strcmp(n, "glXSwapBuffers") ? C_RAW : C_DIRECT;
    g_cls[id] = k; g_cls_ok[id] = 1;
    return &g_cls[id];
}

static int slot_of(uint32_t array)
{
    switch (array) {
    case 0x8074: return S_VERTEX;   case 0x8075: return S_NORMAL;
    case 0x8076: return S_COLOR;    case 0x845E: return S_SECONDARY;
    case 0x8457: return S_FOG;      case 0x8078: return S_TEX0 + (int)(g_client_unit & 7);
    }
    return -1;
}

static int client_arrays_live(void)
{
    for (int i = 0; i < S_N; i++) if (g_slot_on[i] && g_slot_client[i]) return 1;
    return 0;
}

/* ---- the ring ---- */

/* Publish, then look whether the other side sleeps: the fence keeps the two
 * in that order (it pairs with the sleeper's flag store before its last look
 * at the queue), so a wakeup is never lost. */
static void wake_worker(void)
{
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&g_worker_sleeps, memory_order_relaxed)) {
        pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    }
}

static void wake_game(void)
{
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&g_game_sleeps, memory_order_relaxed)) {
        pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    }
}

static double g_wait_ms;               /* the game thread's time waiting on the worker */
static _Atomic double g_work_ms;       /* the worker's CPU time, as of its last frame */
static double now_ms(clockid_t k) { struct timespec t; clock_gettime(k, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* Wait on the game thread until cond holds: a short spin, then sleep. */
#define GAME_WAIT(cond) do { double _w0 = (cond) ? 0 : now_ms(CLOCK_MONOTONIC); \
    for (int _i = 0; !(cond) && _i < 4000; _i++) __asm__ volatile("" ::: "memory"); \
    if (!(cond)) { pthread_mutex_lock(&g_mu); atomic_store(&g_game_sleeps, 1); \
        atomic_thread_fence(memory_order_seq_cst); \
        while (!(cond)) pthread_cond_wait(&g_cv, &g_mu); \
        atomic_store(&g_game_sleeps, 0); pthread_mutex_unlock(&g_mu); } \
    if (_w0) g_wait_ms += now_ms(CLOCK_MONOTONIC) - _w0; } while (0)

/* Records are written at g_head_mine and published to the worker as g_head
 * every 32 (ring_put), or at once before anything the game then waits for
 * (ring_put_wake): a store the worker reads on every record, made for each of
 * 40,000 calls a frame, cost more than the calls' own copying. Publishing
 * looks whether the worker sleeps, which costs a full fence (wake_worker). */
static unsigned g_unpublished;
static void publish(void)
{
    atomic_store_explicit(&g_head, g_head_mine, memory_order_release);
    g_unpublished = 0;
    wake_worker();
}

/* Room for n bytes (header included) at the head: a record never wraps. */
static Rec *ring_take(uint32_t n)
{
    n = (n + 15u) & ~15u;
    uint64_t h = g_head_mine;
    uint32_t off = (uint32_t)(h % RING_SIZE);
    /* the worker's tail as last read: the real one (the cache line the worker
     * keeps writing) is read only when that says the ring may be full */
    static uint64_t tail_seen;
    if (off + n > RING_SIZE) {         /* to the end, a marker the worker skips */
        uint32_t pad = RING_SIZE - off;
        if (h + pad + n - tail_seen > RING_SIZE) tail_seen = atomic_load_explicit(&g_tail, memory_order_acquire);
        if (h + pad - tail_seen > RING_SIZE) publish();   /* about to wait for room */
        GAME_WAIT(h + pad - atomic_load_explicit(&g_tail, memory_order_acquire) <= RING_SIZE);
        Rec *w = (Rec *)(g_ring + off);
        w->size = pad; w->kind = R_WRAP;
        h += pad;
        g_head_mine = h;
        off = 0;
    }
    if (h + n - tail_seen > RING_SIZE) {
        tail_seen = atomic_load_explicit(&g_tail, memory_order_acquire);
        if (h + n - tail_seen > RING_SIZE) publish();
        GAME_WAIT(h + n - (tail_seen = atomic_load_explicit(&g_tail, memory_order_acquire)) <= RING_SIZE);
    }
    Rec *r = (Rec *)(g_ring + off);
    r->size = n;
    return r;
}

/* the record at the head is complete */
static void ring_put(void)
{
    g_head_mine += ((Rec *)(g_ring + (uint32_t)(g_head_mine % RING_SIZE)))->size;
    if (++g_unpublished >= 32) publish();
}
static void ring_put_wake(void)
{
    g_head_mine += ((Rec *)(g_ring + (uint32_t)(g_head_mine % RING_SIZE)))->size;
    publish();
}

static uint32_t va_of(const void *p) { return g_ring_va + (uint32_t)((const uint8_t *)p - g_ring); }

/* ---- the worker ---- */

static void *worker(void *arg)
{
    (void)arg;
    if (g_take_context) g_take_context();
    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    for (;;) {
        uint64_t t = atomic_load_explicit(&g_tail, memory_order_relaxed);
        if (atomic_load_explicit(&g_head, memory_order_acquire) == t) {
            for (int i = 0; i < 2000 && atomic_load_explicit(&g_head, memory_order_acquire) == t; i++)
                __asm__ volatile("" ::: "memory");
            if (atomic_load_explicit(&g_head, memory_order_acquire) == t) {
                pthread_mutex_lock(&g_mu);
                atomic_store(&g_worker_sleeps, 1);
                atomic_thread_fence(memory_order_seq_cst);
                while (atomic_load_explicit(&g_head, memory_order_acquire) == t)
                    pthread_cond_wait(&g_cv, &g_mu);
                atomic_store(&g_worker_sleeps, 0);
                pthread_mutex_unlock(&g_mu);
            }
            continue;
        }
        Rec *r = (Rec *)(g_ring + (uint32_t)(t % RING_SIZE));
        uint32_t size = r->size;
        switch (r->kind) {
        case R_ASYNC:
            /* a GL handler reads its arguments off esp and answers in eax:
             * the rest of the CPU is cleared once, not for every call (352
             * bytes, 40,000 times a frame) */
            cpu.eax = cpu.edx = 0; cpu.fpu_top = 0;
            cpu.esp = va_of((uint8_t *)r + HDR);
            if (g_prev && cls((HleId)r->id)->kind != C_RAW) g_prev(&cpu, (HleId)r->id, r->fn); else r->fn(&cpu);
            break;
        case R_SYNC:
            if (g_prev && cls((HleId)r->id)->kind != C_RAW) g_prev(r->u.c, (HleId)r->id, r->fn);
            else r->fn(r->u.c);
            atomic_store_explicit(&g_done_seq, r->seq, memory_order_release);
            break;
        case R_JOB:
            r->u.job(r->arg);
            atomic_store(&g_work_ms, now_ms(CLOCK_THREAD_CPUTIME_ID));
            atomic_store_explicit(&g_done_seq, r->seq, memory_order_release);
            break;
        }
        atomic_store_explicit(&g_tail, t + size, memory_order_release);
        if (r->kind != R_ASYNC) wake_game();
    }
    return NULL;
}

/* ---- the game thread's side ---- */

static unsigned g_n_async, g_n_sync;
static unsigned g_sync_by_id[HLE_ID_MAX];

static uint32_t put_sync(CPU *c, HleId id, HleHandler fn)
{
    Rec *r = ring_take(HDR);
    r->kind = R_SYNC; r->id = (uint32_t)id; r->fn = fn; r->u.c = c; r->seq = ++g_seq;
    ring_put_wake();
    return r->seq;
}

/* the viewport and scissor box the game last set (shadow_state, below) */
static HleId g_id_viewport, g_id_scissor, g_id_popattrib, g_id_getint, g_id_makecurrent;
static int32_t g_vp_shadow[4], g_sc_shadow[4];
static int g_vp_known, g_sc_known;
extern unsigned g_frame;
static void call_sync(CPU *c, HleId id, HleHandler fn)
{
    {   /* LINDBERGH_GLQ_SYNCLOG=<frame>: every call waited for in that frame, with its arguments */
        static long f = -2;
        if (f == -2) { const char *v = getenv("LINDBERGH_GLQ_SYNCLOG"); f = v ? atol(v) : -1; }
        if (f >= 0 && (long)g_frame == f)
            fprintf(stderr, "[glq] sync %s(%#x, %#x, %#x, %#x, %#x, %#x) array buffer %u, element buffer %u\n", hle_name(id),
                    A32(0), A32(1), A32(2), A32(3), A32(4), A32(5), g_array_buf, g_element_buf);
    }
    {   /* LINDBERGH_GLQ_GETS=1: each state read the game waits for, by what it asks (once) */
        static int on = -1; static uint32_t seen[64]; static int nseen;
        if (on < 0) { const char *v = getenv("LINDBERGH_GLQ_GETS"); on = v && *v == '1'; }
        const char *n = on ? hle_name(id) : NULL;
        if (n && g_frame > 1500 && !strncmp(n, "glGet", 5) && nseen < 64) {
            uint32_t key = (uint32_t)id << 20 ^ A32(0);
            int i = 0;
            while (i < nseen && seen[i] != key) i++;
            if (i == nseen) { seen[nseen++] = key; fprintf(stderr, "[glq] frame %u: %s(%#x) waits\n", g_frame, n, A32(0)); }
        }
    }
    uint32_t s = put_sync(c, id, fn);
    GAME_WAIT((int32_t)(atomic_load_explicit(&g_done_seq, memory_order_acquire) - s) >= 0);
    g_n_sync++; g_sync_by_id[id]++;
    if (id && id == g_id_getint && A32(1)) {      /* the answer, kept for the next read */
        uint32_t pname = A32(0), out = A32(1);
        int32_t *v = pname == 0x0BA2 ? g_vp_shadow : pname == 0x0C10 ? g_sc_shadow : NULL;
        if (v) { for (int i = 0; i < 4; i++) v[i] = (int32_t)rd32(out + 4u * (uint32_t)i); if (pname == 0x0BA2) g_vp_known = 1; else g_sc_known = 1; }
    }
}

/* The viewport and scissor box, as the game last set them: its glGetIntegerv
 * of either is answered here, without waiting for the worker (8 times a frame
 * in The House of the Dead 4). glPopAttrib may put back older ones: then the
 * next read waits, and its answer is kept. */
static void shadow_ids(void)
{
    for (unsigned i = 0; i < HLE_COUNT; i++) {
        const char *n = hle_name((HleId)i);
        if (!strcmp(n, "glViewport")) g_id_viewport = (HleId)i;
        else if (!strcmp(n, "glScissor")) g_id_scissor = (HleId)i;
        else if (!strcmp(n, "glPopAttrib")) g_id_popattrib = (HleId)i;
        else if (!strcmp(n, "glGetIntegerv")) g_id_getint = (HleId)i;
        else if (!strcmp(n, "glXMakeCurrent")) g_id_makecurrent = (HleId)i;
    }
}
/* 1: the call was a read answered here */
static int shadow_state(CPU *c, HleId id)
{
    static int off = -1;        /* LINDBERGH_GLQ_NOSHADOW=1: every read waits, as before */
    if (off < 0) { const char *v = getenv("LINDBERGH_GLQ_NOSHADOW"); off = v && *v == '1'; }
    if (!id || off) return 0;
    if (id == g_id_viewport) { for (int i = 0; i < 4; i++) g_vp_shadow[i] = (int32_t)A32(i); g_vp_known = 1; return 0; }
    if (id == g_id_scissor)  { for (int i = 0; i < 4; i++) g_sc_shadow[i] = (int32_t)A32(i); g_sc_known = 1; return 0; }
    if (id == g_id_popattrib) { g_vp_known = g_sc_known = 0; return 0; }
    if (id == g_id_getint) {
        uint32_t pname = A32(0), out = A32(1);
        const int32_t *v = pname == 0x0BA2 && g_vp_known ? g_vp_shadow : pname == 0x0C10 && g_sc_known ? g_sc_shadow : NULL;
        if (!v || !out) return 0;
        for (int i = 0; i < 4; i++) wr32(out + 4u * (uint32_t)i, (uint32_t)v[i]);
        return 1;
    }
    return 0;
}

/* ---- client arrays copied at a lock ----
 * A batch of vertices in the game's own memory, locked (glLockArraysEXT) and
 * drawn: its locked range of every array in use, and its index lists, go to
 * the worker as copies instead of making the game wait (twice a frame in The
 * House of the Dead 4: an offscreen pass of 423 vertices). The copies live in
 * three buffers used a frame each - the game is never more than a frame
 * ahead. LINDBERGH_GLQ_NOCOPY=1: such a batch waits, as before. */
#define ARENA_SIZE (2u << 20)
static uint8_t *g_arena[3];
static uint32_t g_arena_used, g_arena_frame = ~0u;
static int copy_on(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("LINDBERGH_GLQ_NOCOPY"); on = !(v && *v == '1'); }
    return on;
}
static uint8_t *arena_take(uint32_t n)
{
    if (g_arena_frame != g_frame) { g_arena_frame = g_frame; g_arena_used = 0; }
    uint8_t **a = &g_arena[g_frame % 3];
    if (!*a) *a = guest_alloc(ARENA_SIZE, 64);
    n = (n + 15u) & ~15u;
    if (!*a || g_arena_used + n > ARENA_SIZE) return NULL;
    uint8_t *p = *a + g_arena_used;
    g_arena_used += n;
    return p;
}
static uint32_t type_size(uint32_t t)
{
    switch (t) {
    case 0x1400: case 0x1401: return 1;
    case 0x1402: case 0x1403: case 0x140B: return 2;
    case 0x1404: case 0x1405: case 0x1406: return 4;
    case 0x140A: return 8;
    }
    return 0;
}
/* bytes of one element of slot s's array, and its stride */
static int ptr_layout(int s, uint32_t *elem, uint32_t *stride)
{
    const uint32_t *a = g_ptr[s].args;
    uint32_t size, type, st;
    if (g_ptr[s].a == 2) { size = s == S_NORMAL ? 3 : 1; type = a[0]; st = a[1]; }
    else if (g_ptr[s].a == 3) { size = a[0] == 0x80E1 ? 4 : a[0]; type = a[1]; st = a[2]; }
    else if (g_ptr[s].a == 5) { size = a[1] == 0x80E1 ? 4 : a[1]; type = a[2]; st = a[4]; }
    else return 0;
    uint32_t ts = type_size(type);
    if (!ts || size < 1 || size > 4) return 0;
    *elem = size * ts; *stride = st ? st : size * ts;
    return 1;
}
static void put_call(HleId id, HleHandler fn, const uint32_t *args)
{
    Rec *r = ring_take(HDR + ARGS * 4);
    r->kind = R_ASYNC; r->id = (uint32_t)id; r->fn = fn;
    memcpy((uint8_t *)r + HDR, args, ARGS * 4);
    ring_put();
    g_n_async++;
}
/* slot s's pointer call again, its array at va (texture coordinates: for their unit) */
static void put_pointer(int s, uint32_t va)
{
    uint32_t args[ARGS];
    memcpy(args, g_ptr[s].args, sizeof args);
    args[g_ptr[s].a] = va;
    int tex = s >= S_TEX0 && s < S_ATTR0;
    uint32_t unit[ARGS] = { 0 };
    if (tex && g_cat_fn) { unit[0] = 0x84C0 + g_ptr[s].unit; put_call(g_cat_id, g_cat_fn, unit); }
    put_call(g_ptr[s].id, g_ptr[s].fn, args);
    if (tex && g_cat_fn) { unit[0] = 0x84C0 + g_client_unit; put_call(g_cat_id, g_cat_fn, unit); }
}
/* the locked range [first, first+count) of every client array in use, copied
 * and pointed at; 0: not possible (nothing changed) */
static int lock_copy(uint32_t first, uint32_t count)
{
    if (!count || count > (1u << 20)) return 0;
    uint8_t *copy[S_N] = { 0 };
    uint32_t stride[S_N] = { 0 };
    for (int s = 0; s < S_N; s++) {
        if (!(g_slot_on[s] && g_slot_client[s])) continue;
        uint32_t elem;
        if (!g_ptr[s].set || !ptr_layout(s, &elem, &stride[s])) return 0;
        uint32_t src = g_ptr[s].args[g_ptr[s].a] + first * stride[s];
        uint32_t n = (count - 1) * stride[s] + elem;
        if (!(copy[s] = arena_take(n))) return 0;
        memcpy(copy[s], GP(src), n);
    }
    for (int s = 0; s < S_N; s++) {
        if (!copy[s]) continue;
        put_pointer(s, H2G(copy[s]) - first * stride[s]);   /* element i at copy + (i - first) * stride */
        g_slot_client[s] = 0; g_ptr[s].copied = 1;
    }
    return 1;
}
/* at unlock: the copied arrays back on the game's memory (an unlocked draw then waits) */
static void unlock_restore(void)
{
    for (int s = 0; s < S_N; s++)
        if (g_ptr[s].copied) { g_ptr[s].copied = 0; g_slot_client[s] = 1; put_pointer(s, g_ptr[s].args[g_ptr[s].a]); }
}

static void glq_hook(CPU *c, HleId id, HleHandler fn)
{
    const Cls *k = cls(id);
    uint32_t a0 = A32(0);
    if (shadow_state(c, id)) return;
    int async = 1;
    uint32_t extra = 0;                /* bytes of arrays after the arguments */
    switch (k->kind) {
    case C_SYNC: async = 0; break;
    case C_RAW:
        /* glXMakeCurrent of a context: on the worker it picks the drawable's
         * framebuffer and succeeds (the window is up by then) - queued, and
         * answered here. Twice a frame in The House of the Dead 4, around a
         * pass into a pbuffer; each made the game wait for the whole queue. */
        if (id == g_id_makecurrent && g_id_makecurrent && A32(2) && copy_on()) { c->eax = 1; break; }
        async = 0; break;
    case C_DIRECT: fn(c); return;
    case C_VAL: break;
    case C_BLOB: extra = k->b; if (!A32(k->a)) async = 0; break;
    case C_VEC: {
        int32_t n = (int32_t)A32(k->b);
        if (n < 0 || n > 4096 || !A32(k->a)) async = 0; else extra = (uint32_t)n * k->c;
        break; }
    case C_BIND:
        if (a0 == 0x8892) g_array_buf = A32(1);
        else if (a0 == 0x8893) g_element_buf = A32(1);
        break;
    case C_ENABLE_CS: case C_DISABLE_CS: {
        int s = slot_of(a0); if (s >= 0) g_slot_on[s] = k->kind == C_ENABLE_CS;
        break; }
    case C_CLIENT_TEX: g_client_unit = a0 - 0x84C0; g_cat_id = id; g_cat_fn = fn; break;
    case C_ENABLE_VA: case C_DISABLE_VA:
        if (a0 < 16) g_slot_on[S_ATTR0 + a0] = k->kind == C_ENABLE_VA;
        break;
    case C_PTR: {
        int s = k->b == S_TEX0 ? S_TEX0 + (int)(g_client_unit & 7) : k->b == S_ATTR0 ? (a0 < 16 ? S_ATTR0 + (int)a0 : -1) : k->b;
        uint32_t ptr = A32(k->a);
        if (s >= 0) {
            g_slot_client[s] = g_array_buf == 0 && ptr != 0;
            g_ptr[s].set = g_array_buf == 0; g_ptr[s].copied = 0; g_ptr[s].unit = (uint8_t)(g_client_unit & 7);
            g_ptr[s].id = id; g_ptr[s].fn = fn; g_ptr[s].a = k->a;
            for (int i = 0; i < 16; i++) g_ptr[s].args[i] = A32(i);
        }
        /* the pointer is the game's memory: keep in step - unless the copies
         * at a lock take care of it (a draw from it otherwise still waits),
         * or it points at nothing */
        if (g_array_buf == 0 && ptr != 0 && !(copy_on() && s >= 0)) async = 0;
        break; }
    case C_DRAW: if (client_arrays_live()) async = 0; break;
    case C_LOCK:
        if (client_arrays_live() && !(copy_on() && lock_copy(A32(0), A32(1)))) async = 0;
        break;
    case C_UNLOCK: unlock_restore(); break;
    case C_DRAW_EL: if (client_arrays_live() || !g_element_buf) async = 0; break;
    case C_MULTI_EL: {
        int32_t n = (int32_t)A32(4);
        if (client_arrays_live() || n <= 0 || n > 4096 || !A32(1) || !A32(3)) async = 0;
        else if (g_element_buf) extra = 8u * (uint32_t)n;
        else if (!copy_on() || !type_size(A32(2))) async = 0;
        else {      /* index lists in the game's memory: they go along too */
            extra = 8u * (uint32_t)n;
            for (int32_t i = 0; i < n; i++) extra += ((uint32_t)rd32(A32(1) + 4u * (uint32_t)i) * type_size(A32(2)) + 15u) & ~15u;
            if (extra > (4u << 20)) async = 0;
        }
        break; }
    }
    if (!async) { call_sync(c, id, fn); return; }

    Rec *r = ring_take(HDR + ARGS * 4 + ((extra + 15u) & ~15u));
    r->kind = R_ASYNC; r->id = (uint32_t)id; r->fn = fn;
    uint32_t *args = (uint32_t *)((uint8_t *)r + HDR);
    memcpy(args, GP(c->esp), ARGS * 4);
    uint8_t *blob = (uint8_t *)args + ARGS * 4;
    if (k->kind == C_BLOB) {
        memcpy(blob, GP(A32(k->a)), extra);
        args[k->a] = va_of(blob);
    } else if (k->kind == C_VEC && extra) {
        memcpy(blob, GP(A32(k->a)), extra);
        args[k->a] = va_of(blob);
    } else if (k->kind == C_MULTI_EL) {
        uint32_t n = A32(4);
        memcpy(blob, GP(A32(1)), 4u * n);           /* counts */
        memcpy(blob + 4u * n, GP(A32(3)), 4u * n);  /* offsets into the element buffer */
        args[1] = va_of(blob); args[3] = va_of(blob + 4u * n);
        if (!g_element_buf) {                       /* or the lists themselves, copied after */
            uint32_t *ptrs = (uint32_t *)(blob + 4u * n), *cnt = (uint32_t *)blob, ts = type_size(A32(2));
            uint8_t *at = blob + 8u * n;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t bytes = cnt[i] * ts;
                memcpy(at, GP(ptrs[i]), bytes);
                ptrs[i] = va_of(at);
                at += (bytes + 15u) & ~15u;
            }
        }
    }
    ring_put();
    g_n_async++;
}

/* A job for the worker; host_swap's end of frame. Returns its number, for
 * glq_wait. */
uint32_t glq_job(void (*job)(void *), void *arg)
{
    Rec *r = ring_take(HDR);
    r->kind = R_JOB; r->u.job = job; r->arg = arg; r->seq = ++g_seq;
    ring_put_wake();
    return r->seq;
}

void glq_wait(uint32_t seq)
{
    if (!g_on || !seq) return;
    GAME_WAIT((int32_t)(atomic_load_explicit(&g_done_seq, memory_order_acquire) - seq) >= 0);
}

static void nothing(void *a) { (void)a; }
void glq_drain(void) { if (g_on) glq_wait(glq_job(nothing, NULL)); }

void glq_report(unsigned frames)
{
    if (!g_on || !frames) return;
    /* which thread holds the frame back: each one's CPU time, and the game
     * thread's time waiting for the worker */
    static double game0, work0;
    double game = now_ms(CLOCK_THREAD_CPUTIME_ID), work = atomic_load(&g_work_ms);
    fprintf(stderr, "[glq] ms per frame: game thread %.1f (waiting %.1f), GL thread %.1f; ",
            game0 ? (game - game0) / frames : 0, g_wait_ms / frames, work0 ? (work - work0) / frames : 0);
    game0 = game; work0 = work; g_wait_ms = 0;
    fprintf(stderr, "per frame: %u queued, %u waited for", g_n_async / frames, g_n_sync / frames);
    for (int k = 0; k < 10; k++) {
        unsigned best = 0;
        for (unsigned i = 1; i < HLE_ID_MAX; i++) if (g_sync_by_id[i] > g_sync_by_id[best]) best = i;
        if (!g_sync_by_id[best]) break;
        fprintf(stderr, "%s %s %.1f", k ? "," : ":", hle_name((HleId)best), (double)g_sync_by_id[best] / frames);
        g_sync_by_id[best] = 0;
    }
    fputc('\n', stderr);
    memset(g_sync_by_id, 0, sizeof g_sync_by_id);
    g_n_async = g_n_sync = 0;
}

/* Start, from the game thread at its first swap. release() lets go of the GL
 * context on this thread; take() makes it current on the worker. */
int glq_start(void (*release)(void), void (*take)(void))
{
    if (g_on) return 1;
    void *p = guest_alloc(RING_SIZE, 64);
    if (!p) { fprintf(stderr, "[glq] no room for the queue\n"); return 0; }
    g_ring = p; g_ring_va = H2G(p);
    /* every GL and GLX import through the hook (stereo.c routes the GL) */
    for (unsigned i = 0; i < HLE_COUNT; i++) {
        const char *n = hle_name((HleId)i);
        if (n[0] == 'g' && n[1] == 'l' && strncmp(n, "glut", 4)) g_gl_ids[i] = 1;
    }
    g_prev = g_gl_hook;
    shadow_ids();
    g_take_context = take;
    if (release) release();
    if (pthread_create(&g_thread, NULL, worker, NULL)) {
        fprintf(stderr, "[glq] no worker thread\n");
        if (take) take();
        return 0;
    }
    g_on = 1;
    g_gl_hook = glq_hook;
    fprintf(stderr, "[glq] GL on its own thread\n");
    return 1;
}
