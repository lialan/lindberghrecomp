#include "stereo_scene.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstring>
namespace lindbergh::vk {
StereoScene::StereoScene(StereoSettings settings):settings_(settings)
{
    if(!std::isfinite(settings.scale)||settings.scale<=0 || !std::isfinite(settings.screen)||settings.screen<=0)
        throw std::invalid_argument("Invalid stereo camera scale/screen");
    cache_.reserve(32);
    for(auto &m:identity_.view_projection) for(unsigned i=0;i<4;++i)m[i*5]=1;
}
void StereoScene::configure(StereoSettings settings)
{
    if(!std::isfinite(settings.scale)||settings.scale<=0||!std::isfinite(settings.screen)||settings.screen<=0)
        throw std::invalid_argument("Invalid stereo settings");
    settings_=settings;if(valid_)publish(eyes_);
}
void StereoScene::invalidate() { valid_=false;cache_.clear();last_=0;builds_=0; }
void StereoScene::publish(const std::array<StereoPose,2> &eyes)
{
    invalidate();eyes_=eyes;
    const auto &head=eyes[0].orientation;
    const float inverse[]={-head[0],-head[1],-head[2],head[3]},unit[]={0,0,0,1};
    for(unsigned eye=0;eye<2;++eye) {
        for(float v:eyes[eye].tangents) if(!std::isfinite(v)||v<=0) throw std::invalid_argument("Invalid stereo field of view");
        for(float v:eyes[eye].position) if(!std::isfinite(v)) throw std::invalid_argument("Invalid stereo eye position");
        for(float v:eyes[eye].orientation) if(!std::isfinite(v)) throw std::invalid_argument("Invalid stereo orientation");
        tangents_[eye]=eyes[eye].tangents;
        eye_positions_[eye]=eyes[eye].position;
        Matrix overlay_view,projection{};
        lg_stereo_pose_view(eyes[eye].orientation.data(),eyes[eye].position.data(),1,overlay_view.data());
        const auto &t=eyes[eye].tangents;const float n=.02f,f=100;
        projection[0]=2/(t[0]+t[1]);projection[5]=2/(t[2]+t[3]);
        projection[8]=(t[1]-t[0])/(t[0]+t[1]);projection[9]=(t[3]-t[2])/(t[2]+t[3]);
        projection[10]=-(f+n)/(f-n);projection[11]=-1;projection[14]=-2*f*n/(f-n);
        const auto vp=multiply(projection,overlay_view);std::copy(vp.begin(),vp.end(),overlay_.view_projection[eye]);
        lg_stereo_pose_view(eyes[eye].orientation.data(),eyes[eye].position.data(),settings_.scale,view_[eye].data());
        float offset[3],relative[3];
        for(unsigned i=0;i<3;++i) offset[i]=eyes[eye].position[i]-.5f*(eyes[0].position[i]+eyes[1].position[i]);
        lg_stereo_rotate(inverse,offset,relative);
        lg_stereo_pose_view(unit,relative,settings_.scale,hud_view_[eye].data());
        if(!settings_.head_locked) hud_view_[eye]=view_[eye];
        else if(settings_.hud_pitch!=0) {
            const float a=settings_.hud_pitch*.017453292519943295f;
            auto tilt=identity();tilt[5]=tilt[10]=std::cos(a);tilt[6]=std::sin(a);tilt[9]=-std::sin(a);
            hud_view_[eye]=multiply(hud_view_[eye],tilt);
        }
    }
    valid_=true;
}
bool StereoScene::aim(const StereoPose &controller,int width,int height,bool on_hud,float aim_distance,StereoRay &ray) const
{
    ray={};if(!valid_)return false;
    const float forward[]={0,0,-1};lg_stereo_rotate(controller.orientation.data(),forward,ray.direction.data());
    for(unsigned i=0;i<3;++i)ray.origin[i]=controller.position[i]*settings_.scale;
    auto o=ray.origin,d=ray.direction;
    if(on_hud && settings_.head_locked) {
        const auto &head=eyes_[0].orientation;
        const float inverse[]={-head[0],-head[1],-head[2],head[3]};
        for(unsigned i=0;i<3;++i)o[i]-=.5f*(eyes_[0].position[i]+eyes_[1].position[i])*settings_.scale;
        lg_stereo_rotate(inverse,o.data(),o.data());lg_stereo_rotate(inverse,d.data(),d.data());
        const float a=settings_.hud_pitch*.017453292519943295f,c=std::cos(a),s=std::sin(a);
        for(auto *v:{&o,&d}) {const float y=(*v)[1],z=(*v)[2];(*v)[1]=c*y+s*z;(*v)[2]=-s*y+c*z;}
    }
    float world_reach=0;
    if(!on_hud) {
        if(d[2]>-1e-4f)return false;
        const float t=(-aim_distance*settings_.scale-o[2])/d[2];if(t<=0)return false;world_reach=t/settings_.scale;
        for(unsigned i=0;i<3;++i)d[i]=o[i]+t*d[i];o={0,0,0};
    }
    float x,y,reach;
    if(!lg_stereo_ray_to_plane(o.data(),d.data(),screen_lens().data(),settings_.screen*settings_.scale,width,height,&x,&y,&reach) ||
       !std::isfinite(x)||!std::isfinite(y)||x<0||y<0||x>=width||y>=height)return false;
    ray.x=int(x);ray.y=int(y);ray.reach=on_hud?reach/settings_.scale:world_reach;return true;
}
bool StereoScene::world_camera(const Matrix &p) const
{
    return lg_stereo_is_perspective(p.data()) && !(settings_.hud_lens>0 && std::fabs(p[0]-settings_.hud_lens)<.002f);
}
bool StereoScene::zoomed(const Matrix &p) const {return world_camera(p)&&flat_;}
const Matrices &StereoScene::transforms(const Matrix &p,int kind)
{
    if(!valid_ || kind==LG_STEREO_PASS || (kind==LG_STEREO_WORLD&&!lg_stereo_is_perspective(p.data()))) return identity_;
    auto matches=[&](const Entry &e){return e.kind==kind && (kind!=LG_STEREO_WORLD || e.projection==p);};
    if(last_<cache_.size()&&matches(cache_[last_]))return cache_[last_].matrices;
    for(size_t i=0;i<cache_.size();++i) if(matches(cache_[i])) {last_=i;return cache_[i].matrices;}
    if(cache_.size()==32)cache_.clear(); // bounded storage, no per-draw allocation growth
    Entry e{};e.projection=p;e.kind=kind;
    const auto unit=identity();
    for(unsigned eye=0;eye<2;++eye) {
        auto &out=e.matrices.view_projection[eye];
        if(kind==LG_STEREO_FILL) {std::memcpy(out,unit.data(),64);out[10]=0;out[14]=-.999f;continue;}
        LgStereoProjection policy{view_[eye].data(),hud_view_[eye].data(),tangents_[eye].data(),screen_lens().data(),settings_.screen*settings_.scale};
        Matrix effective;
        // HUD maps guest clip space straight onto its screen plane. It is
        // independent of the original lens, so needs one transform per frame.
        lg_stereo_eye_projection(kind==LG_STEREO_HUD?unit.data():p.data(),kind,&policy,effective.data());
        if(kind==LG_STEREO_HUD)std::memcpy(out,effective.data(),64);
        else if(!lg_stereo_clip_reprojection(p.data(),effective.data(),out))
            throw std::runtime_error("Cannot reproject a singular guest camera");
    }
    cache_.push_back(e);last_=cache_.size()-1;++builds_;return cache_.back().matrices;
}
void StereoScene::observe(const Matrix &p,int kind)
{
    if(!world_camera(p))return;
    longest_=std::max(longest_,p[0]);
    if(kind!=LG_STEREO_WORLD)return;
    for(auto &l:tally_)if(l.draws && std::fabs(l.projection[0]-p[0])<.002f && std::fabs(l.projection[5]-p[5])<.002f) {++l.draws;return;}
    for(auto &l:tally_)if(!l.draws) {l.projection=p;l.draws=1;return;}
}
void StereoScene::finish_frame(bool sniping,bool cqc,bool menu)
{
    if(sniping)snipe_=3;
    if(cqc)cqc_=3;
    const float limit=zoom_limit();
    const auto best=std::max_element(tally_.begin(),tally_.end(),[](const auto &a,const auto &b){return a.draws<b.draws;});
    if(!flat_ && best->draws && best->projection[0]>.3f && best->projection[0]<limit) lens_={best->projection[0],best->projection[5]};
    for(auto &l:tally_)l.draws=0;
    // a scope is held; a lens passing through on a camera move is not one
    zoom_frames_=longest_>limit?zoom_frames_+1:0;longest_=0;
    flat_=menu||zoom_frames_>=6;
    if(snipe_)--snipe_;
    if(cqc_)--cqc_;
    cache_.clear();last_=0;
}
} // namespace lindbergh::vk
