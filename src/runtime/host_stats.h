/*
 * host_stats.h - the numbers on the headset's stats panel ("Show stats"):
 * frame rate, the game's CPU use, the GPU's load, clock and temperature, and
 * memory. Read from the places an app may read on each headset; anything that
 * cannot be read stays -1 and shows as "-".
 */
#ifndef HOST_STATS_H
#define HOST_STATS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fps;            /* frames presented per second (the caller's count) */
    float frame_ms;     /* mean time between them */
    float cpu_pct;      /* the game process's CPU time, % of all online cores */
    float game_ms;      /* the game thread's CPU time per frame */
    int gpu_pct;        /* GPU busy */
    int gpu_mhz;        /* GPU clock */
    int gpu_c;          /* GPU temperature, Celsius */
    int rss_mb;         /* the process's resident memory */
    int eye_w, eye_h;   /* each eye's image (the caller's) */
} LgStats;

#define LG_STATS_LINES 4
#define LG_STATS_LINE 40

/* Fills cpu_pct, game_ms and the GPU and memory fields; CPU use is measured
 * since the previous call. fps, frame_ms and eye size are the caller's. */
void lg_stats_sample(LgStats *stats);
/* The panel's four lines. */
void lg_stats_format(const LgStats *stats, char lines[LG_STATS_LINES][LG_STATS_LINE]);
/* The game thread reports its CPU time per frame here, once a frame. */
void lg_stats_set_game_ms(float ms);

#ifdef __cplusplus
}
#endif
#endif
