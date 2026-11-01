#include "display_cadence.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Publisher: XR frame owner. Consumer: guest pacing. A kick is consumed once,
 * without a renderer/guest data race when the shared render worker is active. */
static _Atomic int64_t display_period;
static atomic_int phase_kick;
static int64_t previous;
static unsigned histogram[5], samples, last_odd, frame;

void host_display_timing_reset(void)
{
    atomic_store_explicit(&display_period, 0, memory_order_relaxed);
    atomic_store_explicit(&phase_kick, 0, memory_order_relaxed);
    previous = 0;
    samples = last_odd = frame = 0;
    memset(histogram, 0, sizeof histogram);
}

void host_display_timing(int64_t time, int64_t period)
{
    if (period <= 0 || time <= 0) return;
    int64_t old = atomic_load_explicit(&display_period, memory_order_relaxed);
    if (!old || fabs((double)old - period) > period * 0.001 ||
        (previous && time <= previous)) {
        /* A refresh change starts a new phase history. The observed period,
         * rather than the requested refresh, controls the guest budget. */
        host_display_timing_reset();
    }
    atomic_store_explicit(&display_period, period, memory_order_relaxed);
    if (previous && time > previous) {
        int64_t k = (time - previous + period / 2) / period;
        histogram[k < 1 ? 0 : k > 4 ? 4 : k]++;
        ++frame;
        long want = (long)(1e9 / 60.0 / (double)period + 0.5);
        if (k != want) {
            /* Two odd display-frame spans within half a second indicate
             * edge-phase jitter. One half-display-period delay moves the
             * game off that edge, preserving the established GL policy. */
            if (last_odd && frame - last_odd < 30) {
                atomic_store_explicit(&phase_kick, 1, memory_order_relaxed);
                last_odd = 0;
            } else last_odd = frame;
        }
        if (++samples == 300) {
            fprintf(stderr, "[pace] display frames per game frame: 1: %u, 2: %u, 3: %u, 4+: %u\n",
                    histogram[1] + histogram[0], histogram[2], histogram[3], histogram[4]);
            memset(histogram, 0, sizeof histogram);
            samples = 0;
        }
    }
    previous = time;
}

float host_frame_period_ms(void)
{
    int64_t period = atomic_load_explicit(&display_period, memory_order_relaxed);
    if (period <= 0) return 0;
    double per = 1e9 / 60.0 / (double)period;
    long k = (long)(per + 0.5);
    if (k < 1 || fabs(per - k) >= 0.05) return 0;
    double ms = k * period / 1e6;
    if (atomic_exchange_explicit(&phase_kick, 0, memory_order_relaxed)) ms += period / 2e6;
    return (float)ms;
}
