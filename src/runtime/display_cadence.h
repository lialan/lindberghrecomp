#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* XR owner thread publishes actual predicted display timing once per game
 * frame. The guest thread reads the matching near-60 Hz budget. No GL/Vulkan
 * dependency and no wait, allocation or file I/O on either call. */
void host_display_timing(int64_t predicted_time_ns, int64_t display_period_ns);
void host_display_timing_reset(void); /* owner thread, between sessions */
float host_frame_period_ms(void); /* zero until a compatible rate is observed */
#ifdef __cplusplus
}
#endif
