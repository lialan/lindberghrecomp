#include "overlay.hpp"
#include "../../runtime/xr_overlay_geometry.h"
#include "overlay_gun_vert.hpp"
#include "overlay_gun_frag.hpp"
#include "overlay_beam_vert.hpp"
#include "overlay_beam_frag.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace lindbergh::vk {
namespace {
struct Parameters {float model[16],color[4],shape[4];};
struct BeamVertex {float left[3],right[3],uv[2];};
RasterState gun_state() {RasterState r;r.depth_compare=VK_COMPARE_OP_LESS;return r;}
RasterState beam_state() {
    RasterState r;r.depth_test=r.depth_write=false;r.blend=true;
    r.src_rgb=VK_BLEND_FACTOR_SRC_ALPHA;r.dst_rgb=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    r.src_alpha=VK_BLEND_FACTOR_ZERO;r.dst_alpha=VK_BLEND_FACTOR_ONE;return r;
}
RasterState glass_state() {auto r=beam_state();r.depth_test=r.depth_write=true;r.depth_compare=VK_COMPARE_OP_ALWAYS;return r;}
RasterState reticle_state() {auto r=beam_state();r.depth_test=true;r.depth_compare=VK_COMPARE_OP_EQUAL;return r;}
}
Overlay::Overlay(Device &device,FrameRing &ring,PipelineCache &cache):device_(device),ring_(ring),cache_(cache),uniforms_(device,ring,{{0,sizeof(Parameters)}})
{
    std::vector<float> vertices(size_t(xo_gun_capacity())*9);auto cmd=ring_.begin();
    for(int model=0;model<3;++model)for(int sight=0;sight<3;++sight) {
        auto &mesh=meshes_[model*3+sight];
        const int count=xo_gun_build(model,sight,vertices.data(),xo_gun_capacity(),&mesh.rail);
        if(!count)throw std::runtime_error("Cannot build shared overlay mesh");
        mesh.vertices=uint32_t(count);const auto bytes=size_t(count)*9*sizeof(float);
        mesh.buffer=std::make_unique<GeometryBuffer>(device,bytes);mesh.buffer->upload(cmd,ring,vertices.data(),bytes);
    }
    ring_.submit(); // queue-ordered once at initialization; no CPU wait
}
void Overlay::prepare(Target &target)
{
    if(targets_.count(&target))return;
    auto &bundle=targets_[&target];
    ProgramSource gun{overlay_gun_vert,overlay_gun_frag,sizeof overlay_gun_vert,sizeof overlay_gun_frag,
        {{0,36,VK_VERTEX_INPUT_RATE_VERTEX}},{{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32_SFLOAT,12},{2,0,VK_FORMAT_R32G32B32_SFLOAT,24}},
        {uniforms_.layout()}};
    ProgramSource beam{overlay_beam_vert,overlay_beam_frag,sizeof overlay_beam_vert,sizeof overlay_beam_frag,
        {{0,sizeof(BeamVertex),VK_VERTEX_INPUT_RATE_VERTEX}},{{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32_SFLOAT,12},{2,0,VK_FORMAT_R32G32_SFLOAT,24}},
        {uniforms_.layout()}};
    bundle.gun=std::make_unique<Renderer>(device_,target,cache_,gun);bundle.beam=std::make_unique<Renderer>(device_,target,cache_,beam);
    bundle.gun->prepare(gun_state());bundle.beam->prepare(beam_state());bundle.beam->prepare(glass_state());bundle.beam->prepare(reticle_state());
}
uint64_t Overlay::draw_count() const {
    uint64_t count=0;for(const auto &entry:targets_)count+=entry.second.gun->draw_count()+entry.second.beam->draw_count();return count;
}
void Overlay::draw(VkCommandBuffer cmd,Target &target,const Matrices &views,const std::array<std::array<float,3>,2> &eyes,
                   const std::array<XoFrame,2> &frames,Renderer *previous,bool preserve)
{
    auto found=targets_.find(&target);if(found==targets_.end())throw std::logic_error("Overlay target was not prepared");
    auto &bundle=found->second;bool any=false;
    for(const auto &f:frames)any|=f.gun_valid&&(f.show_gun||f.beam||f.dot||f.flash);
    if(!any){if(previous)previous->end();return;}
    Renderer *active;
    if(previous){previous->detach();bundle.gun->attach(cmd);active=bundle.gun.get();}
    else{bundle.gun->begin(cmd,{0,0,0,1},preserve);active=bundle.gun.get();++pass_starts_;}
    const VkRect2D rect{{0,0},{target.info.width,target.info.height}};
    active->viewport(0,0,float(target.info.width),float(target.info.height),rect);bool forced_depth=false;
    auto select=[&](Renderer &renderer,bool force) {
        if(active!=&renderer){active->detach();renderer.attach(cmd);active=&renderer;}
        if(force!=forced_depth){active->viewport(0,0,float(target.info.width),float(target.info.height),rect,force?1.f:0.f,1);forced_depth=force;}
    };
    auto bind=[&](const Parameters &p) {
        const auto offset=uniforms_.upload(0,&p,sizeof p);const auto set=uniforms_.current_set();active->bind_descriptors(0,1,&set,1,&offset);
    };
    auto shape=[&](const float q[2][6][5],const std::array<float,4> &color,float ring,float thick,RasterState raster,bool force) {
        select(*bundle.beam,force);Parameters p{};std::copy(color.begin(),color.end(),p.color);p.shape[0]=ring;p.shape[1]=thick;bind(p);
        const int uv_eye=q[0][0][3]!=0?0:1;
        BeamVertex vertices[6];for(int i=0;i<6;++i) {
            std::memcpy(vertices[i].left,q[0][i],12);std::memcpy(vertices[i].right,q[1][i],12);std::memcpy(vertices[i].uv,q[uv_eye][i]+3,8);
        }
        auto stream=ring_.upload(vertices,sizeof vertices);active->draw_buffers(&stream,1,6,views,raster);
    };
    auto spot=[&](const float *position,const std::array<float,2> &radii,const std::array<float,4> &color,float ring,float thick,bool reticle) {
        float q[2][6][5]{};bool visible=false;
        for(int eye=0;eye<2;++eye)visible|=xo_spot_vertices(q[eye],eyes[eye].data(),position,radii[eye])!=0;
        if(visible)shape(q,color,ring,thick,reticle?reticle_state():beam_state(),reticle);
    };
    for(const auto &f:frames) {
        if(!f.gun_valid)continue;
        if(f.gun_model<0||f.gun_model>2||f.sight<0||f.sight>2)throw std::invalid_argument("Invalid overlay model/sight");
        const std::array<float,4> color=f.p2?std::array<float,4>{.15f,.45f,1,1}:std::array<float,4>{1,.12f,.08f,1};
        const auto &mesh=meshes_[f.gun_model*3+f.sight];Matrix model;
        xo_pose_matrix(f.gun_q,f.gun_p,model.data());auto kick=identity();kick[14]=f.recoil;model=multiply(model,kick);
        if(f.show_gun) {
            select(*bundle.gun,false);active->clear(VK_IMAGE_ASPECT_DEPTH_BIT,{},1,0,rect);
            Parameters p{};std::copy(model.begin(),model.end(),p.model);bind(p);Matrices mvp;
            for(int eye=0;eye<2;++eye){Matrix vp;std::copy(views.view_projection[eye],views.view_projection[eye]+16,vp.begin());const auto m=multiply(vp,model);std::copy(m.begin(),m.end(),mvp.view_projection[eye]);}
            const Upload stream{mesh.buffer->handle(),0};active->draw_buffers(&stream,1,mesh.vertices,mvp,gun_state());
        }
        std::array<float,2> distances;
        for(int eye=0;eye<2;++eye){float d=0;for(int i=0;i<3;++i)d+=(f.end[i]-eyes[eye][i])*(f.end[i]-eyes[eye][i]);distances[eye]=std::sqrt(d);}
        auto radii=[&](float scale){return std::array<float,2>{scale*distances[0],scale*distances[1]};};
        if(f.show_gun&&f.sight!=XO_SIGHT_NONE) {
            select(*bundle.beam,true);active->clear(VK_IMAGE_ASPECT_DEPTH_BIT,{},0,0,rect);
            float q[2][6][5];xo_glass_vertices(q[0],model.data(),f.sight,mesh.rail);std::memcpy(q[1],q[0],sizeof q[0]);
            shape(q,{.55f,.75f,.85f,.1f},0,0,glass_state(),true);
            if(f.sight==XO_SIGHT_RED_DOT)spot(f.end,radii(.0012f),color,0,0,true);
            else{auto ringcolor=color;ringcolor[3]=.95f;spot(f.end,radii(.0105f),ringcolor,.9f,.08f,true);spot(f.end,radii(.0009f),color,0,0,true);}
            active->clear(VK_IMAGE_ASPECT_DEPTH_BIT,{},1,0,rect);
        }
        if(f.beam) {
            float q[2][6][5]{};bool visible=false;
            for(int eye=0;eye<2;++eye)visible|=xo_beam_vertices(q[eye],eyes[eye].data(),f.muzzle,f.end,.0015f)!=0;
            if(visible){auto beamcolor=color;beamcolor[3]=.85f;shape(q,beamcolor,0,0,beam_state(),false);}
        }
        if(f.dot)spot(f.end,radii(.006f),color,0,0,false);
        if(f.flash)spot(f.muzzle,{.045f,.045f},{1,.75f,.3f,.9f},0,0,false);
    }
    active->end();
}
}
