/*
 * game_profile.c - a game's settings, applied once at start (game_profile.h).
 *
 * Settings the runtime reads from the environment are given as defaults
 * there: a LINDBERGH_* variable already set (env.txt, a test run) wins.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_profile.h"
#include "lindbergh_rt.h"
#include "stereo.h"

static LgGameProfile g_profile;

const LgGameProfile *game_profile(void) { return &g_profile; }

static void env_default(const char *name, const char *value)
{
    const char *v = getenv(name);
    if (!v || !*v) setenv(name, value, 1);
}

void game_profile_apply(const LgGameProfile *p)
{
    g_profile = *p;
    if (p->title) host_window_title(p->title);
    int w = p->width, h = p->height;
#ifdef __ANDROID__
    if (p->vr.width) { w = p->vr.width; h = p->vr.height; }
#endif
    if (w > 0 && h > 0) host_window_hint(w, h);
    if (p->pace_fps > 0) host_pace_fps(p->pace_fps);
    if (p->sound_gain > 0) segaapi_master_gain(p->sound_gain);
    if (p->aim[0] || p->aim[1] || p->aim[2] || p->aim[3]) host_aim_default(p->aim[0], p->aim[1], p->aim[2], p->aim[3]);
    if (p->gun_map) host_gun_map(p->gun_map);
    if (p->ride_board) rideboard_enable(p->ride_board);
    if (p->sysdata_region) sega_sysdata_region(1);
    if (p->reload_switch[1]) jvs_reload_switch(p->reload_switch[0], p->reload_switch[1]);
#ifdef __ANDROID__
    if (p->vr.title) env_default("LINDBERGH_XR_TITLE", p->vr.title);
    if (p->vr.scale > 0) {
        char s[32];
        snprintf(s, sizeof s, "%g", p->vr.scale);
        env_default("LINDBERGH_XR_SCALE", s);
    }
    if (p->vr.world_w > 0) stereo_picture_size(p->vr.world_w, p->vr.world_h);
    if (p->vr.camera_in_constants) stereo_env_projection(1);
    stereo_depth_read(p->vr.depth_read);
    if (p->vr.face_swings) env_default("LINDBERGH_XR_FACE_SWINGS", "1");
    if (p->vr.level_horizon) {
        env_default("LINDBERGH_XR_LEVEL", "1");
        if (*getenv("LINDBERGH_XR_LEVEL") != '0') stereo_level_horizon(1);
    }
#else
    if (p->desktop_wide) {
        env_default("LINDBERGH_WIDE", "1");
        const char *wide = getenv("LINDBERGH_WIDE");
        if (p->cull_fov > 0 && *wide != '0') {
            char s[32];
            snprintf(s, sizeof s, "%g", p->cull_fov);
            env_default("LINDBERGH_CULL_FOV", s);
        }
    }
#endif
}
