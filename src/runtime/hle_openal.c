/*
 * hle_openal.c - OpenAL, which the cabinet's sound libraries play through.
 *
 * snd_util.so (CRI ADX) and librnalindbergh_jr.so are lifted by
 * tools/recomp/solib.py; their output end is OpenAL, answered here by the
 * host's own OpenAL (macOS's framework, openal-soft on Android). Every call
 * takes 32-bit integers, floats or pointers, so each is a typed thunk; the
 * device and context are host pointers, so the guest holds small tokens.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif

#include "lindbergh_rt.h"

static float argf(CPU *c, unsigned i) { uint32_t b = A32(i); float f; memcpy(&f, &b, 4); return f; }

/* ---- device and context handles ---- */
#define TOK_DEV 0x414C4400u     /* "ALD" */
#define TOK_CTX 0x414C4300u     /* "ALC" */
static ALCdevice  *g_dev[4];
static ALCcontext *g_ctx[8];

static ALCdevice *dev_of(uint32_t t)
{
    return (t & ~0xFFu) == TOK_DEV && (t & 0xFF) < 4 ? g_dev[t & 0xFF] : NULL;
}
static ALCcontext *ctx_of(uint32_t t)
{
    return (t & ~0xFFu) == TOK_CTX && (t & 0xFF) < 8 ? g_ctx[t & 0xFF] : NULL;
}

static void h_alcOpenDevice(CPU *c)
{
    /* The cabinet named its own device; the host's default is the one to use. */
    fprintf(stderr, "[al] alcOpenDevice(%s) -> default device\n", A32(0) ? ASTR(0) : "NULL");
    ALCdevice *d = alcOpenDevice(NULL);
    if (!d) { fprintf(stderr, "[al] no audio device\n"); RET(0); return; }
    for (unsigned i = 0; i < 4; i++)
        if (!g_dev[i]) { g_dev[i] = d; RET(TOK_DEV | i); return; }
    RET(0);
}
static void h_alcCloseDevice(CPU *c)
{
    ALCdevice *d = dev_of(A32(0));
    if (d) { alcCloseDevice(d); g_dev[A32(0) & 0xFF] = NULL; }
    RET(d ? 1 : 0);
}
static void h_alcCreateContext(CPU *c)
{
    ALCdevice *d = dev_of(A32(0));
    ALCcontext *x = d ? alcCreateContext(d, (const ALCint *)APTR(1)) : NULL;
    if (!x) { RET(0); return; }
    for (unsigned i = 0; i < 8; i++)
        if (!g_ctx[i]) { g_ctx[i] = x; RET(TOK_CTX | i); return; }
    RET(0);
}
static void h_alcDestroyContext(CPU *c)
{
    ALCcontext *x = ctx_of(A32(0));
    if (x) { alcDestroyContext(x); g_ctx[A32(0) & 0xFF] = NULL; }
}
static void h_alcMakeContextCurrent(CPU *c) { RET(alcMakeContextCurrent(ctx_of(A32(0)))); }
static void h_alcProcessContext(CPU *c)     { ALCcontext *x = ctx_of(A32(0)); if (x) alcProcessContext(x); }
static void h_alcGetError(CPU *c)           { RET(alcGetError(dev_of(A32(0)))); }

