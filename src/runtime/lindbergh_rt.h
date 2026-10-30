/*
 * lindbergh_rt.h - runtime for statically recompiled Lindbergh games.
 *
 * The CPU model is pcrecomp's (pcrecomp/runtime/recomp32_cpu/cpu.h): an
 * explicit `CPU *c` threaded through every lifted function, flat memory where
 * a register holds a real 32-bit host address. That means the host must be a
 * 32-bit build - the game's segments are mapped at the virtual addresses it
 * was linked for, and on Linux/i386 those are down at 0x08048000.
 *
 * What this header adds is the Lindbergh side of the boundary: a Linux/i386
 * process to be, a kernel to call, and a set of shared libraries the board
 * provided that we have to provide instead.
 */
#ifndef LINDBERGH_RT_H
#define LINDBERGH_RT_H

/* Generated code calls abort() wherever an instruction did not lift, so the
 * declaration has to travel with the header the generated code includes. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

/* cpu.h defaults RECOMP_TODO to a bare abort(), and invites a consumer to
 * define its own first. Take the invitation: an abort() here raises
 * __fastfail past every handler with no message, so an unimplemented
 * instruction is indistinguishable from memory corruption - which cost four
 * wrong diagnoses before this existed. */
void lindbergh_todo(uint32_t va, const char *text);
#define RECOMP_TODO(va, text) lindbergh_todo((va), (text))

#include "cpu.h"
#include "posix_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- guest process ---- */

/* Map an ELF's PT_LOADs at their original virtual addresses and build the
 * initial stack (argc/argv/envp/auxv) the way the kernel does, so the game's
 * _start finds what it expects. Returns 0, or -1 with the reason on stderr. */
int  guest_load(const char *elf_path, int argc, char **argv);

/* Entry point VA from the ELF header, valid after guest_load. */
uint32_t guest_entry(void);

/* Initialise a CPU to enter the guest: esp at the built stack, everything
 * else zero, exactly as Linux hands a fresh process to _start. */
void guest_init_cpu(CPU *c);

/* Guest break, for the brk/sbrk syscalls. */
uint32_t guest_brk(uint32_t newbrk);

/* ---- kernel ---- */

/* `int 0x80` and the vDSO's sysenter both land here. Number in eax, arguments
 * in ebx/ecx/edx/esi/edi/ebp, result back into eax (negative errno on
 * failure), per the Linux i386 calling convention. */
void linux_syscall(CPU *c);

/* ---- shared libraries ---- */

/* Every PLT import in the game gets an HLE_* id; recomp_imports.h is
 * generated from the ELF and lists them. */
#include "recomp_imports.h"

/* Imports of the dlopened libraries lifted by tools/recomp/solib.py that the
 * main ELF does not import itself. */
#if __has_include("recomp_solib.h")
#include "recomp_solib.h"
#else
#define SOLIB_IMPORTS(X)
#endif

#define HLE_ENUM(id, name) id,
typedef enum { HLE_IMPORTS(HLE_ENUM) SOLIB_IMPORTS(HLE_ENUM) HLE_COUNT } HleId;
#undef HLE_ENUM

/* A library function's body. Reads its arguments off the guest stack and
 * leaves the return value in eax, the cdecl the ELF was built for. */
typedef void (*HleHandler)(CPU *c);

/* One slot per import. A game project fills in the ones it needs; the toolkit
 * ships the ones every title shares. A slot left null aborts naming itself,
 * which is the to-do list. */
extern HleHandler g_hle_handlers[];

void hle_call(CPU *c, HleId id);

/* When set, every import marked in g_gl_ids goes through the hook instead of
 * straight to its handler (stereo.c: record a frame's GL calls, replay them
 * for the second eye). */
typedef void (*GlHook)(CPU *c, HleId id, HleHandler fn);
extern GlHook g_gl_hook;
extern unsigned char g_gl_ids[];

/* GL entry points a game fetches by name (glXGetProcAddressARB) instead of
 * importing get ids of their own after the imports' - HLE_COUNT up to
 * HLE_ID_MAX - so the hook sees calls through them too, by the same name. */
#define HLE_NAMED_MAX 512
#define HLE_ID_MAX (HLE_COUNT + HLE_NAMED_MAX)
HleId hle_named_id(const char *name);   /* HLE_ID_MAX when out of room */

/* The PLT stub address for a named import, or 0. What glXGetProcAddressARB
 * hands back, so the guest's indirect call through it routes like any other. */
