#pragma once
#include <openxr/openxr.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { LG_XR_AIM,LG_XR_TRIGGER,LG_XR_GRIP,LG_XR_LOWER,LG_XR_UPPER,LG_XR_STICK,LG_XR_MENU,LG_XR_HAPTIC,LG_XR_ACTION_COUNT };
typedef struct {
    XrInstance instance;XrSession session;XrActionSet set;
    XrAction action[LG_XR_ACTION_COUNT];XrSpace aim_space[2];XrPath hand_path[2];
} LgXrActions;
typedef struct {
    XrPosef pose[2];int valid[2];float trigger[2],grip[2];
    int lower[2],upper[2],stick[2],menu;
} LgXrActionSample;
XrResult lg_xr_actions_create(LgXrActions *actions,XrInstance instance,XrSession session);
void lg_xr_actions_destroy(LgXrActions *actions);
XrResult lg_xr_actions_sync(const LgXrActions *actions);
float lg_xr_action_float(const LgXrActions *actions,XrAction action,int hand);
int lg_xr_action_bool(const LgXrActions *actions,XrAction action,int hand);
XrResult lg_xr_actions_sample(const LgXrActions *actions,XrSpace space,XrTime time,LgXrActionSample *sample);
XrResult lg_xr_actions_rumble(const LgXrActions *actions,int hand,float amplitude,int milliseconds);
#ifdef __cplusplus
}
#endif
