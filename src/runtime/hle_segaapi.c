/*
 * hle_segaapi.c - libsegaapi, the Lindbergh's sound card API, silent for now.
 *
 * The House of the Dead 4 plays everything through SEGAAPI: it creates
 * buffers, fills them (or has the library allocate them and writes into the
 * mapping it is given), and starts, stops and loops them. This first version
 * keeps the bookkeeping honest - buffers exist, have memory, remember their
 * user data, and report a play position that moves while they play - and
 * makes no sound. That is enough for a game that waits on its sounds to get
 * past them. The OpenAL mixer comes once the game boots.
 *
 * The buffer config, as libsegaapi lays it out on i386:
 *      +0   dwPriority      +4  dwSampleRate   +8  dwSampleFormat
 *      +12  byNumChans      +16 dwReserved     +20 hUserData
 *      +24  mapData.dwSize  +28 mapData.dwOffset  +32 mapData.hBufferHdr
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lindbergh_rt.h"

#define SEGA_OK 0
#define HABUF_ALLOC_USER_MEM  1u
#define HABUF_USE_MAPPED_MEM  2u

/* Each buffer is a guest block the game holds a pointer to. */
typedef struct {
    uint32_t user;
    uint32_t data, size;
    uint32_t rate, format, chans;
    uint32_t playing, looping;
    uint32_t start_lo;            /* host ms when play began */
    uint32_t pos_at_start;
    uint32_t end;
} SegaBuf;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static SegaBuf *buf_of(uint32_t h) { return h ? (SegaBuf *)GP(h) : NULL; }

static uint32_t bytes_per_frame(const SegaBuf *b)
{
    /* formats: 0 PCM16, 1 PCM8, 2 float (in practice 16-bit) */
    uint32_t bps = b->format == 1 ? 1 : 2;
    return bps * (b->chans ? b->chans : 1);
}

static uint32_t position(const SegaBuf *b)
{
    if (!b->playing) return b->pos_at_start;
    uint64_t frames = (uint64_t)(now_ms() - b->start_lo) * (b->rate ? b->rate : 48000) / 1000;
    uint64_t pos = b->pos_at_start + frames * bytes_per_frame(b);
    uint32_t end = b->end ? b->end : b->size;
    if (end && pos >= end) {
        if (b->looping) pos %= end;
        else pos = end;
    }
    return (uint32_t)pos;
}

/* SEGAAPI_CreateBuffer(config, callback, flags, *handle) */
static void h_create(CPU *c)
{
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
    b->end = b->size;
    if (out) wr32(out, H2G(b));
    RET(SEGA_OK);
}

static void h_destroy(CPU *c)  { RET(SEGA_OK); }
static void h_play(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    if (b && !b->playing) { b->playing = 1; b->start_lo = now_ms(); }
    RET(SEGA_OK);
}
static void h_stop(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    if (b) { b->pos_at_start = position(b); b->playing = 0; }
    RET(SEGA_OK);
}
static void h_pause(CPU *c) { h_stop(c); }
/* status: 0 stop, 1 active, 2 pause */
static void h_status(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    if (b && b->playing && !b->looping && position(b) >= (b->end ? b->end : b->size)) {
        b->pos_at_start = position(b);
        b->playing = 0;
    }
    RET(b && b->playing ? 1 : 0);
}
static void h_get_pos(CPU *c) { SegaBuf *b = buf_of(A32(0)); RET(b ? position(b) : 0); }
static void h_set_pos(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    if (b) { b->pos_at_start = A32(1); b->start_lo = now_ms(); }
    RET(SEGA_OK);
}
static void h_get_user(CPU *c) { SegaBuf *b = buf_of(A32(0)); RET(b ? b->user : 0); }
static void h_set_user(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->user = A32(1); RET(SEGA_OK); }
static void h_set_loop(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->looping = A32(1); RET(SEGA_OK); }
static void h_set_end(CPU *c)  { SegaBuf *b = buf_of(A32(0)); if (b) b->end = A32(1); RET(SEGA_OK); }
static void h_set_rate(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->rate = A32(1); RET(SEGA_OK); }
static void h_ok(CPU *c) { RET(SEGA_OK); }

/* libkswapapi: the cabinet's key-swap helper. Nothing to collect. */
static void h_kswap_collect(CPU *c) { RET(0); }

void hle_register_segaapi(void)
{
    hle_bind("SEGAAPI_Init", h_ok);
    hle_bind("SEGAAPI_Exit", h_ok);
    hle_bind("SEGAAPI_Reset", h_ok);
    hle_bind("SEGAAPI_CreateBuffer", h_create);
    hle_bind("SEGAAPI_DestroyBuffer", h_destroy);
    hle_bind("SEGAAPI_UpdateBuffer", h_ok);
    hle_bind("SEGAAPI_Play", h_play);
    hle_bind("SEGAAPI_Stop", h_stop);
    hle_bind("SEGAAPI_Pause", h_pause);
    hle_bind("SEGAAPI_GetPlaybackStatus", h_status);
    hle_bind("SEGAAPI_GetPlaybackPosition", h_get_pos);
    hle_bind("SEGAAPI_SetPlaybackPosition", h_set_pos);
    hle_bind("SEGAAPI_GetUserData", h_get_user);
    hle_bind("SEGAAPI_SetUserData", h_set_user);
    hle_bind("SEGAAPI_SetLoopState", h_set_loop);
    hle_bind("SEGAAPI_SetEndOffset", h_set_end);
    hle_bind("SEGAAPI_SetEndLoopOffset", h_set_end);
    hle_bind("SEGAAPI_SetSampleRate", h_set_rate);
    hle_bind("SEGAAPI_SetStartLoopOffset", h_ok);
    hle_bind("SEGAAPI_SetReleaseState", h_ok);
    hle_bind("SEGAAPI_SetChannelVolume", h_ok);
    hle_bind("SEGAAPI_SetIOVolume", h_ok);
    hle_bind("SEGAAPI_SetSendLevel", h_ok);
    hle_bind("SEGAAPI_SetSendRouting", h_ok);
    hle_bind("SEGAAPI_SetSynthParam", h_ok);
    hle_bind("kswap_collect", h_kswap_collect);
}
