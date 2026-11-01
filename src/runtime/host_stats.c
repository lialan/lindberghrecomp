/*
 * host_stats.c - see host_stats.h.
 *
 * Sources, all readable by an app (checked on the Quest 3 under its app
 * sandbox): the process's own CPU time (getrusage), its resident memory
 * (/proc/self/status), and the GPU through sysfs - Adreno's kgsl on Android
 * (/sys/class/kgsl/kgsl-3d0: gpu_busy_percentage, devfreq/cur_freq, temp),
 * else a devfreq device named for the GPU and a thermal zone whose type names
 * it (desktop Linux, e.g. the Steam Frame). System-wide CPU (/proc/stat) is
 * closed to apps on the Quest, so CPU is the game's own use. The paths are
 * found once; LINDBERGH_STATS_ROOT prefixes them (for tests).
 */
#include "host_stats.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

static volatile float g_game_ms = -1;
void lg_stats_set_game_ms(float ms) { g_game_ms = ms; }

static char g_root[256];
static char g_gpu_busy[320], g_gpu_freq[320], g_gpu_temp[320];
static int g_busy_kgsl;         /* g_gpu_busy is kgsl's "N %" (else devfreq's "N" or "N@...") */
static int g_found;

static int read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    return ok;
}

static int readable(const char *path) { char b[64]; return read_line(path, b, sizeof b); }

static void path(char *out, size_t n, const char *rest) { snprintf(out, n, "%s%s", g_root, rest); }

static void find_sources(void)
{
    const char *r = getenv("LINDBERGH_STATS_ROOT");
    snprintf(g_root, sizeof g_root, "%s", r ? r : "");
    char p[320];
    /* Adreno under Android's kgsl driver */
    path(p, sizeof p, "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
    if (readable(p)) { strcpy(g_gpu_busy, p); g_busy_kgsl = 1; }
    path(p, sizeof p, "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq");
    if (readable(p)) strcpy(g_gpu_freq, p);
    path(p, sizeof p, "/sys/class/kgsl/kgsl-3d0/temp");
    if (readable(p)) strcpy(g_gpu_temp, p);
    /* desktop Linux: a devfreq device for the GPU (msm: "*.gpu") */
    char dir[300]; path(dir, sizeof dir, "/sys/class/devfreq");
    DIR *d = (!g_gpu_freq[0] || !g_gpu_busy[0]) ? opendir(dir) : NULL;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (!strstr(e->d_name, "gpu")) continue;
        if (!g_gpu_freq[0]) { snprintf(p, sizeof p, "%s/%s/cur_freq", dir, e->d_name); if (readable(p)) strcpy(g_gpu_freq, p); }
        if (!g_gpu_busy[0]) {
            snprintf(p, sizeof p, "%s/%s/gpu_load", dir, e->d_name);
            if (!readable(p)) snprintf(p, sizeof p, "%s/%s/load", dir, e->d_name);
            if (readable(p)) strcpy(g_gpu_busy, p);
        }
    }
    if (d) closedir(d);
    /* a thermal zone that says it is the GPU's */
    path(dir, sizeof dir, "/sys/class/thermal");
    d = !g_gpu_temp[0] ? opendir(dir) : NULL;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (strncmp(e->d_name, "thermal_zone", 12)) continue;
        char type[64] = "";
        snprintf(p, sizeof p, "%s/%s/type", dir, e->d_name);
        if (!read_line(p, type, sizeof type) || !strstr(type, "gpu")) continue;
        snprintf(p, sizeof p, "%s/%s/temp", dir, e->d_name);
        if (readable(p)) { strcpy(g_gpu_temp, p); break; }
    }
    if (d) closedir(d);
    g_found = 1;
}

static long read_long(const char *p)
{
    char b[64];
    if (!p[0] || !read_line(p, b, sizeof b)) return -1;
    return strtol(b, NULL, 10);     /* "45 %", "45", "45@587000000Hz" all lead with the number */
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void lg_stats_sample(LgStats *s)
{
    static double cpu0 = -1, wall0;
    if (!g_found) find_sources();

    struct rusage ru;
    double cpu = -1, wall = now_s();
    if (getrusage(RUSAGE_SELF, &ru) == 0)
        cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    s->cpu_pct = cpu >= 0 && cpu0 >= 0 && wall > wall0 && cores > 0
                 ? (float)((cpu - cpu0) / (wall - wall0) / cores * 100.0) : -1;
    cpu0 = cpu; wall0 = wall;
    s->game_ms = g_game_ms;

    long busy = read_long(g_gpu_busy);
    s->gpu_pct = busy < 0 ? -1 : (int)(g_busy_kgsl || busy <= 100 ? busy : busy / 10);   /* devfreq load may be per mille */
    long hz = read_long(g_gpu_freq);
    s->gpu_mhz = hz <= 0 ? -1 : (int)(hz >= 1000000 ? hz / 1000000 : hz);                 /* Hz, or already MHz */
    long t = read_long(g_gpu_temp);
    s->gpu_c = t < 0 ? -1 : (int)(t >= 1000 ? (t + 500) / 1000 : t);                     /* milli-Celsius */

    s->rss_mb = -1;
    char p[320], line[128];
    path(p, sizeof p, "/proc/self/status");
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmRSS:", 6)) { s->rss_mb = (int)(strtol(line + 6, NULL, 10) / 1024); break; }
    if (f) fclose(f);
}

/* "-" where a value is unknown */
static const char *num(char *b, size_t n, const char *fmt, double v, int known)
{
    if (known) snprintf(b, n, fmt, v); else snprintf(b, n, "-");
    return b;
}

void lg_stats_format(const LgStats *s, char lines[LG_STATS_LINES][LG_STATS_LINE])
{
    char a[16], b[16], c[16], d[16];
    snprintf(lines[0], LG_STATS_LINE, "%s fps  %s ms", num(a, sizeof a, "%.0f", s->fps, s->fps >= 0),
             num(b, sizeof b, "%.1f", s->frame_ms, s->frame_ms > 0));
    snprintf(lines[1], LG_STATS_LINE, "CPU  game %s%%  main %s ms", num(a, sizeof a, "%.0f", s->cpu_pct, s->cpu_pct >= 0),
             num(b, sizeof b, "%.1f", s->game_ms, s->game_ms >= 0));
    if (s->gpu_pct < 0 && s->gpu_mhz <= 0 && s->gpu_c < 0) snprintf(lines[2], LG_STATS_LINE, "GPU  n/a");
    else snprintf(lines[2], LG_STATS_LINE, "GPU  %s%%  %s MHz  %sC", num(a, sizeof a, "%.0f", s->gpu_pct, s->gpu_pct >= 0),
                  num(b, sizeof b, "%.0f", s->gpu_mhz, s->gpu_mhz > 0), num(c, sizeof c, "%.0f", s->gpu_c, s->gpu_c >= 0));
    snprintf(lines[3], LG_STATS_LINE, "MEM %s MB  %sx%s", num(a, sizeof a, "%.0f", s->rss_mb, s->rss_mb >= 0),
             num(b, sizeof b, "%.0f", s->eye_w, s->eye_w > 0), num(d, sizeof d, "%.0f", s->eye_h, s->eye_h > 0));
}
