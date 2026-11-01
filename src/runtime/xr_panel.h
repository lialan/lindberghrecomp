#pragma once
#include <stdint.h>
#include "xr_settings.h"
#include "xr_menu.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { XP_SIGHT,XP_LASER,XP_CROSSHAIR,XP_GUN,XP_HAND,XP_DUAL,XP_FPS,XP_SCREEN,XP_HUD,XP_RATE,XP_RES,XP_COINS,XP_COIN,XP_RECENTRE,XP_TEST,XP_SERVICE,XP_RESET,XP_CLOSE,XP_COUNT };
enum { XP_SAVE=1,XP_CROSSHAIR_CHANGED=2,XP_SCREEN_CHANGED=4,XP_HUD_CHANGED=8,XP_RATE_CHANGED=16,XP_RECENTER=32,XP_RESTART=64,XP_HAND_CHANGED=128 };
typedef struct {
    int open,hover,dirty;float c[3],right[3],up[3],n[3],hw,hh;
    int test_frames,service_frames,coin,reset_armed;
} XpPanel;
void xp_items(const XpPanel *panel,const LgXrSettings *settings,int hand,int hud_locked,int running_resolution,int running_width,XmItem *items);
unsigned xp_choose(XpPanel *panel,LgXrSettings *settings,int *hand,int item,const float *rates,int nrates);
void xp_place(XpPanel *panel,const float head[3],const float orientation[4]);
int xp_hit(const XpPanel *panel,const float origin[3],const float direction[3],float *u,float *v,float hit[3]);
void xp_matrix(const XpPanel *panel,float model[16]);
int xp_menu_button(XpPanel *panel,int pressed,int previous);
int xp_hold_button(XpPanel *panel,int pressed,int previous,int dual,int64_t time,int64_t *held_since);
#ifdef __cplusplus
}
#endif