uint32_t hle_plt_address(const char *name);

/* Give a named import a body. Binding by name and not by HLE_* id is what
 * keeps a handler file title-agnostic: HLE_memcpy only exists as an enumerator
 * if *this* game imports memcpy, so a file that said `g_hle_handlers[HLE_memcpy]`
 * would fail to compile against a game that does not. Returns 0 when the game
 * does not import that name, which is not an error - most do not import most
 * of them. */
int hle_bind(const char *name, HleHandler fn);

/* Bind every handler the toolkit ships. A game project calls this once, then
 * binds its own on top. */
void hle_register_all(void);
void hle_register_libc(void);
void hle_register_libc2(void);
void hle_register_cg(void);
void hle_register_io(void);
void hle_register_libc3(void);
void hle_register_pthread(void);
void hle_register_window(void);

/* ---- the guest side of a call ----
 *
 * The lifter turns `call <plt stub>` into hle_call() and pushes no return
 * address, so on entry esp points straight at argument 0. cdecl, so the
 * handler must not move esp - the caller's own `add esp, N` does that.
 */
#define A32(n)   rd32(c->esp + 4u * (unsigned)(n))
/* Guest address -> host pointer, keeping 0 as NULL: handlers test what they
 * were given for null, and 0 + base is not one. Lifted code uses plain G2H. */
static inline void *GP(uint32_t a) { return a ? G2H(a) : NULL; }   /* evaluates a once */
#define APTR(n)  ((void *)GP(A32(n)))
#define ASTR(n)  ((char *)GP(A32(n)))
#define AI32(n)  ((int32_t)A32(n))

/* Host pointer -> guest address. On a 64-bit host only memory inside the guest
 * window has a guest address; anything else is a host object leaking into the
 * game, which stops the process naming the caller instead of truncating the
 * pointer and failing somewhere else later. NULL maps to 0. */
uint32_t guest_addr_of(const void *p, const char *file, int line);
#define H2G(p)   guest_addr_of((const void *)(p), __FILE__, __LINE__)

/* RET takes an integer or a pointer. A pointer goes through H2G, so a handler
 * that returns what a host function returned is checked rather than truncated. */
/* 5 is clang/GCC's pointer_type_class; arrays and string literals decay to it. */
#define RET(v)   (c->eax = __builtin_classify_type(v) == 5 \
                     ? H2G((const void *)(uintptr_t)(v)) : (uint32_t)(uintptr_t)(v))

/* Memory the guest can see: the guest heap, in the window. Thread stacks,
 * strings handed to the game and its malloc all come from here. */
void    *guest_alloc(size_t n, size_t align);
void     guest_free(void *p);
size_t   guest_alloc_size(void *p);
/* A copy of a host string in guest memory, interned: the same text returns the
 * same address, so callers may treat it as static (strerror, getenv, GL). */
uint32_t guest_strdup_static(const char *s);
int      guest_mem_init(void);

/* A host FILE* as the guest holds it: a small guest block that carries the
 * host pointer. The guest never looks inside a FILE, it only passes it back. */
uint32_t guest_file_wrap(FILE *f);
FILE    *guest_file(uint32_t handle);
void     guest_file_release(uint32_t handle);
#define AFILE(n) guest_file(A32(n))

/* A function returning float or double returns it in st(0) on i386 SysV, not
 * in an XMM register - the caller does `fstp` to take it. Every math import
 * below goes back this way. */
#define RETF(v)  fpush(c, (double)(v))

/* Name for an id, for diagnostics. */
const char *hle_name(HleId id);

/* ---- lifted code ---- */

/* Address -> lifted function. Generated: recomp_funcs_list.h is the X-macro
 * of every VA that lifted, and recomp_dispatch.c turns it into a table. */
void dispatch(CPU *c, uint32_t va);
void dispatch_jmp(CPU *c, uint32_t va);

/* Run a guest function from host code and return its eax. Arguments are
 * pushed cdecl-style and esp is restored afterwards. */
uint32_t guest_call(CPU *c, uint32_t fn, const uint32_t *args, int nargs);

/* ---- bring-up diagnostics ----
 * A fault in lifted code tells a debugger nothing useful. These report the
 * guest machine instead: its registers, and the last 64 addresses dispatch()
 * sent control to. */
