#include "xr_settings.h"
#include "xr_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *const LASER_NAME[]={"laser","dot","off"};
static const char *const SIGHT_NAME[]={"none","reddot","holo"};
static const char *const FOVEATION_NAME[]={"off","low","medium","high"};
void xr_settings_save(const LgXrSettings *c)
{
    FILE *f = fopen("vr.cfg", "w");
    if (!f) return;
    fprintf(f, "scale %g\nscreen %g\naim %g\nresolution %d\nwidth %d\nrefresh %d\nhand %s\nlaser %s\ncoins %s\n"
               "sight %s\ncrosshair %s\ngun %s\nhud_pitch %d\ndual %s\nfps %s\nfoveation %s\n",
            c->scale, c->screen, c->aim, c->resolution, c->width, c->refresh, c->left_handed ? "left" : "right",
            LASER_NAME[c->laser], c->auto_coins ? "auto" : "manual",
            SIGHT_NAME[c->sight], c->crosshair ? "on" : "off", c->show_gun ? "on" : "off", c->hud_pitch, c->dual ? "on" : "off", c->show_fps ? "on" : "off",
            FOVEATION_NAME[c->foveation < 0 || c->foveation > 3 ? 2 : c->foveation]);
    fclose(f);
}

void xr_settings_load(LgXrSettings *c)
{
    /* the game's own size of a metre, until vr.cfg says otherwise: set by
     * its android_main (Let's Go Jungle's world is in about decimetres) */
    { const char *v = getenv("LINDBERGH_XR_SCALE"); if (v && atof(v) > 0) c->scale = (float)atof(v); }
    FILE *f = fopen("vr.cfg", "r");
    if (!f) f = fopen("../vr.cfg", "r");    /* beside the game: where adb can put one */
    char key[32], val[32];
    while (f && fscanf(f, "%31s %31s", key, val) == 2) xr_settings_set(c,key,val);
    if (f) fclose(f);
    /* LINDBERGH_XR_SET="key=value,...": for this run only, not saved (testing) */
    const char *e = getenv("LINDBERGH_XR_SET");
    while (e && *e) {
        if (sscanf(e, "%31[^=]=%31[^,]", key, val) == 2) xr_settings_set(c,key,val);
        e = strchr(e, ',');
        if (e) e++;
    }
    if (c->scale <= 0) c->scale = 1;
    if (c->screen <= 0.2f) c->screen = 3;
    if (c->aim < 1.0f || c->aim > 200.0f) c->aim = 15.0f;
    if (c->resolution < 512 || c->resolution > 4096) c->resolution = 2280;
    if (c->width < 0 || c->width > 4096) c->width = 0;
}

void xr_settings_set(LgXrSettings *c,const char *key,const char *val)
{
    {
        if (!strcmp(key, "scale")) c->scale = (float)atof(val);
        else if (!strcmp(key, "screen")) c->screen = (float)atof(val);
        else if (!strcmp(key, "hud_pitch")) c->hud_pitch = atoi(val);
        else if (!strcmp(key, "aim")) c->aim = (float)atof(val);
        else if (!strcmp(key, "resolution")) c->resolution = atoi(val);
        else if (!strcmp(key, "width")) c->width = atoi(val);
        else if (!strcmp(key, "refresh")) c->refresh = atoi(val);
        else if (!strcmp(key, "hand")) c->left_handed = !strcmp(val, "left");
        else if (!strcmp(key, "laser")) c->laser = !strcmp(val, "dot") ? LASER_DOT : !strcmp(val, "off") ? LASER_OFF : LASER_ON;
        else if (!strcmp(key, "coins")) c->auto_coins = strcmp(val, "manual") != 0;
        else if (!strcmp(key, "sight")) c->sight = !strcmp(val, "reddot") ? XO_SIGHT_RED_DOT : !strcmp(val, "holo") ? XO_SIGHT_HOLO : XO_SIGHT_NONE;
        else if (!strcmp(key, "crosshair")) c->crosshair = strcmp(val, "off") != 0;
        else if (!strcmp(key, "gun")) c->show_gun = strcmp(val, "off") != 0;
        else if (!strcmp(key, "dual")) c->dual = !strcmp(val, "on");
        else if (!strcmp(key, "fps")) c->show_fps = !strcmp(val, "on");
        else if (!strcmp(key, "foveation")) {
            for (int i = 0; i < 4; i++) if (!strcmp(val, FOVEATION_NAME[i])) c->foveation = i;
        }
    }
}
