/*
 * hle_segaapi.c - libsegaapi, the Lindbergh's sound card API, as a mixer.
 *
 * The House of the Dead 4 plays everything through SEGAAPI. It creates
 * buffers - 24 mono ones for effects, 4 small stereo rings for the streamed
 * music and voices - writes PCM into the memory each one maps, and starts,
 * stops, loops and repositions them, reading the play position back to know
 * where to refill a ring. So the position has to be the one the listener is
 * actually hearing, and the way to have that is to mix here: an SDL audio
 * callback walks every playing buffer straight out of guest memory, at the
 * buffer's own rate, and advances its position by exactly what it played.
 *
 * Levels: each source channel has a volume and up to seven sends, each send
 * routed to an output (0 front left, 1 front right, 2 centre, 4/5 rear L/R;
 * 0xFFFF0001 is "not routed"). A send's level times the channel's volume is
 * what reaches that output. Volumes and levels are 0..0xFFFFFFFF.
 *
 * The buffer config, as libsegaapi lays it out on i386:
 *      +0   dwPriority      +4  dwSampleRate   +8  dwSampleFormat
 *      +12  byNumChans      +16 dwReserved     +20 hUserData
 *      +24  mapData.dwSize  +28 mapData.dwOffset  +32 mapData.hBufferHdr
 * Sample formats: 0x20 signed 16-bit, 0x04 unsigned 8-bit.
 *
 * Muted (the player's setting, or LINDBERGH_MUTE) the mix still runs - the
 * game depends on positions moving - and only the output is silence.
 * LINDBERGH_SEGA_METER=1 logs the mix's peak level every two seconds, which
 * is how a muted run checks there is sound to hear.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lindbergh_rt.h"

#ifndef _WIN32
#include <SDL.h>
#include "host_settings.h"
#define HAVE_MIXER 1
#endif

#define SEGA_OK 0
/* flags: 1 a synth (effect) voice, 2 the caller's own memory, 4 mapped memory */
#define HABUF_ALLOC_USER_MEM  2u
#define OUT_RATE 48000
#define NSEND 7
#define NOT_ROUTED 0xFFFF0001u

/* Each buffer is a guest block the game holds a pointer to. */
typedef struct SegaBuf {
    uint32_t user;
    uint32_t data, size;
    uint32_t rate, format, chans;
    int      playing, paused, looping, release;
    double   pos;                     /* in frames */
    uint32_t loop_start, loop_end, end;   /* bytes */
    float    vol[2];
    float    atten;                   /* SetSynthParam 0, as a gain */
    float    level[2][NSEND];
    uint32_t route[2][NSEND];
    struct SegaBuf *next;             /* every buffer, for the mixer */
} SegaBuf;

static SegaBuf *g_bufs;
static float    g_io[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };

#ifdef HAVE_MIXER
static SDL_mutex *g_lock;
#define LOCK()   SDL_LockMutex(g_lock)
#define UNLOCK() SDL_UnlockMutex(g_lock)
#else
#define LOCK()   ((void)0)
#define UNLOCK() ((void)0)
#endif

static SegaBuf *buf_of(uint32_t h) { return h ? (SegaBuf *)GP(h) : NULL; }
static uint32_t frame_bytes(const SegaBuf *b) { return (b->format == 0x04 ? 1 : 2) * (b->chans ? b->chans : 1); }
static float level01(uint32_t v) { return (float)(v / 4294967295.0); }

#ifdef HAVE_MIXER
static float sample(const SegaBuf *b, uint32_t frame, int ch)
{
    uint32_t off = frame * frame_bytes(b) + (uint32_t)ch * (b->format == 0x04 ? 1 : 2);
    if (off + 2 > b->size) return 0;
    if (b->format == 0x04) return (((const uint8_t *)GP(b->data))[off] - 128) / 128.0f;
    int16_t s;
    memcpy(&s, (const uint8_t *)GP(b->data) + off, 2);
    return s / 32768.0f;
}

