/* Contract tests using guest pointers, the dispatch ABI and SDL's dummy
 * audio device. No game files or hardware are required. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lindbergh_rt.h"
#include "solib.h"

static int initialized;
static const unsigned char image[32] = { 0x78, 0x56, 0x34, 0x12 };
const SolibImage g_solib_images[] = {
    { "test.so", 0xB4000000u, sizeof image, image, 0xB4000010u, 0, 0 }
};
const int g_solib_count = 1;

void L_B4000010(CPU *c) { initialized++; c->eax = 99; c->ecx = 99; c->esp += 4; }
void L_B4000000(CPU *c) { c->eax = rd32(c->esp + 4) + c->ecx; c->esp += 4; }

#ifndef _WIN32
#include <SDL.h>
#include "host_settings.h"
#include "glsl_compat.h"
static int fails;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL ABI:%d %s\n", __LINE__, #x); fails++; } } while (0)
static uint32_t call(CPU *c, HleId id, const uint32_t *args, int count)
{
    for (int i = 0; i < count; i++) wr32(c->esp + 4u * i, args[i]);
    hle_call(c, id); return c->eax;
}
#define CALL(id, ...) call(&c, id, (uint32_t[]){ __VA_ARGS__ }, sizeof((uint32_t[]){ __VA_ARGS__ }) / sizeof(uint32_t))

static uint32_t constructed[8], destroyed[8], allocation, freed;
static int nc, nd;
static void alloc_array(CPU *c) { allocation = H2G(guest_alloc(A32(0), 16)); c->eax = allocation; }
static void free_array(CPU *c) { freed = A32(0); guest_free(APTR(0)); }
static void construct(CPU *c) { constructed[nc++] = A32(0); wr32(A32(0), 1234); }
static void destroy(CPU *c) { destroyed[nd++] = A32(0); }
static void shader(const char *input, const char *expected)
{
    char *s = glsl_compat(input);
    CHECK(s && !strcmp(s, expected));
    if (s && strcmp(s, expected)) fprintf(stderr, "shader result: %s\n", s);
    free(s);
}

int test_runtime_abis(void)
{
    CHECK(guest_mem_init() == 0);
    CPU c = {0};
    c.esp = H2G(guest_alloc(4096, 16)) + 2048;
    uint32_t out = H2G(guest_alloc(16, 16));
    hle_register_libc(); hle_register_cxx(); hle_register_alsa();

    CHECK(jvs_switch_byte(CAB_P1_ACTION, 0, 1) == 0x80);
    CHECK(jvs_switch_byte(CAB_P2_ACTION, 1, 1) == 0x80);
    jvs_action_switch(0, 0, 0, 8); jvs_action_switch(1, 0, 0, 4);
    CHECK(jvs_switch_byte(CAB_P1_ACTION | CAB_P1_TRIGGER, 0, 0) == 10);
    CHECK(jvs_switch_byte(CAB_P2_ACTION, 0, 0) == 4);
    CHECK(jvs_switch_byte(CAB_P2_ACTION, 1, 0) == 0);
    CHECK(jvs_switch_byte(CAB_P1_ACTION | CAB_P2_ACTION, 0, 0) == 12);
    CHECK(jvs_switch_byte(0, 0, 0) == 0);
    CHECK(jvs_switch_byte(CAB_P1_RELOAD, 0, 0) == 1);
    CHECK(jvs_switch_byte(CAB_P2_TRIGGER, 1, 0) == 2);
    jvs_action_switch(0, 0, 1, 0x80); jvs_action_switch(1, 1, 1, 0x80);

    shader("half4 c=0;", "vec4 c=vec4(0);");
    shader("vec3 c=-1.0;", "vec3 c=vec3(-1.0);");
    shader("float d=shadow2D(s,vec3(uv,0));", "float d=shadow2D(s,vec3(uv,0)).r;");
    shader("const mat2 m={{1,2},{3,4}};", "const mat2 m=mat2( 1,2 , 3,4 );");
    shader("c.rgb=dot(v,vec3(1));", "c.rgb=vec3(dot(v,vec3(1)));");
    shader("dot(v,(0.2,0.3,0.5))", "dot(vec3(v),vec3(0.2,0.3,0.5))");
    shader("sampler2D s; uniform sampler2D t; void f(sampler2D a){}",
           "uniform sampler2D s; uniform sampler2D t; void f(sampler2D a){}");
    shader("// half\n#define half float\nmyhalf half_var;\nvec4 c=texture2D(s,uv);",
           "// half\n#define half float\nmyhalf half_var;\nvec4 c=texture2D(s,uv);");

    wr32(out, 0x12345678);
    CHECK(CALL(HLE_posix_memalign, out, 3, 32) == 22);
    CHECK(rd32(out) == 0x12345678);
    CHECK(CALL(HLE_posix_memalign, out, 4, 32) == 0); /* guest pointer width */
    uint32_t ptr = rd32(out);
    CHECK(ptr && !(ptr & 3));
    wr32(ptr, 0x12345678); CHECK(rd32(ptr) == 0x12345678);
    CALL(HLE_free, ptr);
    CHECK(CALL(HLE_posix_memalign, out, 4096, 32) == 0);
    CHECK(!(rd32(out) & 4095)); CALL(HLE_free, rd32(out));

    guest_override_va("array allocator", 0x08049000, alloc_array);
    guest_override_va("array deallocator", 0x08049010, free_array);
    guest_override_va("array constructor", 0x08049020, construct);
    guest_override_va("array destructor", 0x08049030, destroy);
    uint32_t array = CALL(HLE___cxa_vec_new2, 3, 8, 4, 0x08049020, 0x08049030, 0x08049000, 0x08049010);
    CHECK(array == allocation + 4 && rd32(array - 4) == 3 && nc == 3);
    CHECK(constructed[0] == array && constructed[2] == array + 16);
    CALL(HLE___cxa_vec_delete2, array, 8, 4, 0x08049030, 0x08049010);
    CHECK(nd == 3 && destroyed[0] == array + 16 && destroyed[2] == array && freed == allocation);
    CALL(HLE___cxa_vec_delete2, 0, 8, 4, 0x08049030, 0x08049010);
    CHECK(nd == 3);

    uint32_t list = H2G(guest_alloc(24, 16)), a = list + 8, b = list + 16;
    wr32(list, list); wr32(list + 4, list);
    CALL(HLE_list_hook, a, list); CALL(HLE_list_hook, b, list);
    CHECK(rd32(list) == a && rd32(a) == b && rd32(b) == list);
    CHECK(rd32(list + 4) == b && rd32(b + 4) == a && rd32(a + 4) == list);
    CALL(HLE_list_unhook, a);
    CHECK(rd32(list) == b && rd32(b + 4) == list);
    CALL(HLE_list_unhook, b);
    CHECK(rd32(list) == list && rd32(list + 4) == list);
    guest_free(GP(list));

    uint32_t sp = c.esp;
    c.ecx = 17;
    CHECK(CALL(HLE_test_solib_function, 25) == 42);
    CHECK(c.esp == sp && c.ecx == 17 && initialized == 1);
    CHECK(rd32(0xB4000000) == 0x12345678);
    CHECK(CALL(HLE_test_solib_function, 1) == 18 && initialized == 1);
    CHECK(!solib_export("does_not_exist"));

    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    settings()->muted = 1;
    CHECK(CALL(HLE_snd_pcm_open, out, 0, 0, 0) == 0);
    uint32_t pcm = rd32(out);
    CHECK(CALL(HLE_snd_pcm_hw_params_malloc, out) == 0);
    uint32_t params = rd32(out);
    CHECK(CALL(HLE_snd_pcm_hw_params_any, pcm, params) == 0);
    CHECK(CALL(HLE_snd_pcm_hw_params_set_channels, pcm, params, 6) == 0);
    CHECK(CALL(HLE_snd_pcm_hw_params, pcm, params) == 0);
    CHECK(CALL(HLE_snd_pcm_avail_update, pcm) == 4096);
    uint32_t samples = H2G(guest_alloc(4096 * 12, 16));
    memset(GP(samples), 0x3f, 4096 * 12);
    CHECK(CALL(HLE_snd_pcm_writei, pcm, 0, 0) == 0);
    CHECK(CALL(HLE_snd_pcm_writei, pcm, samples, 5000) == 4096); /* short write */
    CHECK(CALL(HLE_snd_pcm_avail_update, pcm) == 0);
    CHECK((int32_t)CALL(HLE_snd_pcm_writei, pcm, samples, 1) == -11);
    CHECK(CALL(HLE_snd_pcm_start, pcm) == 0);
    SDL_Delay(60);
    CHECK(CALL(HLE_snd_pcm_avail_update, pcm) > 0); /* mute still consumes PCM */
    CHECK(CALL(HLE_snd_pcm_drop, pcm) == 0);
    CHECK(CALL(HLE_snd_pcm_avail_update, pcm) == 4096);
    CHECK(CALL(HLE_snd_pcm_close, pcm) == 0);
    CHECK((int32_t)CALL(HLE_snd_pcm_avail_update, pcm) == -77);
    CALL(HLE_snd_pcm_hw_params_free, params);
    guest_free(GP(samples)); guest_free(GP(out)); guest_free(GP(sp - 2048));
    if (!fails) puts("ok: guest allocation, C++ containers, linked libraries, JVS routing, GLSL, muted PCM timing");
    return fails;
}
#else
int test_runtime_abis(void) { return 0; }
#endif
