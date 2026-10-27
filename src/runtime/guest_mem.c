/*
 * guest_mem.c - the guest's 4 GB address space on a 64-bit host.
 *
 * A Lindbergh game is a 32-bit process: every pointer it holds, stores or
 * compares is 32 bits wide. On a 32-bit host that is simply a host pointer. On
 * a 64-bit host (macOS, the Quest) there is no address 0x08048000 to map the
 * image at, so instead one 4 GB window is reserved and a guest address is an
 * offset into it - cpu.h's G2H() adds the base on every access.
 *
 * Everything the game can hold a pointer to has to live inside that window:
 * its image, its stacks, its heap, and any host object a library call hands
 * back to it. guest_alloc() is the heap for all of those, and guest_addr_of()
 * is the check at the boundary - a host pointer that is NOT in the window has
 * no 32-bit name, and truncating it silently is exactly the kind of fault that
 * shows up as a crash somewhere unrelated much later.
 *
 * Layout, chosen to match where Linux/i386 puts things:
 *   0x00000000..0x0000FFFF   never mapped: a null dereference faults
 *   0x08048000..             the ELF image, then its brk heap
 *   0x20000000..0xB0000000   guest_alloc() heap (dlmalloc mspace, 2.25 GB)
 *   0xBF7F0000..0xBFFF0000   the main thread's stack (guest.c)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>

#include "lindbergh_rt.h"

#define ONLY_MSPACES 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_LOCKS 1
#define USE_DL_PREFIX 1
#define MALLOC_FAILURE_ACTION
#define FOOTERS 0
#include "../../third_party/dlmalloc/malloc.c"

#define WINDOW      (1ull << 32)
#define GUARD       0x10000u          /* overrun past 0xFFFFFFFF lands here */
#define HEAP_LO     0x20000000u
#define HEAP_HI     0xB0000000u

uint8_t *g_guest_base;
static mspace g_heap;

int guest_mem_init(void)
{
    if (g_guest_base) return 0;
    /* One reservation for the whole window plus a guard, so a 4-byte access at
     * 0xFFFFFFFE stays inside memory we own. MAP_NORESERVE: pages cost nothing
     * until the guest touches them. */
    void *p = mmap(NULL, WINDOW + GUARD, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("[mem] reserving the 4 GB guest window");
        return -1;
    }
    g_guest_base = (uint8_t *)p;
    mprotect(p, GUARD, PROT_NONE);    /* guest null page */

    g_heap = create_mspace_with_base(g_guest_base + HEAP_LO, HEAP_HI - HEAP_LO, 1);
    if (!g_heap) {
        fprintf(stderr, "[mem] cannot create the guest heap\n");
        return -1;
    }
    fprintf(stderr, "[mem] guest window at %p, heap %#x..%#x\n",
            p, HEAP_LO, HEAP_HI);
    return 0;
}

void *guest_alloc(size_t n, size_t align)
{
    void *p = align > 16 ? mspace_memalign(g_heap, align, n ? n : 1)
                         : mspace_malloc(g_heap, n ? n : 1);
    if (!p) {
        fprintf(stderr, "[mem] guest heap exhausted: %zu bytes (align %zu)\n", n, align);
        fflush(stderr);
    }
    return p;
}

void guest_free(void *p)
{
    if (p) mspace_free(g_heap, p);
}

size_t guest_alloc_size(void *p)
{
    return p ? mspace_usable_size(p) : 0;
}

uint32_t guest_addr_of(const void *p, const char *file, int line)
{
    if (!p) return 0;
    uintptr_t off = (uintptr_t)((const uint8_t *)p - g_guest_base);
    if ((const uint8_t *)p < g_guest_base || off >= WINDOW) {
        fprintf(stderr,
                "[mem] %s:%d handed the guest a host pointer %p outside its 4 GB "
                "window (base %p). It has no 32-bit address; this is a port bug.\n",
                file, line, p, (void *)g_guest_base);
        fflush(stderr);
        guest_report_state("host pointer escaped into the guest");
        abort();
    }
    return (uint32_t)off;
}

/* Interned copies of host strings, for calls whose result the caller treats as
 * static storage. A small linear table: these are a few dozen distinct
 * strings (error texts, GL_VENDOR, locale names), not a hot path. */
static pthread_mutex_t g_str_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char *host; uint32_t guest; } g_strs[512];
static int g_nstrs;

uint32_t guest_strdup_static(const char *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&g_str_lock);
    for (int i = 0; i < g_nstrs; i++)
        if (strcmp(g_strs[i].host, s) == 0) {
            uint32_t g = g_strs[i].guest;
            pthread_mutex_unlock(&g_str_lock);
            return g;
        }
    size_t n = strlen(s) + 1;
    char *copy = (char *)guest_alloc(n, 16);
    memcpy(copy, s, n);
    uint32_t g = H2G(copy);
    if (g_nstrs < (int)(sizeof g_strs / sizeof g_strs[0])) {
        g_strs[g_nstrs].host = copy;
        g_strs[g_nstrs].guest = g;
        g_nstrs++;
    }
    pthread_mutex_unlock(&g_str_lock);
    return g;
}

/* ---- FILE handles ----
 * A guest FILE* is a 16-byte guest block holding the host FILE*. Freed on
 * fclose; a handle that does not carry the magic is refused, not followed. */
#define FILE_MAGIC 0x46494C45u        /* "FILE" */

typedef struct { uint32_t magic, pad; FILE *f; } GuestFile;

uint32_t guest_file_wrap(FILE *f)
{
    if (!f) return 0;
    GuestFile *g = (GuestFile *)guest_alloc(sizeof *g, 16);
    g->magic = FILE_MAGIC;
    g->f = f;
    return H2G(g);
}

FILE *guest_file(uint32_t handle)
{
    if (!handle) return NULL;
    GuestFile *g = (GuestFile *)G2H(handle);
    if (g->magic != FILE_MAGIC) {
        fprintf(stderr, "[mem] %#010x is not a FILE handle\n", handle);
        return NULL;
    }
    return g->f;
}

void guest_file_release(uint32_t handle)
{
    if (!handle) return;
    GuestFile *g = (GuestFile *)G2H(handle);
    if (g->magic == FILE_MAGIC) { g->magic = 0; guest_free(g); }
}
