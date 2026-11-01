#ifndef LINDBERGH_FRAME_TIMING_H
#define LINDBERGH_FRAME_TIMING_H
#include <stdint.h>
/* Initialize on the game thread before starting the render worker. Records
 * CPU submission boundaries, not GPU completion or compositor presentation.
 * Disabled by default; enabled collection allocates once and writes at exit. */
#ifdef __cplusplus
extern "C" {
#endif
extern int frame_timing_on;
void frame_timing_init(const char *backend);
void frame_timing_record(unsigned frame, uint64_t ticks, uint64_t frequency);
#ifdef __cplusplus
}
#endif
#endif
