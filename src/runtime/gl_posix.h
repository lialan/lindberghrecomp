/*
 * gl_posix.h - what the typed GL thunks (generated and hand-written) share.
 *
 * Used on 64-bit POSIX hosts in place of hle_gl.c's raw dword forwarding,
 * which is only correct for a 32-bit __stdcall host.
 */
#ifndef GL_POSIX_H
#define GL_POSIX_H

#define GL_SILENCE_DEPRECATION 1
#if defined(__APPLE__)
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <string.h>
#include "lindbergh_rt.h"

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER   0x88EB
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif

static inline float gl_arg_f(CPU *c, unsigned slot)
{
    uint32_t b = A32(slot);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static inline double gl_arg_d(CPU *c, unsigned slot)
{
    uint64_t b = (uint64_t)A32(slot) | ((uint64_t)A32(slot + 1) << 32);
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* A gl*Pointer / glDrawElements / glTexImage "pointer" is a byte offset when a
 * buffer is bound to the matching target, and a client address otherwise. */
const void *gl_ptr_or_offset(unsigned target, uint32_t va);

/* LINDBERGH_GL_TRACE=1: name each GL call as it is made, so the last line
 * before a fault is the GL call that faulted. */
extern int g_gl_trace;
void gl_trace_call(const char *name);
#define gl_trace(name) do { if (g_gl_trace) gl_trace_call(name); } while (0)

/* LINDBERGH_GLERR=1: glGetError after every call; the first call to raise each
 * distinct error is named. */
extern int g_gl_errcheck;
void gl_check_error(const char *name);
#define gl_check(name) do { if (g_gl_errcheck) gl_check_error(name); } while (0)

void gl_register_generated(void);

#endif
