#include "xr_controls.h"
#include "cabinet_buttons.h"
int lg_xr_controls_select(LgXrControls *s,const int trigger[2],int dual)
{
    for(int h=0;h<2;++h)if(!dual&&trigger[h]&&!s->prev_trigger[h]&&h!=s->gun_hand)s->gun_hand=h;
    return s->gun_hand;
}
/* A thumbstick as the 8-way lever: past a dead zone, an axis counts once it
 * is over tan(22.5 deg) of the other, so each of the 8 sectors is 45 deg. */
static unsigned lever(float x,float y)
{
    unsigned b=0;const float ax=x<0?-x:x,ay=y<0?-y:y;
    if(x*x+y*y<.35f*.35f)return 0;
    if(ax>ay*.414f)b|=x<0?CAB_P1_LEFT:CAB_P1_RIGHT;
    if(ay>ax*.414f)b|=y>0?CAB_P1_UP:CAB_P1_DOWN;
    return b;
}
/* Virtua Tennis 3 in the headset: either thumbstick is the lever; the right
 * trigger is button 1 (topspin), A button 2 (lob), B button 3 (slice); the
 * left trigger and X double as buttons 2 and 3 for the other hand. */
static unsigned lever_buttons(const LgXrControlFrame *f)
{
    if(f->blocked)return 0;
    unsigned b=lever(f->thumb_x[0],f->thumb_y[0])|lever(f->thumb_x[1],f->thumb_y[1]);
    if(f->trigger[1])b|=CAB_P1_TRIGGER;
    if(f->lower[1]||f->trigger[0])b|=CAB_P1_BUTTON2;
    if(f->upper[1]||f->lower[0])b|=CAB_P1_BUTTON3;
    return b;
}
LgXrCabinet lg_xr_controls_update(LgXrControls *s,const LgXrControlFrame *f)
{
    LgXrCabinet out={0,0};const int g=s->gun_hand,off=1-g;
    if(f->joystick)out.held|=lever_buttons(f);
    else for(int pl=0;pl<(f->dual?2:1);++pl) {
        const int h=pl?off:g;unsigned b=0;
        if(f->fire_supplied?f->firing[pl]:(f->trigger[h]&&f->aimed[pl]&&!f->blocked))b|=CAB_P1_TRIGGER;
        if(f->trigger[h]&&!f->aimed[pl]&&!f->blocked)b|=CAB_P1_RELOAD;
        if(f->grip[h]&&!s->prev_grip[h])s->grip_at[h]=f->time;
        if(f->grip[h]&&!f->blocked&&f->time-s->grip_at[h]<120000000)b|=CAB_P1_RELOAD;
        if(f->lower[h]&&!f->blocked)b|=CAB_P1_ACTION;
        if(f->upper[h]&&!f->blocked)b|=CAB_P1_FIREMODE;
        out.held|=pl?b<<CAB_P2_SHIFT:b;
    }
    int start=(f->dual?f->stick[g]:f->stick[0]||f->stick[1])&&!f->blocked;
    int start2=f->dual&&f->stick[off]&&!f->blocked;
    if(f->dual&&f->trigger[off]&&!s->prev_trigger[off]&&!f->blocked&&
       (!s->joined_at||f->time-s->joined_at>3000000000LL)) {
        s->joined_at=f->time;out.coins+=f->auto_coins;
    }
    const int join_start=f->dual&&s->joined_at&&f->time-s->joined_at>=150000000&&f->time-s->joined_at<450000000;
    if(f->start_override[0])start=1;
    if(f->start_override[1]&&f->dual)start2=1;
    if(start)out.held|=CAB_P1_START;
    if(start2)out.held|=CAB_P2_START;
    if(f->auto_coins) {
        if(++s->credit_frames==240)++out.coins;
        if(start&&!s->prev_start[0])++out.coins;
        if(start2&&!s->prev_start[1])++out.coins;
    }
    if(join_start&&!f->blocked)out.held|=CAB_P2_START;
    s->prev_start[0]=start;s->prev_start[1]=start2;
    for(int h=0;h<2;++h){s->prev_trigger[h]=f->trigger[h];s->prev_grip[h]=f->grip[h];}
    return out;
}
