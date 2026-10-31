/* ALSA's playback PCM seam. Guest sound libraries retain their decoder,
 * mixer and refill callbacks; SDL consumes their interleaved PCM in real
 * time. Availability is queue space, including when output is muted. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "lindbergh_rt.h"
#include "host_settings.h"

enum { PCM_TOKEN = 0x50430000, PCM_COUNT = 8, PARAM_BYTES = 32 };
typedef struct {
    int used, state;
    uint32_t rate, channels, format, capacity;
    SDL_AudioDeviceID device;
    uint32_t meter_frames;
    int peak;
} Pcm;
static Pcm g_pcm[PCM_COUNT];

static Pcm *pcm(uint32_t token)
{
    unsigned i = token - PCM_TOKEN;
    return i < PCM_COUNT && g_pcm[i].used ? &g_pcm[i] : NULL;
}

static void h_open(CPU *c)
{
    if (!A32(0) || A32(2) != 0) { RET(-22); return; } /* playback only */
    for (unsigned i = 0; i < PCM_COUNT; i++) if (!g_pcm[i].used) {
        g_pcm[i] = (Pcm){ .used = 1, .rate = 48000, .channels = 2,
                         .format = 2, .capacity = 4096 };
        wr32(A32(0), PCM_TOKEN + i);
        RET(0); return;
    }
    RET(-24);
}
static void h_close(CPU *c)
{
    Pcm *p = pcm(A32(0));
    if (!p) { RET(-77); return; }
    if (p->device) SDL_CloseAudioDevice(p->device);
    memset(p, 0, sizeof *p);
    RET(0);
}
static void h_params_malloc(CPU *c)
{
    if (!A32(0)) { RET(-22); return; }
    void *p = guest_alloc(PARAM_BYTES, 16);
    if (!p) { RET(-12); return; }
    memset(p, 0, PARAM_BYTES);
    wr32(A32(0), H2G(p)); RET(0);
}
static void h_params_free(CPU *c) { if (A32(0)) guest_free(APTR(0)); RET(0); }
static void h_hw_any(CPU *c)
{
    Pcm *p = pcm(A32(0));
    if (!p || !A32(1)) { RET(-22); return; }
    wr32(A32(1), p->rate); wr32(A32(1) + 4, p->channels);
    wr32(A32(1) + 8, p->format); wr32(A32(1) + 12, p->capacity);
    RET(0);
}
static void h_format(CPU *c)
{
    if (!pcm(A32(0)) || !A32(1) || A32(2) != 2) { RET(-22); return; }
    wr32(A32(1) + 8, A32(2)); RET(0); /* SND_PCM_FORMAT_S16_LE */
}
static void h_channels(CPU *c)
{
    if (!pcm(A32(0)) || !A32(1) || A32(2) < 1 || A32(2) > 8) { RET(-22); return; }
    wr32(A32(1) + 4, A32(2)); RET(0);
}
static void h_rate(CPU *c)
{
    if (!pcm(A32(0)) || !A32(1) || !A32(2)) { RET(-22); return; }
    uint32_t rate = rd32(A32(2));
    if (rate < 8000 || rate > 192000) { RET(-22); return; }
    wr32(A32(1), rate); if (A32(3)) wr32(A32(3), 0); RET(0);
}
static void h_capacity(CPU *c)
{
    if (!pcm(A32(0)) || !A32(1) || A32(2) < 512 || A32(2) > 262144) { RET(-22); return; }
    wr32(A32(1) + 12, A32(2)); RET(0);
}
static void h_access(CPU *c) { RET(pcm(A32(0)) && A32(2) == 3 ? 0 : -22); } /* RW_INTERLEAVED */
static void h_width(CPU *c) { RET(A32(0) == 2 ? 16 : -22); }
static void h_ok(CPU *c) { RET(pcm(A32(0)) ? 0 : -77); }
static void h_hw_apply(CPU *c)
{
    Pcm *p = pcm(A32(0));
    if (!p || !A32(1)) { RET(-22); return; }
    p->rate = rd32(A32(1)); p->channels = rd32(A32(1) + 4);
    p->format = rd32(A32(1) + 8); p->capacity = rd32(A32(1) + 12);
    if (p->format != 2 || !p->channels || p->channels > 8 ||
        p->rate < 8000 || p->rate > 192000 || p->capacity < 512 || p->capacity > 262144) { RET(-22); return; }
    if (p->device) SDL_CloseAudioDevice(p->device);
    p->device = 0;
    SDL_AudioSpec want = {0};
    want.freq = (int)p->rate; want.format = AUDIO_S16LSB;
    want.channels = (Uint8)p->channels; want.samples = 512;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0 ||
        !(p->device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0))) {
        fprintf(stderr, "[alsa] cannot open PCM: %s\n", SDL_GetError()); RET(-5); return;
    }
    p->state = 2; /* PREPARED */
    fprintf(stderr, "[alsa] PCM %u Hz, %u channels, S16_LE, %u frames\n", p->rate, p->channels, p->capacity);
    RET(0);
}
static void h_state(CPU *c) { Pcm *p = pcm(A32(0)); RET(p ? p->state : -77); }
static void h_start(CPU *c)
{
    Pcm *p = pcm(A32(0));
    if (!p || !p->device) { RET(-77); return; }
    p->state = 3; SDL_PauseAudioDevice(p->device, 0); RET(0);
}
static void h_drop(CPU *c)
{
    Pcm *p = pcm(A32(0));
    if (!p || !p->device) { RET(-77); return; }
    SDL_PauseAudioDevice(p->device, 1); SDL_ClearQueuedAudio(p->device);
    p->state = 1; RET(0);
}
static void h_prepare(CPU *c)
{
    Pcm *p = pcm(A32(0));
    if (!p || !p->device) { RET(-77); return; }
    SDL_PauseAudioDevice(p->device, 1);
    SDL_ClearQueuedAudio(p->device); p->state = 2; RET(0);
}
static uint32_t available(Pcm *p)
{
    uint32_t queued = SDL_GetQueuedAudioSize(p->device) / (p->channels * 2);
    return queued < p->capacity ? p->capacity - queued : 0;
}
static void h_avail(CPU *c)
{
    Pcm *p = pcm(A32(0)); RET(p && p->device ? (int32_t)available(p) : -77);
}
static void h_write(CPU *c)
{
    Pcm *p = pcm(A32(0));
    if (!p || !p->device) { RET(-77); return; }
    uint32_t n = A32(2), avail = available(p);
    if (!n) { RET(0); return; }
    if (n > avail) n = avail;
    if (!n) { RET(-11); return; }
    if (!A32(1)) { RET(-22); return; }
    uint32_t bytes = n * p->channels * 2;
    int16_t *out = malloc(bytes);
    if (!out) { RET(-12); return; }
    HostSettings *st = settings();
    int volume = st->muted ? 0 : st->volume;
    const int16_t *in = APTR(1);
    static int meter = -1;
    if (meter < 0) { const char *v = getenv("LINDBERGH_ALSA_METER"); meter = v && *v && *v != '0'; }
    for (uint32_t i = 0; i < n * p->channels; i++) {
        out[i] = (int16_t)(in[i] * volume / 100);
        if (meter) { int peak = in[i] < 0 ? -in[i] : in[i]; if (peak > p->peak) p->peak = peak; }
    }
    if (meter && (p->meter_frames += n) >= 2 * p->rate) {
        fprintf(stderr, "[alsa] source peak %.4f, output %s\n", p->peak / 32768.0, volume ? "on" : "muted");
        p->meter_frames = 0; p->peak = 0;
    }
    int ok = SDL_QueueAudio(p->device, out, bytes) == 0;
    free(out); RET(ok ? (int32_t)n : -5);
}
static void h_error(CPU *c)
{
    const char *s = strerror(AI32(0) < 0 ? -AI32(0) : AI32(0));
    static uint32_t buffer;
    if (!buffer) buffer = H2G(guest_alloc(256, 16));
    if (buffer) snprintf(GP(buffer), 256, "%s", s);
    RET(buffer);
}
void hle_register_alsa(void)
{
#define B(n, f) hle_bind("snd_" n, f)
    B("pcm_open", h_open); B("pcm_close", h_close);
    B("pcm_hw_params_malloc", h_params_malloc); B("pcm_hw_params_free", h_params_free);
    B("pcm_sw_params_malloc", h_params_malloc); B("pcm_sw_params_free", h_params_free);
    B("pcm_hw_params_any", h_hw_any); B("pcm_hw_params_set_format", h_format);
    B("pcm_hw_params_set_channels", h_channels); B("pcm_hw_params_set_rate_near", h_rate);
    B("pcm_hw_params_set_buffer_size", h_capacity); B("pcm_hw_params_set_access", h_access);
    B("pcm_hw_params_set_periods", h_ok); B("pcm_hw_params", h_hw_apply);
    B("pcm_sw_params_current", h_ok); B("pcm_sw_params_set_avail_min", h_ok);
    B("pcm_sw_params_set_start_threshold", h_ok); B("pcm_sw_params_set_xfer_align", h_ok);
    B("pcm_sw_params", h_ok); B("pcm_format_width", h_width);
    B("pcm_state", h_state); B("pcm_start", h_start); B("pcm_resume", h_start);
    B("pcm_drop", h_drop); B("pcm_prepare", h_prepare);
    B("pcm_avail_update", h_avail); B("pcm_writei", h_write); B("strerror", h_error);
#undef B
}
