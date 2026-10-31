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

#ifdef RECOMP_GUEST_BASE
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
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

static int tree_height(uint32_t node, uint32_t parent, int lo, int hi, int *count, unsigned depth)
{
    if (!node) return 1;
    CHECK(depth < 65);
    if (depth >= 65) return 0;
    int key = (int)rd32(node + 16);
    uint32_t l = rd32(node + 8), r = rd32(node + 12);
    CHECK(key > lo && key < hi && rd32(node + 4) == parent);
    CHECK(rd32(node) <= 1);
    if (!rd32(node)) { CHECK(!l || rd32(l) == 1); CHECK(!r || rd32(r) == 1); }
    (*count)++;
    int lh = tree_height(l, node, lo, key, count, depth + 1);
    int rh = tree_height(r, node, key, hi, count, depth + 1);
    CHECK(lh == rh);
    return lh + (int)rd32(node);
}
static void check_tree(uint32_t header, const unsigned char *alive, int expected)
{
    uint32_t root = rd32(header + 4);
    int count = 0;
    CHECK(!root || rd32(root) == 1);
    tree_height(root, header, -1, 64, &count, 0);
    CHECK(count == expected);
    int first = 0, last = 63;
    while (first < 64 && !alive[first]) first++;
    while (last >= 0 && !alive[last]) last--;
    if (expected) {
        CHECK((int)rd32(rd32(header + 8) + 16) == first);
        CHECK((int)rd32(rd32(header + 12) + 16) == last);
    } else CHECK(!root && rd32(header + 8) == header && rd32(header + 12) == header);
}
static void test_tree(CPU cpu)
{
    CPU c = cpu;
    uint32_t memory = H2G(guest_alloc(16 + 64 * 20, 16));
    uint32_t random = 713;
    for (int round = 0; round < 32; round++) {
        memset(GP(memory), 0, 16 + 64 * 20);
        wr32(memory + 8, memory); wr32(memory + 12, memory);
        int order[64]; unsigned char alive[64] = {0};
        for (int i = 0; i < 64; i++) order[i] = i;
        for (int i = 63; i > 0; i--) {
            random = random * 1664525u + 1013904223u;
            int j = (int)(random % (unsigned)(i + 1)), t = order[i]; order[i] = order[j]; order[j] = t;
        }
        for (int i = 0; i < 64; i++) {
            int key = order[i], left = 1;
            uint32_t node = memory + 16 + 20u * key, parent = memory, at = rd32(memory + 4);
            wr32(node + 16, (uint32_t)key);
            while (at) { parent = at; left = key < (int)rd32(at + 16); at = rd32(at + (left ? 8 : 12)); }
            CALL(HLE_rb_insert, left, node, parent, memory);
            alive[key] = 1; check_tree(memory, alive, i + 1);
        }
        for (int i = 0; i < 64; i++) {
            int key;
            if (i % 4 == 0) key = (int)rd32(rd32(memory + 4) + 16); /* root / two-child erase */
            else { random = random * 1664525u + 1013904223u; key = (int)(random % 64); while (!alive[key]) key = (key + 1) % 64; }
            uint32_t node = memory + 16 + 20u * key;
            CHECK(CALL(HLE_rb_erase, node, memory) == node);
            alive[key] = 0; check_tree(memory, alive, 63 - i);
        }
    }
    guest_free(GP(memory));
}

int test_runtime_abis(void)
{
    CHECK(guest_mem_init() == 0);
#ifdef _WIN32
    /* The 4 GB reservation must not consume 4 GB of commit. Exercise untouched
     * image/stack pages, including a 64 KB boundary and the top of uint32_t. */
    MEMORY_BASIC_INFORMATION mbi;
    CHECK(VirtualQuery(GP(0), &mbi, sizeof mbi) != 0 && mbi.State == MEM_RESERVE);
    CHECK(VirtualQuery(GP(0x1234fffcu), &mbi, sizeof mbi) != 0 && mbi.State == MEM_RESERVE);
    wr32(0x1234fffcu, 0x12345678); wr32(0x12350000u, 0x87654321);
    wr32(0xfffffffcu, 0xabcdef12);
    CHECK(rd32(0x1234fffcu) == 0x12345678 && rd32(0x12350000u) == 0x87654321);
    CHECK(rd32(0xfffffffcu) == 0xabcdef12);
    CHECK(VirtualQuery(GP(0x12350000u), &mbi, sizeof mbi) != 0 && mbi.State == MEM_COMMIT);
    CHECK(VirtualQuery(GP(0), &mbi, sizeof mbi) != 0 && mbi.State == MEM_RESERVE);
    char *buffer = guest_alloc(512 * 1024, 4096);
    CHECK(buffer != NULL);
    if (buffer) {
        HANDLE rd = NULL, wr = NULL;
        DWORD written = 0, read = 0;
        CHECK(VirtualQuery(buffer + 256 * 1024, &mbi, sizeof mbi) != 0 && mbi.State == MEM_COMMIT);
        CHECK(CreatePipe(&rd, &wr, NULL, 0));
        if (rd && wr) {
            CHECK(WriteFile(wr, "guest kernel IO", 16, &written, NULL) && written == 16);
            CHECK(ReadFile(rd, buffer + 256 * 1024, 16, &read, NULL) && read == 16);
            CHECK(!memcmp(buffer + 256 * 1024, "guest kernel IO", 16));
            CloseHandle(rd); CloseHandle(wr);
        }
        guest_free(buffer);
    }
#endif
    CPU c = {0};
    c.esp = H2G(guest_alloc(4096, 16)) + 2048;
    uint32_t out = H2G(guest_alloc(16, 16));
    hle_register_libc(); hle_register_cxx(); hle_register_alsa();
    test_tree(c);

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
