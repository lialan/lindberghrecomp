/*
 * hle_zlib.c - zlib's inflate, for a guest that hands over an i386 z_stream.
 *
 * The guest's z_stream is the 32-bit layout: fourteen 4-byte fields, four of
 * them pointers. The host's has 8-byte pointers, so it cannot be passed
 * through. Each guest stream gets a host z_stream of its own; every call
 * copies the guest's fields in, runs the host's inflate, and copies the
 * results back. The guest buffers live in the guest window, so next_in and
 * next_out translate both ways without copying any data.
 */

#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "lindbergh_rt.h"

/* struct z_stream_s on i386 */
enum { ZS_next_in = 0, ZS_avail_in = 4, ZS_total_in = 8, ZS_next_out = 12,
       ZS_avail_out = 16, ZS_total_out = 20, ZS_msg = 24, ZS_state = 28,
       ZS_data_type = 44, ZS_adler = 48 };

#define MAX_STREAMS 64
static struct { uint32_t guest; z_stream zs; } g_streams[MAX_STREAMS];

static z_stream *stream_of(uint32_t g, int create)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (g_streams[i].guest == g) return &g_streams[i].zs;
        if (!g_streams[i].guest && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return NULL;
    g_streams[free_slot].guest = g;
    memset(&g_streams[free_slot].zs, 0, sizeof(z_stream));
    return &g_streams[free_slot].zs;
}

static void pull(z_stream *z, uint32_t g)
{
    z->next_in   = (Bytef *)GP(rd32(g + ZS_next_in));
    z->avail_in  = rd32(g + ZS_avail_in);
    z->next_out  = (Bytef *)GP(rd32(g + ZS_next_out));
    z->avail_out = rd32(g + ZS_avail_out);
}

static void push(const z_stream *z, uint32_t g)
{
    wr32(g + ZS_next_in, H2G(z->next_in));
    wr32(g + ZS_avail_in, z->avail_in);
    wr32(g + ZS_total_in, (uint32_t)z->total_in);
    wr32(g + ZS_next_out, H2G(z->next_out));
    wr32(g + ZS_avail_out, z->avail_out);
    wr32(g + ZS_total_out, (uint32_t)z->total_out);
    wr32(g + ZS_msg, z->msg ? guest_strdup_static(z->msg) : 0);
    wr32(g + ZS_data_type, (uint32_t)z->data_type);
    wr32(g + ZS_adler, (uint32_t)z->adler);
    /* Non-zero while the stream is live: the guest tests state for null. */
    wr32(g + ZS_state, 0x5A000000u | (uint32_t)(z - &g_streams[0].zs));
}

/* inflateInit2_(strm, windowBits, version, stream_size) */
static void h_inflateInit2_(CPU *c)
{
    uint32_t g = A32(0);
    z_stream *z = stream_of(g, 1);
    if (!z) { fprintf(stderr, "[zlib] too many streams\n"); RET((uint32_t)Z_MEM_ERROR); return; }
    pull(z, g);
    int r = inflateInit2(z, AI32(1));
    push(z, g);
    RET((uint32_t)r);
}

static void h_inflateInit_(CPU *c)
{
    uint32_t g = A32(0);
    z_stream *z = stream_of(g, 1);
    if (!z) { RET((uint32_t)Z_MEM_ERROR); return; }
    pull(z, g);
    int r = inflateInit(z);
    push(z, g);
    RET((uint32_t)r);
}

static void h_inflate(CPU *c)
{
    uint32_t g = A32(0);
    z_stream *z = stream_of(g, 0);
    if (!z) { RET((uint32_t)Z_STREAM_ERROR); return; }
    pull(z, g);
    int r = inflate(z, AI32(1));
    push(z, g);
    RET((uint32_t)r);
}

static void h_inflateReset(CPU *c)
{
    uint32_t g = A32(0);
    z_stream *z = stream_of(g, 0);
    if (!z) { RET((uint32_t)Z_STREAM_ERROR); return; }
    int r = inflateReset(z);
    push(z, g);
    RET((uint32_t)r);
}

static void h_inflateEnd(CPU *c)
{
    uint32_t g = A32(0);
    z_stream *z = stream_of(g, 0);
    if (!z) { RET((uint32_t)Z_STREAM_ERROR); return; }
    int r = inflateEnd(z);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (g_streams[i].guest == g) g_streams[i].guest = 0;
    wr32(g + ZS_state, 0);
    RET((uint32_t)r);
}

void hle_register_zlib(void)
{
    hle_bind("inflateInit2_", h_inflateInit2_);
    hle_bind("inflateInit_", h_inflateInit_);
    hle_bind("inflate", h_inflate);
    hle_bind("inflateReset", h_inflateReset);
    hle_bind("inflateEnd", h_inflateEnd);
}
