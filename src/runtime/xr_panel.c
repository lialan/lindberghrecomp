#include "xr_panel.h"
#include "stereo_projection.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void xp_items(const XpPanel *menu,const LgXrSettings *cfg,int hand,int hud_locked,int running_resolution,int running_width,XmItem *it)
{
    static const char *const sight[] = { "Point shooting", "Red dot", "Holographic" };
    static const char *const laser[] = { "Laser and dot", "Dot only", "Off" };
    static const char *const labels[XP_COUNT] = {
        "Sight", "Laser", "Game crosshair", "Gun model", "Gun hand", "Dual wield", "Show FPS", "Screen distance", "HUD position", "Display rate", "Resolution per eye",
        "Foveation", "Coins", "Insert a coin", "Recentre view", "Arcade Test button (select)",
        "Arcade Service button (next)", "Reset game", "Close" };
    for (int i = 0; i < XP_COUNT; i++) { it[i].label = labels[i]; it[i].value[0] = 0; }
    snprintf(it[XP_SIGHT].value, 40, "%s", sight[cfg->sight]);
    snprintf(it[XP_LASER].value, 40, "%s", laser[cfg->laser]);
    snprintf(it[XP_CROSSHAIR].value, 40, "%s", cfg->crosshair ? "Shown" : "Hidden");
    snprintf(it[XP_GUN].value, 40, "%s", cfg->show_gun ? "Shown" : "Hidden");
    snprintf(it[XP_HAND].value, 40, "%s", hand == 0 ? "Left" : "Right");
    if (cfg->dual) it[XP_HAND].label = "Player 1 hand";    /* the other is player 2's */
    snprintf(it[XP_DUAL].value, 40, "%s", !cfg->dual ? "Off" : hand == 0 ? "On (right hand P2)" : "On (left hand P2)");
    snprintf(it[XP_FPS].value, 40, "%s", cfg->show_fps ? "On" : "Off");
    snprintf(it[XP_SCREEN].value, 40, "%.1f m", cfg->screen);
    if (!hud_locked) snprintf(it[XP_HUD].value, 40, "With the screen");
    else if (cfg->hud_pitch == 0) snprintf(it[XP_HUD].value, 40, "Centre");
    else snprintf(it[XP_HUD].value, 40, "%s %d deg", cfg->hud_pitch < 0 ? "Down" : "Up", abs(cfg->hud_pitch));
    snprintf(it[XP_RATE].value, 40, "%d Hz", cfg->refresh);
    char res[24];
    if (cfg->width) snprintf(res, sizeof res, "%d x %d", cfg->width, cfg->resolution);
    else snprintf(res, sizeof res, "%d", cfg->resolution);
    if (cfg->resolution == running_resolution && cfg->width == running_width) snprintf(it[XP_RES].value, 40, "%s", res);
    else snprintf(it[XP_RES].value, 40, "%s (on restart)", res);
    static const char *const foveation[] = { "Off", "Low", "Medium", "High" };
    snprintf(it[XP_FOVEATION].value, 40, "%s", foveation[cfg->foveation < 0 || cfg->foveation > 3 ? 2 : cfg->foveation]);
    snprintf(it[XP_COINS].value, 40, "%s", cfg->auto_coins ? "Automatic" : "By menu");
    if (menu->reset_armed) snprintf(it[XP_RESET].value, 40, "Pull again to reset");
}