/* ---- AL ---- */
static void h_alGetError(CPU *c)     { RET(alGetError()); }
static void h_alGenBuffers(CPU *c)   { alGenBuffers(AI32(0), (ALuint *)APTR(1)); }
static void h_alGenSources(CPU *c)   { alGenSources(AI32(0), (ALuint *)APTR(1)); }
static void h_alDeleteBuffers(CPU *c){ alDeleteBuffers(AI32(0), (const ALuint *)APTR(1)); }
static void h_alDeleteSources(CPU *c){ alDeleteSources(AI32(0), (const ALuint *)APTR(1)); }
static void h_alIsBuffer(CPU *c)     { RET(alIsBuffer(A32(0))); }
static void h_alIsSource(CPU *c)     { RET(alIsSource(A32(0))); }
static void h_alBufferData(CPU *c)
{
    alBufferData(A32(0), A32(1), APTR(2), AI32(3), AI32(4));
    /* LINDBERGH_AL_LEVEL=1: peak level of the 16-bit audio per format and
     * rate, every 500 buffers of each - music and effects stream separately */
    static int on = -1;
    static struct { uint32_t fmt; int freq, n, peak; } st[8];
    if (on < 0) { const char *v = getenv("LINDBERGH_AL_LEVEL"); on = v && *v == '1'; }
    if (on && APTR(2) && (A32(1) == AL_FORMAT_MONO16 || A32(1) == AL_FORMAT_STEREO16)) {
        int k = 0;
        while (k < 7 && st[k].n && (st[k].fmt != A32(1) || st[k].freq != AI32(4))) k++;
        st[k].fmt = A32(1); st[k].freq = AI32(4);
        const int16_t *p = (const int16_t *)APTR(2);
        for (int i = 0; i < AI32(3) / 2; i++) { int v = p[i] < 0 ? -p[i] : p[i]; if (v > st[k].peak) st[k].peak = v; }
        if (++st[k].n % 500 == 0) {
            fprintf(stderr, "[al] %s %d Hz: buffers %d, peak level %d / 32767\n",
                    A32(1) == AL_FORMAT_STEREO16 ? "stereo" : "mono", AI32(4), st[k].n, st[k].peak);
            st[k].peak = 0;
        }
    }
}
static void h_alSourcei(CPU *c)      { alSourcei(A32(0), A32(1), AI32(2)); }
static void h_alGetSourcei(CPU *c)   { alGetSourcei(A32(0), A32(1), (ALint *)APTR(2)); }
static void h_alSourcefv(CPU *c)     { alSourcefv(A32(0), A32(1), (const ALfloat *)APTR(2)); }
static void h_alSource3f(CPU *c)     { alSource3f(A32(0), A32(1), argf(c, 2), argf(c, 3), argf(c, 4)); }
static void h_alListenerfv(CPU *c)   { alListenerfv(A32(0), (const ALfloat *)APTR(1)); }
static void h_alListener3f(CPU *c)   { alListener3f(A32(0), argf(c, 1), argf(c, 2), argf(c, 3)); }
static void h_alSourcePlay(CPU *c)   { alSourcePlay(A32(0)); }
static void h_alSourcePause(CPU *c)  { alSourcePause(A32(0)); }
static void h_alSourceStop(CPU *c)   { alSourceStop(A32(0)); }
static void h_alSourcePlayv(CPU *c)  { alSourcePlayv(AI32(0), (const ALuint *)APTR(1)); }
static void h_alSourceQueueBuffers(CPU *c)   { alSourceQueueBuffers(A32(0), AI32(1), (const ALuint *)APTR(2)); }
static void h_alSourceUnqueueBuffers(CPU *c) { alSourceUnqueueBuffers(A32(0), AI32(1), (ALuint *)APTR(2)); }

void hle_register_openal(void)
{
    hle_bind("alcOpenDevice", h_alcOpenDevice);
    hle_bind("alcCloseDevice", h_alcCloseDevice);
    hle_bind("alcCreateContext", h_alcCreateContext);
    hle_bind("alcDestroyContext", h_alcDestroyContext);
    hle_bind("alcMakeContextCurrent", h_alcMakeContextCurrent);
    hle_bind("alcProcessContext", h_alcProcessContext);
    hle_bind("alcGetError", h_alcGetError);
    hle_bind("alGetError", h_alGetError);
    hle_bind("alGenBuffers", h_alGenBuffers);
    hle_bind("alGenSources", h_alGenSources);
    hle_bind("alDeleteBuffers", h_alDeleteBuffers);
    hle_bind("alDeleteSources", h_alDeleteSources);
    hle_bind("alIsBuffer", h_alIsBuffer);
    hle_bind("alIsSource", h_alIsSource);
    hle_bind("alBufferData", h_alBufferData);
    hle_bind("alSourcei", h_alSourcei);
    hle_bind("alGetSourcei", h_alGetSourcei);
    hle_bind("alSourcefv", h_alSourcefv);
    hle_bind("alSource3f", h_alSource3f);
    hle_bind("alListenerfv", h_alListenerfv);
    hle_bind("alListener3f", h_alListener3f);
    hle_bind("alSourcePlay", h_alSourcePlay);
    hle_bind("alSourcePause", h_alSourcePause);
    hle_bind("alSourceStop", h_alSourceStop);
    hle_bind("alSourcePlayv", h_alSourcePlayv);
    hle_bind("alSourceQueueBuffers", h_alSourceQueueBuffers);
    hle_bind("alSourceUnqueueBuffers", h_alSourceUnqueueBuffers);
}
