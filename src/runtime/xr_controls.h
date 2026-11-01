#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Shared cabinet policy. Button arrays use left/right hands; aimed/start
 * arrays use player 1/player 2. Times are OpenXR nanoseconds. */
typedef struct {
    int gun_hand,prev_trigger[2],prev_grip[2],prev_start[2];
    int64_t grip_at[2],joined_at;
    unsigned credit_frames;
} LgXrControls;
#define LG_XR_CONTROLS_DEFAULT {1,{0,0},{0,0},{0,0},{0,0},0,0}
typedef struct {
    int64_t time;
    int dual,auto_coins,blocked;
    int trigger[2],grip[2],lower[2],upper[2],stick[2];
    int aimed[2],start_override[2];
    /* GL evaluates recoil/firing before panel callbacks can toggle it. */
    int firing[2],fire_supplied;
} LgXrControlFrame;
typedef struct { unsigned held;int coins; } LgXrCabinet;
int lg_xr_controls_select(LgXrControls *state,const int trigger[2],int dual);
LgXrCabinet lg_xr_controls_update(LgXrControls *state,const LgXrControlFrame *frame);
#ifdef __cplusplus
}
#endif