/* How much of one source channel reaches the left and right outputs. */
static void gains(const SegaBuf *b, int ch, float *l, float *r)
{
    *l = *r = 0;
    for (int s = 0; s < NSEND; s++) {
        float g = b->level[ch][s];
        if (g <= 0) continue;
        switch (b->route[ch][s]) {
        case 0: case 4: *l += g; break;
        case 1: case 5: *r += g; break;
        case 2: case 3: *l += g * 0.707f; *r += g * 0.707f; break;
        default: break;
        }
    }
    *l *= b->vol[ch] * b->atten * g_io[0];
    *r *= b->vol[ch] * b->atten * g_io[1];
}

static void mix(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    float *out = (float *)stream;
    int frames = len / (int)(2 * sizeof(float));
    memset(stream, 0, (size_t)len);
    LOCK();
    for (SegaBuf *b = g_bufs; b; b = b->next) {
        if (!b->playing || b->paused || !b->data || !b->size) continue;
        uint32_t fb = frame_bytes(b), total = b->size / fb;
        uint32_t end = (b->end && b->end <= b->size ? b->end : b->size) / fb;
        uint32_t lend = (b->loop_end && b->loop_end <= b->size ? b->loop_end : b->size) / fb;
        uint32_t lstart = b->loop_start / fb;
        double step = (b->rate ? b->rate : OUT_RATE) / (double)OUT_RATE;
        int chans = b->chans >= 2 ? 2 : 1;
        float gl[2], gr[2];
        for (int ch = 0; ch < chans; ch++) gains(b, ch, &gl[ch], &gr[ch]);
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)b->pos;
            if (b->looping && !b->release ? f >= lend : f >= end) {
                if (b->looping && !b->release && lend > lstart) { b->pos = lstart + (b->pos - lend); f = (uint32_t)b->pos; }
                else { b->playing = 0; b->pos = end; break; }
            }
            if (f >= total) { b->playing = 0; break; }
            for (int ch = 0; ch < chans; ch++) {
                float s = sample(b, f, ch);
                out[2 * i]     += s * gl[ch];
                out[2 * i + 1] += s * gr[ch];
            }
            b->pos += step;
        }
    }
    UNLOCK();

    static float peak; static unsigned blocks;
    static int meter = -1;
    if (meter < 0) { const char *v = getenv("LINDBERGH_SEGA_METER"); meter = v && *v == '1'; }
    HostSettings *st = settings();
    float vol = st->muted ? 0.0f : st->volume / 100.0f;
    for (int i = 0; i < frames * 2; i++) {
        float a = fabsf(out[i]);
        if (a > peak) peak = a;
        /* soft knee: untouched below 0.8, then eased towards 1.0, so a
         * firefight of a dozen voices compresses instead of clipping */
        float v = out[i], m = fabsf(v);
        if (m > 0.8f) v = copysignf(0.8f + 0.2f * tanhf((m - 0.8f) / 0.2f), v);
        out[i] = v * vol;
    }
    if (meter && ++blocks * (unsigned)frames >= 2 * OUT_RATE) {
        fprintf(stderr, "[sega-api] mix peak %.3f%s\n", peak, st->muted ? " (muted)" : "");
        peak = 0; blocks = 0;
    }
}

