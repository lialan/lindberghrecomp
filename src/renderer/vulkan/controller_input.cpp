#include "controller_input.hpp"
#include <algorithm>
#include <cmath>
#include "../../runtime/xr_settings.h"
#include <stdexcept>
namespace lindbergh::vk {
ControllerInput::ControllerInput(ControllerSettings settings):settings_(settings)
{
    if(settings.width<=0||settings.height<=0||!std::isfinite(settings.aim_distance)||settings.aim_distance<=0)
        throw std::invalid_argument("Invalid controller dimensions/aim distance");
    controls_.gun_hand=settings.left_handed?0:1;
}
void ControllerInput::configure(ControllerSettings settings,int hand)
{
    if(settings.width<=0||settings.height<=0||!std::isfinite(settings.aim_distance)||settings.aim_distance<=0||hand>1)
        throw std::invalid_argument("Invalid controller settings");
    settings_=settings;if(hand>=0)controls_.gun_hand=hand;
}
std::array<ControllerHaptic,2> ControllerInput::update(const StereoScene &scene,const ControllerSample &sample,int64_t time,bool focused,bool select_hand)
{
    overlay_={};
    ControllerSnapshot next;next.dual=settings_.dual;std::array<ControllerHaptic,2> haptics{};
    focused=focused&&scene.tracking_valid();
    if(focused) {
        auto frame=sample.buttons;frame.time=time;frame.dual=settings_.dual;frame.auto_coins=settings_.auto_coins;
        const int main=select_hand?lg_xr_controls_select(&controls_,frame.trigger,frame.dual):controls_.gun_hand;
        // A flat frame (a scope) is a picture on the screen, and the close-
        // quarters markers are 2D on it: aimed where the laser meets the
        // screen, as stereo.c's host does.
        const bool screen_aim=scene.flat()||scene.cqc(),on_hud=settings_.aim_on_hud||screen_aim;
        for(int pl=0;pl<(frame.dual?2:1);++pl) {
            const int hand=pl?1-main:main;
            if(sample.valid[hand]) {
                frame.aimed[pl]=scene.aim(sample.pose[hand],settings_.width,settings_.height,on_hud,settings_.aim_distance,next.ray[pl]);
                next.valid[pl]=1;
                for(float v:next.ray[pl].origin)if(!std::isfinite(v))next.valid[pl]=0;
                for(float v:next.ray[pl].direction)if(!std::isfinite(v))next.valid[pl]=0;
            }
            const bool firing=frame.trigger[hand]&&frame.aimed[pl]&&!frame.blocked;
            if(firing&&!controls_.prev_trigger[hand])fire_start_[pl]=time;
            const float phase=firing?std::fmod(float((time-fire_start_[pl])/1000000)/100.f,1.f):1.f;
            if(firing&&phase<fire_phase_[pl])haptics[hand]={.7f,35};
            if(frame.trigger[hand]&&!frame.aimed[pl]&&!controls_.prev_trigger[hand]&&!frame.blocked)haptics[hand]={.3f,40};
            auto &f=overlay_[pl];f.gun_valid=next.valid[pl];f.p2=pl;
            if(f.gun_valid) {
                std::copy(sample.pose[hand].orientation.begin(),sample.pose[hand].orientation.end(),f.gun_q);
                std::copy(sample.pose[hand].position.begin(),sample.pose[hand].position.end(),f.gun_p);
                f.recoil=firing?.018f*(1-phase)*(1-phase):0;f.flash=firing&&phase<.35f;
                const float tip[]={0,-.006f,-.26f};float muzzle[3];
                lg_stereo_rotate(f.gun_q,tip,muzzle);
                float reach=5;
                if(frame.aimed[pl]) {
                    StereoRay world;
                    reach=settings_.aim_on_hud&&!screen_aim&&scene.aim(sample.pose[hand],settings_.width,settings_.height,false,settings_.aim_distance,world)?world.reach:next.ray[pl].reach;
                }
                for(int i=0;i<3;++i){f.muzzle[i]=f.gun_p[i]+muzzle[i];f.end[i]=f.gun_p[i]+next.ray[pl].direction[i]*reach;}
                f.beam=settings_.laser==LASER_ON;f.dot=frame.aimed[pl]&&settings_.laser!=LASER_OFF;
                f.show_gun=settings_.show_gun;f.sight=f.show_gun?settings_.sight:XO_SIGHT_NONE;f.gun_model=settings_.gun_model;
            }
            fire_phase_[pl]=phase;
        }
        const auto cabinet=lg_xr_controls_update(&controls_,&frame);next.held=cabinet.held|sample.extra_held;next.coins=cabinet.coins+sample.extra_coins;
        if(frame.blocked)for(int pl=0;pl<2;++pl){next.valid[pl]=0;next.ray[pl].x=next.ray[pl].y=-100000;}
    } else {
        for(int h=0;h<2;++h){controls_.prev_trigger[h]=controls_.prev_grip[h]=controls_.prev_start[h]=0;fire_phase_[h]=1;}
    }
    producer_=next;
    std::lock_guard<std::mutex> lock(mutex_);
    pulses_=focused?((sample.buttons.blocked?0:pulses_)|(next.held&~last_held_)):0;
    last_held_=next.held;next.coins+=pending_.coins;pending_=next;available_=true;return haptics;
}
bool ControllerInput::consume(ControllerSnapshot &snapshot)
{
    std::lock_guard<std::mutex> lock(mutex_);if(!available_)return false;
    snapshot=pending_;snapshot.held|=pulses_;pulses_=0;pending_.coins=0;return true;
}
}