unsigned xp_choose(XpPanel *p,LgXrSettings *cfg,int *hand,int item,const float *rates,int nrates)
{
    if(item<0||item>=XP_COUNT)return 0;
    static const float screens[]={2,2.5f,3,4,5};const int armed=p->reset_armed;
    unsigned effects=XP_SAVE;p->reset_armed=0;
    switch(item) {
    case XP_SIGHT:cfg->sight=(cfg->sight+1)%3;break;
    case XP_LASER:cfg->laser=(cfg->laser+1)%3;break;
    case XP_CROSSHAIR:cfg->crosshair=!cfg->crosshair;effects|=XP_CROSSHAIR_CHANGED;break;
    case XP_GUN:cfg->show_gun=!cfg->show_gun;break;
    case XP_HAND:*hand=1-*hand;cfg->left_handed=*hand==0;effects|=XP_HAND_CHANGED;break;
    case XP_DUAL:cfg->dual=!cfg->dual;break;
    case XP_FPS:cfg->show_fps=!cfg->show_fps;break;
    case XP_SCREEN:{int k=0;while(k<5&&screens[k]<=cfg->screen+.01f)++k;cfg->screen=screens[k%5];effects|=XP_SCREEN_CHANGED;break;}
    case XP_HUD:{static const int pitches[]={0,-10,-20,-30,10};int k=0;while(k<5&&pitches[k]!=cfg->hud_pitch)++k;cfg->hud_pitch=pitches[(k+1)%5];effects|=XP_HUD_CHANGED;break;}
    case XP_RATE:if(nrates){int k=0;while(k<nrates&&rates[k]<=cfg->refresh+.5f)++k;cfg->refresh=(int)(rates[k%nrates]+.5f);effects|=XP_RATE_CHANGED;}break;
    case XP_RES:{static const int sizes[][2]={{0,1216},{0,1440},{0,1680},{0,1920},{0,2208},{2064,2280}};int k=0;
        while(k<6&&(sizes[k][1]<cfg->resolution||(sizes[k][1]==cfg->resolution&&sizes[k][0]<=cfg->width)))++k;
        cfg->width=sizes[k%6][0];cfg->resolution=sizes[k%6][1];break;}
    case XP_FOVEATION:cfg->foveation=(cfg->foveation+1)%4;effects|=XP_FOVEATION_CHANGED;break;
    case XP_COINS:cfg->auto_coins=!cfg->auto_coins;break;
    case XP_COIN:p->coin=1;break;
    case XP_RECENTRE:effects|=XP_RECENTER;break;
    case XP_TEST:p->test_frames=12;break;
    case XP_SERVICE:p->service_frames=12;break;
    case XP_RESET:if(!armed)p->reset_armed=1;else effects|=XP_RESTART;break;
    case XP_CLOSE:p->open=0;break;
    default:return 0;
    }
    p->dirty=1;return effects;
}
void xp_place(XpPanel *p,const float head[3],const float orientation[4])
{
    const float back[]={0,0,-1};float f[3];lg_stereo_rotate(orientation,back,f);
    const float yaw=atan2f(-f[0],-f[2])+.55f,fwd[]={-sinf(yaw),0,-cosf(yaw)};
    for(int i=0;i<3;++i){p->c[i]=head[i]+fwd[i]*.95f;p->n[i]=-fwd[i];}
    p->c[1]=head[1]-.12f;p->up[0]=p->up[2]=0;p->up[1]=1;
    p->right[0]=p->up[1]*p->n[2]-p->up[2]*p->n[1];p->right[1]=p->up[2]*p->n[0]-p->up[0]*p->n[2];p->right[2]=p->up[0]*p->n[1]-p->up[1]*p->n[0];
    p->hw=.33f;p->hh=p->hw*XM_H/XM_W;
}
int xp_hit(const XpPanel *p,const float origin[3],const float direction[3],float *u,float *v,float hit[3])
{
    const float dn=direction[0]*p->n[0]+direction[1]*p->n[1]+direction[2]*p->n[2];if(dn>-1e-4f)return 0;
    const float t=((p->c[0]-origin[0])*p->n[0]+(p->c[1]-origin[1])*p->n[1]+(p->c[2]-origin[2])*p->n[2])/dn;if(t<=0)return 0;
    float r[3];for(int i=0;i<3;++i){hit[i]=origin[i]+direction[i]*t;r[i]=hit[i]-p->c[i];}
    const float x=(r[0]*p->right[0]+r[1]*p->right[1]+r[2]*p->right[2])/p->hw,y=(r[0]*p->up[0]+r[1]*p->up[1]+r[2]*p->up[2])/p->hh;
    *u=(x+1)*.5f;*v=(1-y)*.5f;return x>=-1&&x<=1&&y>=-1&&y<=1;
}
void xp_matrix(const XpPanel *p,float model[16])
{
    memset(model,0,64);for(int i=0;i<3;++i){model[i]=p->right[i]*p->hw;model[4+i]=p->up[i]*p->hh;model[8+i]=p->n[i];model[12+i]=p->c[i];}model[15]=1;
}
static void toggle(XpPanel *p){p->open=!p->open;p->reset_armed=0;if(p->open){p->hover=-1;p->dirty=1;}}
int xp_menu_button(XpPanel *p,int pressed,int previous){if(!pressed||previous)return 0;toggle(p);return 1;}
int xp_hold_button(XpPanel *p,int pressed,int previous,int dual,int64_t time,int64_t *held)
{
    if(!dual&&pressed&&!previous)*held=time;
    if(!dual&&pressed&&*held&&time-*held>=1000000000){*held=0;toggle(p);return 1;}return 0;
}
