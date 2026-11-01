/*
 * Runtime self-check. Stands in for the generated parts of a game so the
 * runtime can be built and run on its own.
 *
 * Covers the two pieces with logic in them: the dispatch table's search, and
 * the syscall layer's argument marshalling. guest_load() is not covered here -
 * it needs a real ELF, and the first title exercises it.
 */

#include "display_cadence.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lindbergh_rt.h"

static int g_hit;

void L_08048100(CPU *c) { (void)c; g_hit = 1; }
void L_08048200(CPU *c) { (void)c; g_hit = 2; }
void L_0804FF00(CPU *c) { (void)c; g_hit = 3; }

static int fails;

#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", \
                                __FILE__, __LINE__, #cond); fails++; } } while (0)

static void test_dispatch(void)
{
    CPU c;
    memset(&c, 0, sizeof c);
    /* Every entry reachable, including the first and last - an off-by-one in
     * the binary search hides at exactly those two. */
    g_hit = 0; dispatch(&c, 0x08048100u); CHECK(g_hit == 1);
    g_hit = 0; dispatch(&c, 0x08048200u); CHECK(g_hit == 2);
    g_hit = 0; dispatch(&c, 0x0804FF00u); CHECK(g_hit == 3);
}

static void test_syscall_write(void)
{
    /* write(1, buf, n) - number in eax, args in ebx/ecx/edx. */
    static const char msg[] = "";
    CPU c;
    memset(&c, 0, sizeof c);
    c.eax = 4;
    c.ebx = 1;
    c.ecx = (uint32_t)(uintptr_t)msg;
    c.edx = 0;
    linux_syscall(&c);
    CHECK((int32_t)c.eax == 0);
}

static void test_syscall_unknown(void)
{
    CPU c;
    memset(&c, 0, sizeof c);
    c.eax = 400;                       /* nothing implements this */
    linux_syscall(&c);
    CHECK((int32_t)c.eax == -38);      /* -ENOSYS, not 0 and not a crash */
}

static void test_hle_names(void)
{
    CHECK(strcmp(hle_name(HLE_write), "write") == 0);
    CHECK(strcmp(hle_name(HLE_glClear), "glClear") == 0);
    CHECK(HLE_COUNT == HLE_test_solib_function + 1);
}

static void test_display_cadence(void)
{
    const int64_t p120 = 8333333, p60 = 16666667;
    int64_t t = 1000000000;
    host_display_timing_reset();
    CHECK(host_frame_period_ms() == 0);
    host_display_timing(t, p120);
    CHECK(fabsf(host_frame_period_ms() - 16.666666f) < 0.001f);
    for (int i = 0; i < 8; ++i) {
        t += 2 * p120;
        host_display_timing(t, p120);
        CHECK(fabsf(host_frame_period_ms() - 16.666666f) < 0.001f);
    }
    /* Actual 1/3 delivery phase: one correction, not a permanently slower game. */
    host_display_timing(t += p120, p120);
    host_display_timing(t += 3 * p120, p120);
    CHECK(fabsf(host_frame_period_ms() - 20.833333f) < 0.001f);
    CHECK(fabsf(host_frame_period_ms() - 16.666666f) < 0.001f);
    /* Tiny period noise preserves history; a real refresh change resets it. */
    host_display_timing(t += p120, p120);
    host_display_timing(t += 3 * p120, p120 + 1);
    CHECK(host_frame_period_ms() > 20);
    host_display_timing(t += p120, p120);
    host_display_timing(t += 3 * p120, p120);
    host_display_timing(t += p60, p60);
    CHECK(fabsf(host_frame_period_ms() - 16.666667f) < 0.001f);
    host_display_timing(t += 13888889, 13888889); /* 72 Hz cannot divide 60 evenly */
    CHECK(host_frame_period_ms() == 0);
    host_display_timing(t += 11111111, 11111111); /* neither can 90 Hz */
    CHECK(host_frame_period_ms() == 0);
    host_display_timing(1000000000, p120); /* new session clock/phase */
    CHECK(fabsf(host_frame_period_ms() - 16.666666f) < 0.001f);
    host_display_timing_reset();
    CHECK(host_frame_period_ms() == 0);
}

int main(void)
{
    test_display_cadence();
    test_dispatch();
    test_syscall_write();
    test_syscall_unknown();
    test_hle_names();
    int test_runtime_abis(void);
    fails += test_runtime_abis();
    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    printf("ok: dispatch table, syscall layer, import names\n");
    return 0;
}