void guest_install_crash_handler(void);
void guest_set_current_cpu(CPU *c);
CPU *guest_current_cpu(void);
void guest_report_state(const char *why);
void host_quit(void);              /* end the game at the platform's request */
void guest_trace_dispatch(uint32_t va);
void guest_backtrace(CPU *c);

/* ---- guest function overrides ----
 * Replace a lifted function with a host one, by symbol name. For library code
 * that was statically linked into the game and so never passes through the
 * PLT - libXxf86vm being the reason this exists. */
uint32_t   guest_symbol(const char *name);
uint32_t   guest_copy_reloc(const char *name);   /* R_386_COPY slot, e.g. "stderr" */
int        guest_override(const char *name, HleHandler fn);
int        guest_override_va(const char *name, uint32_t va, HleHandler fn);
HleHandler guest_find_override(uint32_t va);
int        guest_find_override_index(uint32_t va);
HleHandler guest_override_at(int i);
void       hle_register_vidmode(void);
void       hle_register_matrix(void);
void       hle_register_console(void);
void       hle_register_sega(void);
void       hle_register_glut(void);
void       hle_register_cxx(void);
void       hle_register_dir(void);
void       hle_register_scan(void);

/* ---- host input ----
 *
 * One queue, filled by the window procedure and drained by whoever drives the
 * game loop. A light gun is a mouse: the cabinet's gun reports where on the
 * screen it points and whether the trigger is down, which is what a pointer
 * and a button already are. */
enum { HOST_IN_MOTION = 1, HOST_IN_MOUSE, HOST_IN_KEY, HOST_IN_KEYUP,
       HOST_IN_SPECIAL };

typedef struct { int kind, a, b, c, d; } HostInput;

int        hle_stub_call(CPU *c, uint32_t va);
/* The cabinet's switches and its gun, as a desktop can supply them. */
enum { CAB_P1_START = 1u << 0, CAB_P2_START = 1u << 1,
       CAB_TEST     = 1u << 2, CAB_SERVICE  = 1u << 3,
       CAB_P1_TRIGGER = 1u << 4, CAB_P1_RELOAD = 1u << 5,
       CAB_P1_ACTION  = 1u << 6, CAB_P1_FIREMODE = 1u << 7 };

typedef struct {
    unsigned coins[2];        /* only ever counts up, like a coin mech */
    unsigned buttons;
    int gun_x, gun_y;
    int screen_w, screen_h;
} CabinetInput;

void       guest_install_thread_handlers(void);
void       host_cabinet_input(CabinetInput *out);
unsigned   host_cabinet_buttons(void);
void       host_cabinet_switches_read(void);
void       host_audio_toggle_mute(void);
int        host_input_pop(HostInput *out);
void       host_mouse_state(int *x, int *y, int *buttons);

/* The host window and its GL context, shared by the GLX and GLUT seams. */
void       host_window_hint(int w, int h);
void       host_window_title(const char *title);
void       host_pace_fps(int fps);
void       host_gun_map(void (*fn)(int *x, int *y));   /* sight position -> the gun reading for it */
void       segaapi_master_gain(float g);          /* the libsegaapi mix, before its limiter */                 /* hold the game to fps (it relied on vsync) */
void       host_aim_default(double sx, double ox, double sy, double oy);  /* sight = s*pointer + o, set before the settings load */
void       hud_widen_enable(float f, int (*anchored)(void));   /* hud_widen.c */
void       hud_widen_layout(float lx, float lox, float ly, float loy, int vw);
int        host_window_open(const char *title);
int        host_gl_context(void);
void       host_pump(void);
void       host_swap(void);
void       hle_register_gl(void);
void       hle_register_zlib(void);
void       hle_register_openal(void);
void       hle_register_libc4(void);
void       hle_register_segaapi(void);
void       hle_register_glx_posix(void);
int        solib_dlopen(CPU *c, const char *path);
uint32_t   solib_dlsym(const char *name);

/* A callable address for a GL entry point the game asks for by name but does
 * not import. gl_token_call routes one; dispatch() consults it. */
uint32_t   gl_token_for(const char *name);
int        gl_token_call(CPU *c, uint32_t va);
int        gl_token_count(void);
void       lindbergh_window_size(int *w, int *h);

/* Emitted by the lifter wherever an instruction did not translate. Reports and
 * exits - never returns, because the guest state after a skipped instruction
 * is wrong in a way that shows up much later as something else. */


#ifdef __cplusplus
}
#endif
#endif /* LINDBERGH_RT_H */
