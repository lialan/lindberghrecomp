#pragma once
#include <math.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { LG_STEREO_WORLD=0, LG_STEREO_HUD=1, LG_STEREO_PASS=2, LG_STEREO_FILL=3 };
/* Column-major guest-GL clip space. Vulkan's Y/depth conversion happens only
 * after the resulting clip transform in the offline vertex shader wrapper. */
typedef struct {
    const float *view;       /* game-camera -> eye, in game units */
    const float *hud_view;   /* head -> eye when head locked, else view */
    const float *tangents;   /* positive left, right, down, up */
    const float *screen_lens;
    float screen_distance;
} LgStereoProjection;
static inline int lg_stereo_is_perspective(const float p[16])
{
    return fabsf(p[11]+1)<1e-4f && fabsf(p[15])<1e-4f;
}
void lg_stereo_eye_projection(const float projection[16], int draw_class,
                             const LgStereoProjection *eye, float out[16]);
/* effective * inverse(guest), evaluated in double to retain deep scene ranges.
 * No allocation or GPU work; cache by guest projection and eye state. A
 * singular guest matrix is rejected without modifying the caller's output. */
int lg_stereo_clip_reprojection(const float guest[16], const float effective[16], float out[16]);
#ifdef __cplusplus
}
#endif
