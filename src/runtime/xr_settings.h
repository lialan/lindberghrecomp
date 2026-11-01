#pragma once
#ifdef __cplusplus
extern "C" {
#endif
enum { LASER_ON, LASER_DOT, LASER_OFF };
typedef struct {
    float scale,screen,aim;
    int resolution,width,refresh,left_handed,laser,auto_coins;
    int sight,crosshair,show_gun,hud_pitch,dual,show_fps;
    int foveation;              /* 0 off, 1 low, 2 medium, 3 high (Vulkan host) */
} LgXrSettings;
/* ... dual off, FPS readout on (vr.cfg "fps off" hides it), medium foveation */
#define LG_XR_SETTINGS_DEFAULT {1.0f,3.0f,15.0f,2280,2064,120,0,LASER_ON,1,0,1,1,0,0,1,2}
void xr_settings_load(LgXrSettings *settings);
void xr_settings_set(LgXrSettings *settings,const char *key,const char *value);
void xr_settings_save(const LgXrSettings *settings);
#ifdef __cplusplus
}
#endif