static void open_output(void)
{
    static int done;
    if (done) return;
    done = 1;
    g_lock = SDL_CreateMutex();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[sega-api] no audio: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want = { 0 }, got;
    want.freq = OUT_RATE; want.format = AUDIO_F32SYS; want.channels = 2;
    want.samples = 1024; want.callback = mix;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!dev) { fprintf(stderr, "[sega-api] no audio device: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(dev, 0);
    fprintf(stderr, "[sega-api] mixing at %d Hz\n", got.freq);
}
#else
static void open_output(void) { }
#endif

/* ---- the API ---- */

/* SEGAAPI_CreateBuffer(config, callback, flags, *handle) */
static void h_create(CPU *c)
{
    open_output();
    uint32_t cfg = A32(0), flags = A32(2), out = A32(3);
    SegaBuf *b = (SegaBuf *)guest_alloc(sizeof *b, 16);
    memset(b, 0, sizeof *b);
    b->rate = rd32(cfg + 4); b->format = rd32(cfg + 8); b->chans = rd32(cfg + 12);
    b->user = rd32(cfg + 20); b->size = rd32(cfg + 24);
    if (flags & HABUF_ALLOC_USER_MEM) {
        b->data = rd32(cfg + 32);
    } else {
        void *mem = guest_alloc(b->size ? b->size : 16, 16);
        memset(mem, 0, b->size ? b->size : 16);
        b->data = H2G(mem);
        wr32(cfg + 32, b->data);
    }
    b->end = b->loop_end = b->size;
    b->vol[0] = b->vol[1] = 1;
    b->atten = 1;
    for (int ch = 0; ch < 2; ch++)
        for (int s = 0; s < NSEND; s++) b->route[ch][s] = NOT_ROUTED;
    /* until told otherwise: channel 0 left (and right, if mono), 1 right */
    b->route[0][0] = 0; b->level[0][0] = 1;
    if (b->chans >= 2) { b->route[1][1] = 1; b->level[1][1] = 1; }
    else               { b->route[0][1] = 1; b->level[0][1] = 1; }
    LOCK(); b->next = g_bufs; g_bufs = b; UNLOCK();
    if (out) wr32(out, H2G(b));
    RET(SEGA_OK);
}

static void h_destroy(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    LOCK();
    for (SegaBuf **p = &g_bufs; b && *p; p = &(*p)->next)
        if (*p == b) { *p = b->next; break; }
    UNLOCK();
    RET(SEGA_OK);
}
static void h_play(CPU *c)  { SegaBuf *b = buf_of(A32(0)); LOCK(); if (b) { b->playing = 1; b->paused = 0; b->release = 0; } UNLOCK(); RET(SEGA_OK); }
static void h_stop(CPU *c)  { SegaBuf *b = buf_of(A32(0)); LOCK(); if (b) { b->playing = 0; b->paused = 0; } UNLOCK(); RET(SEGA_OK); }
static void h_pause(CPU *c) { SegaBuf *b = buf_of(A32(0)); LOCK(); if (b) b->paused = 1; UNLOCK(); RET(SEGA_OK); }
/* status: 0 stop, 1 active, 2 pause */
static void h_status(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    RET(!b || !b->playing ? 0 : b->paused ? 2 : 1);
}
static void h_get_pos(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    RET(b ? (uint32_t)b->pos * frame_bytes(b) : 0);
}
static void h_set_pos(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    LOCK(); if (b) b->pos = A32(1) / (double)frame_bytes(b); UNLOCK();
    RET(SEGA_OK);
}
static void h_get_user(CPU *c) { SegaBuf *b = buf_of(A32(0)); RET(b ? b->user : 0); }
static void h_set_user(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->user = A32(1); RET(SEGA_OK); }
static void h_set_loop(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->looping = A32(1) != 0; RET(SEGA_OK); }
static void h_set_release(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->release = A32(1) != 0; RET(SEGA_OK); }
static void h_set_start(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->loop_start = A32(1); RET(SEGA_OK); }
static void h_set_endloop(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->loop_end = A32(1); RET(SEGA_OK); }
static void h_set_end(CPU *c)  { SegaBuf *b = buf_of(A32(0)); if (b) b->end = A32(1); RET(SEGA_OK); }
static void h_set_rate(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->rate = A32(1); RET(SEGA_OK); }
/* SetChannelVolume(h, channel, volume) */
static void h_vol(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    if (b && A32(1) < 2) b->vol[A32(1)] = level01(A32(2));
    RET(SEGA_OK);
}
/* SetSendRouting(h, channel, send, destination) */
static void h_route(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    if (b && A32(1) < 2 && A32(2) < NSEND) b->route[A32(1)][A32(2)] = A32(3);
    RET(SEGA_OK);
}
/* SetSendLevel(h, channel, send, level) */
static void h_send(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    if (b && A32(1) < 2 && A32(2) < NSEND) b->level[A32(1)][A32(2)] = level01(A32(3));
    RET(SEGA_OK);
}
/* SetIOVolume(port, volume): the card's outputs */
static void h_iovol(CPU *c) { if (A32(0) < 8) g_io[A32(0)] = level01(A32(1)); RET(SEGA_OK); }
/* The data is read where the game wrote it, so an update needs nothing. */
static void h_ok(CPU *c) { RET(SEGA_OK); }

/* libkswapapi: the cabinet's key-swap helper. Nothing to collect. */
static void h_kswap_collect(CPU *c) { RET(0); }

/* LINDBERGH_SEGA_LOG=1: every call but the constant refills, with arguments. */
static void seg_log(const char *name, CPU *c)
{
    static int on = -1;
    static unsigned n;
    if (on < 0) { const char *v = getenv("LINDBERGH_SEGA_LOG"); on = v && *v == '1'; }
    if (!on || n++ > 4000) return;
    fprintf(stderr, "[sega-api] %s(%#x, %#x, %#x, %#x)\n", name, A32(0), A32(1), A32(2), A32(3));
}
#define LOGGED(name, fn) static void L_##fn(CPU *c) { seg_log(name, c); fn(c); }
LOGGED("CreateBuffer", h_create)   LOGGED("DestroyBuffer", h_destroy)
LOGGED("Play", h_play)             LOGGED("Stop", h_stop)            LOGGED("Pause", h_pause)
LOGGED("SetPlaybackPosition", h_set_pos)  LOGGED("SetLoopState", h_set_loop)
LOGGED("SetReleaseState", h_set_release)  LOGGED("SetStartLoopOffset", h_set_start)
LOGGED("SetEndLoopOffset", h_set_endloop) LOGGED("SetEndOffset", h_set_end)
LOGGED("SetSampleRate", h_set_rate)       LOGGED("SetChannelVolume", h_vol)
LOGGED("SetSendRouting", h_route)         LOGGED("SetSendLevel", h_send)
LOGGED("SetIOVolume", h_iovol)
/* SetSynthParam(h, param, value). Param 0 is the voice's attenuation in
 * centibels (HOTD4 sets 21..98); the rest it sets are envelope and filter
 * times left at their "off" value (-12000), which a plain player matches. */
static void h_synth(CPU *c)
{
    seg_log("SetSynthParam", c);
    SegaBuf *b = buf_of(A32(0));
    if (b && A32(1) == 0) {
        int cb = AI32(2);
        b->atten = cb <= 0 ? 1.0f : (float)pow(10.0, -cb / 200.0);
    }
    RET(SEGA_OK);
}

void hle_register_segaapi(void)
{
    hle_bind("SEGAAPI_Init", h_ok);
    hle_bind("SEGAAPI_Exit", h_ok);
    hle_bind("SEGAAPI_Reset", h_ok);
    hle_bind("SEGAAPI_UpdateBuffer", h_ok);
    hle_bind("SEGAAPI_CreateBuffer", L_h_create);
    hle_bind("SEGAAPI_DestroyBuffer", L_h_destroy);
    hle_bind("SEGAAPI_Play", L_h_play);
    hle_bind("SEGAAPI_Stop", L_h_stop);
    hle_bind("SEGAAPI_Pause", L_h_pause);
    hle_bind("SEGAAPI_GetPlaybackStatus", h_status);
    hle_bind("SEGAAPI_GetPlaybackPosition", h_get_pos);
    hle_bind("SEGAAPI_SetPlaybackPosition", L_h_set_pos);
    hle_bind("SEGAAPI_GetUserData", h_get_user);
    hle_bind("SEGAAPI_SetUserData", h_set_user);
    hle_bind("SEGAAPI_SetLoopState", L_h_set_loop);
    hle_bind("SEGAAPI_SetReleaseState", L_h_set_release);
    hle_bind("SEGAAPI_SetStartLoopOffset", L_h_set_start);
    hle_bind("SEGAAPI_SetEndLoopOffset", L_h_set_endloop);
    hle_bind("SEGAAPI_SetEndOffset", L_h_set_end);
    hle_bind("SEGAAPI_SetSampleRate", L_h_set_rate);
    hle_bind("SEGAAPI_SetChannelVolume", L_h_vol);
    hle_bind("SEGAAPI_SetSendRouting", L_h_route);
    hle_bind("SEGAAPI_SetSendLevel", L_h_send);
    hle_bind("SEGAAPI_SetIOVolume", L_h_iovol);
    hle_bind("SEGAAPI_SetSynthParam", h_synth);
    hle_bind("kswap_collect", h_kswap_collect);
}
