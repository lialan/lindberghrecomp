#pragma once
#include <math.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { LG_STEREO_WORLD=0, LG_STEREO_HUD=1, LG_STEREO_PASS=2, LG_STEREO_FILL=3 };
typedef struct {
    int perspective,hud_target,menu,zoom;
    int texture_view,picture_target,covers_viewport;
    int offscreen,flat,multiview,fills_screen;
} LgStereoDrawFacts;
typedef struct {int game_w,game_h,world_w,world_h,eye_w,eye_h;} LgStereoTargetSize;
void lg_stereo_target_scale(const LgStereoTargetSize *sizes,int width,int height,int rectangle,float *x,float *y);
int lg_stereo_classify_draw(const LgStereoDrawFacts *draw);
int lg_stereo_ray_to_plane(const float origin[3],const float direction[3],const float lens[2],
                           float distance,int width,int height,float *x,float *y,float *reach);
void lg_stereo_rotate(const float quaternion[4],const float point[3],float out[3]);
void lg_stereo_pose_view(const float quaternion[4],const float position[3],float scale,float out[16]);
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
