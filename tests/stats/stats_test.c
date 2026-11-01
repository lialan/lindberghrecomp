/* host_stats.c against a fake /sys and /proc: Adreno's kgsl (Quest) and a
 * devfreq + thermal-zone GPU (desktop Linux), and the panel's formatting.
 * Sources are found once per process, so each layout runs in a child. */
#include "host_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
static void put(const char *root, const char *rel, const char *text)
{
    char p[512], dir[512];
    snprintf(p, sizeof p, "%s%s", root, rel);
    snprintf(dir, sizeof dir, "%s", p);
    for (char *s = dir + strlen(root) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(dir, 0755); *s = '/'; }
    FILE *f = fopen(p, "w"); fputs(text, f); fclose(f);
}
static void expect(const char *what, const char *got, const char *want)
{
    int ok = !strcmp(got, want); fails += !ok;
    printf("%s %-8s \"%s\"", ok ? "ok  " : "FAIL", what, got);
    if (!ok) printf(" (want \"%s\")", want);
    printf("\n");
}
/* one layout, in a child process */
static void run(const char *name, const char *root, const char *gpu)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        fails = 0;
        printf("-- %s\n", name);
        setenv("LINDBERGH_STATS_ROOT", root, 1);
        lg_stats_set_game_ms(7.94f);
        LgStats s = { .fps = 60, .frame_ms = 16.6f, .eye_w = 2064, .eye_h = 2208 };
        lg_stats_sample(&s);
        for (volatile long i = 0; i < 20000000; i++) {}    /* some CPU time to measure */
        lg_stats_sample(&s);                               /* CPU % needs two samples */
        char lines[LG_STATS_LINES][LG_STATS_LINE];
        lg_stats_format(&s, lines);
        expect("fps", lines[0], "60 fps  16.6 ms");
        char cpu[LG_STATS_LINE]; snprintf(cpu, sizeof cpu, "%.10s", lines[1]);
        expect("cpu", cpu, "CPU  game ");
        expect("main", strstr(lines[1], "main") ? strstr(lines[1], "main") : lines[1], "main 7.9 ms");
        expect("gpu", lines[2], gpu);
        expect("mem", lines[3], "MEM 412 MB  2064x2208");
        if (s.cpu_pct <= 0 || s.cpu_pct > 100) { fails++; printf("FAIL cpu %% %.1f out of range\n", s.cpu_pct); }
        fflush(stdout);
        _exit(fails != 0);
    }
    int status = 0; waitpid(pid, &status, 0);
    fails += !WIFEXITED(status) || WEXITSTATUS(status);
}

int main(void)
{
    char quest[] = "/tmp/lgstats-quest.XXXXXX", frame[] = "/tmp/lgstats-frame.XXXXXX", none[] = "/tmp/lgstats-none.XXXXXX";
    if (!mkdtemp(quest) || !mkdtemp(frame) || !mkdtemp(none)) return 1;
    put(quest, "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", "45 %\n");
    put(quest, "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "587000000\n");
    put(quest, "/sys/class/kgsl/kgsl-3d0/temp", "52042\n");
    put(quest, "/proc/self/status", "Name:\thotd4\nVmPeak:\t 900000 kB\nVmRSS:\t  421888 kB\n");
    run("Quest (kgsl)", quest, "GPU  45%  587 MHz  52C");

    put(frame, "/sys/class/devfreq/3d00000.gpu/cur_freq", "680000000\n");
    put(frame, "/sys/class/devfreq/3d00000.gpu/gpu_load", "71\n");
    put(frame, "/sys/class/thermal/thermal_zone0/type", "cpu0-thermal\n");
    put(frame, "/sys/class/thermal/thermal_zone0/temp", "40000\n");
    put(frame, "/sys/class/thermal/thermal_zone7/type", "gpuss-0\n");
    put(frame, "/sys/class/thermal/thermal_zone7/temp", "61500\n");
    put(frame, "/proc/self/status", "VmRSS:\t  421888 kB\n");
    run("desktop Linux (devfreq, thermal zone)", frame, "GPU  71%  680 MHz  62C");

    put(none, "/proc/self/status", "VmRSS:\t  421888 kB\n");
    run("no GPU sources", none, "GPU  n/a");

    printf(fails ? "stats test FAILED\n" : "stats test passed\n");
    return fails != 0;
}
