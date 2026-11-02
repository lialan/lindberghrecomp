#include "overlay.hpp"
#include "../../runtime/xr_overlay_geometry.h"
#include "overlay_gun_vert.hpp"
#include "overlay_gun_frag.hpp"
#include "overlay_beam_vert.hpp"
#include "overlay_beam_frag.hpp"
#include "overlay_panel_vert.hpp"
#include "overlay_panel_frag.hpp"
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
RasterState picture_state() {RasterState r;r.depth_test=r.depth_write=false;r.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;return r;}
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
    const float quad[]={-1,-1,1,-1,-1,1,1,1};
    quad_=std::make_unique<GeometryBuffer>(device,sizeof quad);quad_->upload(cmd,ring,quad,sizeof quad);
    SamplerInfo sampling;sampling.min_filter=sampling.mag_filter=VK_FILTER_LINEAR;
    sampling.u=sampling.v=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    panel_sampler_=std::make_unique<Sampler>(device,sampling);
    for(unsigned i=0;i<2;++i) {
        panel_textures_[i]=std::make_unique<Texture>(device,TextureInfo{uint32_t(i?XM_STATS_W:XM_W),uint32_t(i?XM_STATS_H:XM_H)});
        panel_sets_[i]=std::make_unique<SampledSet>(device,std::vector<SampledImage>{{0,panel_textures_[i]->view(),panel_sampler_->handle()}});
    }
    ring_.submit(); // queue-ordered once at initialization; no CPU wait
}
void Overlay::upload_panel(VkCommandBuffer cmd,unsigned slot,const void *rgba,size_t bytes)
{
    if(slot>=2||!rgba||bytes!=size_t(panel_textures_[slot]->info.width)*panel_textures_[slot]->info.height*4)
        throw std::invalid_argument("Invalid panel image");
    panel_textures_[slot]->upload(cmd,ring_,0,0,rgba,bytes);panel_ready_[slot]=true;++panel_uploads_;
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
    ProgramSource panel{overlay_panel_vert,overlay_panel_frag,sizeof overlay_panel_vert,sizeof overlay_panel_frag,
        {{0,8,VK_VERTEX_INPUT_RATE_VERTEX}},{{0,0,VK_FORMAT_R32G32_SFLOAT,0}},{panel_sets_[0]->layout()}};
    bundle.panel=std::make_unique<Renderer>(device_,target,cache_,panel);
    auto panel_state=beam_state();panel_state.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;bundle.panel->prepare(panel_state);
    bundle.panel->prepare(picture_state());
    bundle.gun->prepare(gun_state());bundle.beam->prepare(beam_state());bundle.beam->prepare(glass_state());bundle.beam->prepare(reticle_state());
}
void Overlay::draw_picture(VkCommandBuffer cmd,Target &target,const Matrices &screen)
{
    auto found=targets_.find(&target);if(found==targets_.end())throw std::logic_error("Overlay target was not prepared");
    auto &panel=*found->second.panel;
    const auto &t=target.info;
    if(!picture_||picture_->info.width!=t.width||picture_->info.height!=t.height||picture_->info.format!=t.color_format) {
        if(picture_){ring_.retire(picture_set_);ring_.retire(picture_);}
        TextureInfo info{t.width,t.height};info.format=t.color_format;
        picture_=std::make_shared<Texture>(device_,info);
        picture_set_=std::make_shared<SampledSet>(device_,std::vector<SampledImage>{{0,picture_->view(),panel_sampler_->handle()}});
    }
    ring_.keep_alive(picture_);ring_.keep_alive(picture_set_);
    // The picture is the same in both eyes (every draw kept the game's own
    // projection): the first layer is shown to both. A raw copy, row for row:
    // an XR swapchain image is sRGB seen through a UNORM view, and a blit
    // would decode it. Texture::copy_from is glCopyTexSubImage's (GL rows).
    VkImageMemoryBarrier to_copy[2]{};
    for(auto &b:to_copy){b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
    to_copy[0].image=target.color;to_copy[0].oldLayout=t.final_layout;to_copy[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_copy[0].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;to_copy[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    to_copy[1].image=picture_->image();to_copy[1].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;to_copy[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_copy[1].srcAccessMask=VK_ACCESS_SHADER_READ_BIT;to_copy[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,0,nullptr,0,nullptr,2,to_copy);
    VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.extent={t.width,t.height,1};
    vkCmdCopyImage(cmd,target.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,picture_->image(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    VkImageMemoryBarrier after[2]{to_copy[0],to_copy[1]};
    std::swap(after[0].oldLayout,after[0].newLayout);
    after[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;after[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    after[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;after[1].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    after[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;after[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0,0,nullptr,0,nullptr,2,after);
    panel.begin(cmd,{0,0,0,1});++pass_starts_;
    const VkRect2D rect{{0,0},{t.width,t.height}};
    panel.viewport(0,0,float(t.width),float(t.height),rect);
    const auto set=picture_set_->handle();panel.bind_descriptors(0,1,&set);
    const Upload stream{quad_->handle(),0};
    panel.draw_buffers(&stream,1,4,screen,picture_state());
    panel.end();++picture_draws_;
}
uint64_t Overlay::draw_count() const {
    uint64_t count=0;for(const auto &entry:targets_)count+=entry.second.gun->draw_count()+entry.second.beam->draw_count()+entry.second.panel->draw_count();return count;
}
void Overlay::draw(VkCommandBuffer cmd,Target &target,const Matrices &views,const std::array<std::array<float,3>,2> &eyes,
                   const std::array<XoFrame,2> &frames,Renderer *previous,bool preserve,const std::array<PanelDraw,2> &panels)
{
    auto found=targets_.find(&target);if(found==targets_.end())throw std::logic_error("Overlay target was not prepared");
    auto &bundle=found->second;bool any=false;
    for(const auto &f:frames)any|=f.gun_valid&&(f.show_gun||f.beam||f.dot||f.flash);
    for(unsigned i=0;i<2;++i)any|=panels[i].visible&&panel_ready_[i];
    if(!any){if(previous)previous->end();return;}
    Renderer *active;
    if(previous){previous->detach();bundle.gun->attach(cmd);active=bundle.gun.get();}
    // The overlay is the last thing drawn on its target in a frame.
    else{bundle.gun->begin(cmd,{0,0,0,1},preserve,1,0,nullptr,false,true);active=bundle.gun.get();++pass_starts_;}
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
    auto draw_panel=[&](unsigned slot) {
        if(!panels[slot].visible||!panel_ready_[slot])return;
        select(*bundle.panel,false);const auto set=panel_sets_[slot]->handle();active->bind_descriptors(0,1,&set);
        Matrices mvp;for(int eye=0;eye<2;++eye){Matrix vp;std::copy(views.view_projection[eye],views.view_projection[eye]+16,vp.begin());
            const auto m=multiply(vp,panels[slot].model);std::copy(m.begin(),m.end(),mvp.view_projection[eye]);}
        const Upload stream{quad_->handle(),0};auto raster=beam_state();raster.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        active->draw_buffers(&stream,1,4,mvp,raster);
    };
    if(panels[0].visible){draw_panel(1);draw_panel(0);} // guns stay over an open menu, with one draw each
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
    if(!panels[0].visible)draw_panel(1);
    active->end();
}
}
