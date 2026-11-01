#include "settings_panel.hpp"
#include "../../runtime/host_stats.h"
#include "../../runtime/cabinet_buttons.h"
#include <algorithm>
#include <cstdio>
namespace lindbergh::vk {
SettingsPanel::SettingsPanel(int width,int height,const char *title,bool fake):width_(width),height_(height),fake_(fake),
    title_(std::string(title?title:"Lindbergh")+" - settings"),panel_pixels_(XM_W*XM_H*4),label_pixels_(XM_STATS_W*XM_STATS_H*4) {}
unsigned SettingsPanel::update(LgXrSettings &cfg,StereoScene &scene,ControllerInput &input,ControllerSample sample,
                              int64_t time,bool focused,const std::vector<float> &rates,int64_t readout_time)
{
    draws_={};guns_={};haptics_={};unsigned effects=0;
    focused=focused&&scene.tracking_valid();
    if(!focused){previous_menu_=previous_upper_[0]=previous_upper_[1]=previous_trigger_[0]=previous_trigger_[1]=0;held_=stats_since_=stats_frames_=0;
        input.update(scene,{},time,false);return 0;}
    const bool dual=cfg.dual;
    const int main=input.select_hand(sample.buttons.trigger,cfg.dual),off=1-main;
    int hand=main;
    float head[3];for(int i=0;i<3;++i)head[i]=.5f*(scene.overlay_eyes()[0][i]+scene.overlay_eyes()[1][i]);
    bool toggled=xp_menu_button(&panel_,sample.menu,previous_menu_);
    const bool held_toggle=xp_hold_button(&panel_,sample.buttons.upper[off],previous_upper_[off],cfg.dual,time,&held_);
    toggled|=held_toggle;
    if(fake_&&!panel_.open){panel_.open=1;panel_.dirty=1;panel_.hover=-1;toggled=true;}
    if(toggled&&panel_.open)xp_place(&panel_,head,scene.head_orientation().data());
    const bool blocked=panel_.open;
    if(panel_.open) {
        for(int pl=0;pl<(dual?2:1);++pl) {
            const int h=pl?off:main;if(!sample.valid[h])continue;
            const float forward[]={0,0,-1};float direction[3],hit[3],u,v;
            lg_stereo_rotate(sample.pose[h].orientation.data(),forward,direction);
            const bool on=xp_hit(&panel_,sample.pose[h].position.data(),direction,&u,&v,hit);
            const int row=on?xm_row_at(u,v,XP_COUNT):-1;
            if((on||(!pl&&!dual))&&row!=panel_.hover){panel_.hover=row;panel_.dirty=1;}
            if(!fake_&&row>=0&&sample.buttons.trigger[h]&&!previous_trigger_[h])effects|=xp_choose(&panel_,&cfg,&hand,row,rates.data(),int(rates.size()));
        }
    }
    // Settings take effect before the cabinet snapshot and overlay are built.
    // The host performs persistence, refresh, recenter and restart effects.
    sample.buttons.blocked=blocked||panel_.open;
    sample.extra_held=(panel_.test_frames?CAB_TEST:0)|(panel_.service_frames?CAB_SERVICE:0);sample.extra_coins=panel_.coin;
    if(panel_.test_frames)--panel_.test_frames;if(panel_.service_frames)--panel_.service_frames;panel_.coin=0;
    if(effects&XP_SAVE) {
        auto controls=input.settings();controls.dual=cfg.dual;controls.auto_coins=cfg.auto_coins;controls.left_handed=cfg.left_handed;
        controls.show_gun=cfg.show_gun;controls.laser=cfg.laser;controls.sight=cfg.sight;
        input.configure(controls,(effects&XP_HAND_CHANGED)?hand:-1);
        if(effects&(XP_SCREEN_CHANGED|XP_HUD_CHANGED)) {
            auto camera=scene.settings();camera.screen=cfg.screen;camera.hud_pitch=float(cfg.hud_pitch);scene.configure(camera);
        }
    }
    haptics_=input.update(scene,sample,time,true,false);guns_=input.overlay_frames();
    if(held_toggle)haptics_[off]={.3f,20};
    if(panel_.open) {
        draws_[0].visible=true;xp_matrix(&panel_,draws_[0].model.data());
        for(auto &gun:guns_)if(gun.gun_valid) {
            const float forward[]={0,0,-1};float direction[3],hit[3],u,v;
            lg_stereo_rotate(gun.gun_q,forward,direction);
            const bool on=xp_hit(&panel_,gun.gun_p,direction,&u,&v,hit);
            gun.beam=1;gun.dot=on;gun.flash=0;
            for(int i=0;i<3;++i)gun.end[i]=on?hit[i]:gun.gun_p[i]+direction[i]*5;
        }
        if(panel_.dirty){XmItem items[XP_COUNT];xp_items(&panel_,&cfg,input.gun_hand(),scene.head_locked(),height_,width_,items);
            xm_paint(panel_pixels_.data(),title_.c_str(),items,XP_COUNT,panel_.hover,"Point and pull the trigger. Menu button: close.");
            panel_.dirty=0;panel_dirty_=true;}
    }
    if(cfg.show_stats) {
        const int64_t now=readout_time?readout_time:time;
        if(!stats_since_)stats_since_=now;else ++stats_frames_;
        if(now-stats_since_>=500000000) {
            LgStats stats{};const double elapsed=double(now-stats_since_);
            stats.fps=int(double(stats_frames_)*1e9/elapsed+.5);
            stats.frame_ms=stats_frames_?float(elapsed/1e6/stats_frames_):-1;
            stats.eye_w=width_;stats.eye_h=height_;
            lg_stats_sample(&stats);stats_since_=now;stats_frames_=0;
            char lines[LG_STATS_LINES][LG_STATS_LINE];lg_stats_format(&stats,lines);
            std::string text;for(const auto &l:lines){text+=l;text+='\n';}
            if(text!=stats_text_) {
                const char *rows[LG_STATS_LINES];for(int i=0;i<LG_STATS_LINES;++i)rows[i]=lines[i];
                xm_paint_lines(label_pixels_.data(),rows,LG_STATS_LINES);label_dirty_=true;stats_text_=std::move(text);
            }
            if(stats_samples_++%10==0) // every 5 s: the same numbers in the log
                std::fprintf(stderr,"[stats] %s | %s | %s | %s\n",lines[0],lines[1],lines[2],lines[3]);
            stats_shown_=true;
        }
        if(stats_shown_){
            // Head-locked in the top-right corner, about 1 m out: centre
            // 0.30 m right and 0.22 m up (about 17 and 12 degrees).
            XpPanel label{};label.hw=.11f;label.hh=label.hw*XM_STATS_H/XM_STATS_W;
            const float x[]={1,0,0},y[]={0,1,0},z[]={0,0,1},at[]={.30f,.22f,-1};float offset[3];const auto &q=scene.head_orientation();
            lg_stereo_rotate(q.data(),x,label.right);lg_stereo_rotate(q.data(),y,label.up);lg_stereo_rotate(q.data(),z,label.n);lg_stereo_rotate(q.data(),at,offset);
            for(int i=0;i<3;++i)label.c[i]=head[i]+offset[i];draws_[1].visible=true;xp_matrix(&label,draws_[1].model.data());}
    }else{stats_since_=stats_frames_=0;}
    previous_menu_=sample.menu;for(int h=0;h<2;++h){previous_upper_[h]=sample.buttons.upper[h];previous_trigger_[h]=sample.buttons.trigger[h];}
    return effects;
}
void SettingsPanel::upload(Overlay &overlay,VkCommandBuffer cmd)
{
    if(panel_dirty_){overlay.upload_panel(cmd,0,panel_pixels_.data(),panel_pixels_.size());panel_dirty_=false;}
    if(label_dirty_){overlay.upload_panel(cmd,1,label_pixels_.data(),label_pixels_.size());label_dirty_=false;}
}
}
