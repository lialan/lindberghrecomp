#include "stereo_scene.hpp"
#include "controller_input.hpp"
#include "overlay.hpp"
#include "settings_panel.hpp"
#include "../../src/runtime/xr_sight.h"
#include "../../src/runtime/cabinet_buttons.h"
#include "../../src/runtime/stereo_projection.h"
#include "device.hpp"
#include "frame_ring.hpp"
#include "renderer.hpp"
#include "guest_state.hpp"
#include "guest_arrays.hpp"
#include "guest_pixels.hpp"
#include "uniforms.hpp"
#include "resources.hpp"
#include "shader_pack.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace lindbergh::vk;
static void require(bool test, const char *what)
{
    if (!test) throw std::runtime_error(what);
}

// Cross-compiled tests stage only source-owned fixtures onto the simulator.
static std::string fixture_path(const char *directory, const char *fallback, const char *name)
{
    const char *override = std::getenv(directory);
    return std::string(override ? override : fallback) + "/" + name;
}

static std::string source(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "missing source-owned shader fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
static std::string hex(const SourceDigest &hash)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : hash) { out += digits[byte >> 4]; out += digits[byte & 15]; }
    return out;
}

template<class F> static void rejected(F fn, const char *what)
{
    bool threw = false;
    try { fn(); } catch (const std::exception &) { threw = true; }
    require(threw, what);
}

struct Readback {
    Device &device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    explicit Readback(Device &d, VkDeviceSize size) : device(d)
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(d.handles.device, &info, nullptr, &buffer), "readback buffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(d.handles.device, buffer, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = d.memory_type(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(d.handles.device, &alloc, nullptr, &memory), "readback memory");
        check(vkBindBufferMemory(d.handles.device, buffer, memory, 0), "bind readback");
        check(vkMapMemory(d.handles.device, memory, 0, size, 0, &mapped), "map readback");
    }
    ~Readback()
    {
        if (mapped) vkUnmapMemory(device.handles.device, memory);
        if (buffer) vkDestroyBuffer(device.handles.device, buffer, nullptr);
        if (memory) vkFreeMemory(device.handles.device, memory, nullptr);
    }
};

static void render_density_reuse(Device &device)
{
    if(!device.capabilities.density_map || !device.capabilities.density_map_non_subsampled) {
        std::puts("skip: density-map framebuffer reuse requires supported non-subsampled FDM");return;
    }
    FrameRing ring(device);ConstantDensityMap full(device,ring,32,32,2),coarse(device,ring,32,32,2,.5f);
    TargetInfo info{32,32,2};info.density_map=full.view();info.final_layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Target target(device,info);const auto first=target.framebuffer;
    target.prepare_density_maps({full.view(),coarse.view(),full.view()});
    require(target.density_framebuffer_count()==2,"density-map framebuffers were not deduplicated");
    target.select_density_map(coarse.view());const auto second=target.framebuffer;
    require(first!=second,"density-map selection retained the wrong framebuffer");
    rejected([&]{target.select_density_map(VK_NULL_HANDLE);},"unknown density-map view accepted");
    PipelineCache cache(device,"");Renderer renderer(device,target,cache);RasterState raster;renderer.prepare(raster);
    Readback output(device,3*32*32*4*2);Matrices matrices{};
    for(auto &m:matrices.view_projection)for(unsigned i=0;i<4;++i)m[i*5]=1;
    Vertex triangle[]={{{-.9f,-.9f,0,1},{1,0,0,1}},{{.9f,-.9f,0,1},{1,0,0,1}},{{0,.9f,0,1},{1,0,0,1}}};
    Submission last{};
    for(unsigned frame=0;frame<3;++frame) {
        target.select_density_map(frame%2?coarse.view():full.view());
        require(target.framebuffer==(frame%2?second:first),"density-map framebuffer was recreated during selection");
        auto cmd=ring.begin();renderer.begin(cmd,{0,0,0,1});renderer.draw(ring,triangle,3,matrices,raster);renderer.end();
        VkBufferImageCopy copy{};copy.bufferOffset=frame*32*32*4*2;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,2};copy.imageExtent={32,32,1};
        vkCmdCopyImageToBuffer(cmd,target.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.buffer,1,&copy);
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=output.buffer;b.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr);
        last=ring.submit();
    }
    ring.wait(last);const auto *pixels=static_cast<const uint8_t *>(output.mapped);
    for(unsigned frame=0;frame<3;++frame)for(unsigned eye=0;eye<2;++eye) {
        const auto *p=pixels+((frame*2+eye)*32*32+16*32+16)*4;
        require(p[0]==255&&p[1]==0&&p[2]==0&&p[3]==255,"density-map selection changed stereo drawing");
    }
    require(target.density_framebuffer_count()==2,"density-map selection allocated additional framebuffers");
    std::puts("ok: two density maps reuse prepared world framebuffers across asynchronous stereo draws");
}

static void render_overlay(Device &device,bool foveation=false)
{
    FrameRing ring(device);std::unique_ptr<ConstantDensityMap> density;
    if(foveation)density=std::make_unique<ConstantDensityMap>(device,ring,32,32,2);
    TargetInfo info{128,128,2};info.final_layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if(density)info.density_map=density->view();
    Target target(device,info);PipelineCache cache(device,"");
    Renderer background(device,target,cache);Overlay overlay(device,ring,cache);overlay.prepare(target);overlay.prepare(target);
    require(overlay.mesh_buffer_count()==9&&overlay.target_count()==1,"overlay geometry/target resources were not retained");
    StereoScene scene({});std::array<StereoPose,2> eyes;eyes[0].position={-.032f,0,0};eyes[1].position={.032f,0,0};scene.publish(eyes);
    std::array<XoFrame,2> frames{};
    for(int pl=0;pl<2;++pl) {
        auto &f=frames[pl];f.gun_valid=f.show_gun=f.beam=f.dot=1;f.gun_q[3]=1;f.p2=pl;
        f.gun_model=pl?XO_GUN_GHOST_SQUAD:XO_GUN_TP9;f.sight=pl?XO_SIGHT_HOLO:XO_SIGHT_RED_DOT;
        f.gun_p[0]=pl?.16f:-.16f;f.gun_p[1]=-.15f;f.gun_p[2]=-.45f;
        f.muzzle[0]=pl?.025f:-.025f;f.muzzle[1]=-.01f;f.muzzle[2]=-.2f;
        f.end[0]=pl?.25f:-.25f;f.end[1]=.1f;f.end[2]=-1;
    }
    Readback output(device,5*128*128*4*2);Submission last{};
    for(unsigned frame=0;frame<5;++frame) {
        frames[0].flash=frame==0;
        for(auto &f:frames){f.show_gun=frame!=1;f.beam=f.dot=frame!=2;}
        if(frame==3) {
            frames[1].gun_valid=0;auto &gun=frames[0];gun.show_gun=1;gun.sight=XO_SIGHT_HOLO;gun.beam=gun.dot=gun.flash=0;
            gun.gun_p[0]=-.032f;gun.gun_p[1]=-.069f;gun.gun_p[2]=-.4f;
            gun.end[0]=-.047625f;gun.end[1]=.015625f;gun.end[2]=-2;
        }
        if(frame==4) {
            frames[1].gun_valid=0;auto &gun=frames[0];gun.show_gun=0;gun.sight=XO_SIGHT_NONE;gun.beam=1;gun.dot=gun.flash=0;
            gun.muzzle[0]=gun.end[0]=-.032f;gun.muzzle[1]=gun.end[1]=0;gun.muzzle[2]=-.1f;gun.end[2]=-2;
        }
        auto cmd=ring.begin();background.begin(cmd,{.1f,.1f,.1f,.37f});
        overlay.draw(cmd,target,scene.overlay_matrices(),scene.overlay_eyes(),frames,&background);
        VkBufferImageCopy copy{};copy.bufferOffset=frame*128*128*4*2;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,2};copy.imageExtent={128,128,1};
        vkCmdCopyImageToBuffer(cmd,target.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.buffer,1,&copy);
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=output.buffer;b.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr);last=ring.submit();
    }
    ring.wait(last);const auto *pixels=static_cast<const uint8_t *>(output.mapped);
    for(unsigned frame=0;frame<4;++frame) {
        const auto *left=pixels+frame*128*128*4*2,*right=left+128*128*4;
        require(std::memcmp(left,right,128*128*4)!=0,"overlay lost stereo eye separation");
        for(unsigned eye=0;eye<2;++eye) {
            const auto *p=eye?right:left;unsigned changed=0,red=0,blue=0;
            for(unsigned i=0;i<128*128;++i) {
                changed+=p[i*4]!=26||p[i*4+1]!=26||p[i*4+2]!=26;
                red+=p[i*4]>p[i*4+2]+20;blue+=p[i*4+2]>p[i*4]+20;
                if(frame==1)require(p[i*4+3]==94,"laser blending changed the game's alpha");
            }
            require(changed>(frame==1?0u:30u),"overlay gun/beam pixels missing");
            if(frame==1)require(red&&blue,"player laser colors missing from an eye");
        }
    }
    const auto *ads=pixels+3*128*128*4*2;
    const auto *dot=ads+(63*128+63)*4;
    require(dot[0]>150&&dot[0]>dot[1]*2,"holographic sight dot missing inside the left eye glass mask");
    unsigned right_red=0;for(unsigned i=0;i<128*128;++i){const auto *p=ads+128*128*4+i*4;right_red+=p[0]>150&&p[0]>p[1]*2;}
    require(!right_red,"reticle leaked outside the right eye glass mask");
    const auto *edge=pixels+4*128*128*4*2;
    for(unsigned i=0;i<128*128;++i)require(edge[i*4]==26&&edge[i*4+1]==26&&edge[i*4+2]==26,"collinear left-eye laser produced pixels");
    const auto *gradient=edge+128*128*4+(63*128+24)*4;
    require(gradient[0]>30&&gradient[0]<180,"right-eye laser lost its gradient when left-eye geometry was degenerate");
    require(overlay.draw_count()==28,"overlay primitives were replayed per eye or silently dropped");
    require(overlay.render_pass_starts()==0,"overlay failed to reuse the final game render pass");
    std::puts(foveation?"ok: shared gun/laser/sight overlay runs inside a density-map render pass":"ok: persistent gun meshes, stereo lasers/sight masks, preserved alpha and shared final render pass");
}

static void render_panels(Device &device,bool foveation=false)
{
    FrameRing ring(device);std::unique_ptr<ConstantDensityMap> density;
    if(foveation)density=std::make_unique<ConstantDensityMap>(device,ring,32,32,2);
    TargetInfo info{128,128,2};if(density)info.density_map=density->view();info.final_layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Target target(device,info);PipelineCache cache(device,"");Renderer background(device,target,cache);
    Overlay overlay(device,ring,cache);overlay.prepare(target);
    StereoScene scene({});std::array<StereoPose,2> eyes;eyes[0].position={-.032f,0,0};eyes[1].position={.032f,0,0};scene.publish(eyes);
    std::vector<uint8_t> picture(XM_W*XM_H*4);
    for(int y=0;y<XM_H;++y)for(int x=0;x<XM_W;++x){auto *p=picture.data()+(y*XM_W+x)*4;p[0]=y<XM_H/2?255:0;p[2]=y<XM_H/2?0:255;p[3]=128;}
    std::array<PanelDraw,2> panels{};panels[0].visible=true;panels[0].model=identity();panels[0].model[0]=panels[0].model[5]=.5f;panels[0].model[14]=-1;
    Readback output(device,128*128*4*2);
    for(int frame=0;frame<4;++frame){auto cmd=ring.begin();if(!frame)overlay.upload_panel(cmd,0,picture.data(),picture.size());
        background.begin(cmd,{.1f,.1f,.1f,.37f});overlay.draw(cmd,target,scene.overlay_matrices(),scene.overlay_eyes(),{},&background,true,panels);
        if(frame==3){VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,2};copy.imageExtent={128,128,1};
            vkCmdCopyImageToBuffer(cmd,target.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.buffer,1,&copy);
            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=output.buffer;barrier.size=VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);}
        const auto submitted=ring.submit();if(frame==3)ring.wait(submitted);
    }
    const auto *pixels=static_cast<const uint8_t *>(output.mapped);
    for(unsigned eye=0;eye<2;++eye){const auto *p=pixels+eye*128*128*4,*top=p+(40*128+64)*4,*bottom=p+(88*128+64)*4;
        require(top[0]>120&&top[2]<30&&bottom[2]>120&&bottom[0]<30,"panel UV orientation or stereo texture sampling changed");
        require(top[3]==94&&bottom[3]==94,"panel changed game alpha");
    }
    require(overlay.panel_uploads()==1&&overlay.draw_count()==4,"unchanged panel was reuploaded or replayed per eye");
    require(overlay.render_pass_starts()==0,"panel added a render pass");
    LgXrSettings cfg=LG_XR_SETTINGS_DEFAULT;cfg.show_fps=1;cfg.auto_coins=0;
    ControllerSettings controls;controls.auto_coins=false;ControllerInput input(controls);
    SettingsPanel menu(cfg.width,cfg.resolution,"Test",true);ControllerSample sample;sample.valid={1,1};
    const std::vector<float> rates;Submission last{};
    for(unsigned frame=0;frame<45;++frame){menu.update(cfg,scene,input,sample,1000000000+int64_t(frame)*16666667,true,rates);
        auto cmd=ring.begin();menu.upload(overlay,cmd);background.begin(cmd,{0,0,0,1});
        overlay.draw(cmd,target,scene.overlay_matrices(),scene.overlay_eyes(),menu.guns(),&background,true,menu.draws());last=ring.submit();}
    ring.wait(last);
    require(menu.draws()[0].visible&&menu.draws()[1].visible,"native settings/FPS panel did not become visible");
    require(overlay.panel_uploads()==3,"unchanged settings or FPS image was uploaded each frame");
    require(overlay.render_pass_starts()==0,"native UI controller added a final render pass");
    std::puts("ok: retained panel image, top-first UV, stereo alpha blending and shared final pass");
}

static void test_sight_policy()
{
    LgXrSight sight{};float gun[]={0,0},gun2[]={-100,-100},box[]={-.02f,-.02f,.02f,.02f};
    for(unsigned frame=1;frame<=40;++frame)require(!lg_xr_sight_draw(&sight,frame,7,1,box,gun,gun2,0),"stationary HUD was hidden as a sight");
    require(!sight.primary,"stationary icon was learned as the gun sight");
    gun[0]=.2f;box[0]+=.2f;box[2]+=.2f;
    require(!lg_xr_sight_draw(&sight,41,7,1,box,gun,gun2,0)&&sight.primary==7,"moving sight failed to learn");
    require(lg_xr_sight_draw(&sight,42,7,1,box,gun,gun2,0),"recognized primary sight was not hidden");
    require(!lg_xr_sight_draw(&sight,43,7,1,box,gun,gun2,1),"shown sight was hidden");
    require(!lg_xr_sight_draw(&sight,44,8,1,box,gun,gun2,0),"unrecognized texture was hidden");
    gun2[0]=-.3f;gun2[1]=0;box[0]=-.32f;box[2]=-.28f;
    for(unsigned frame=50;frame<79;++frame)require(!lg_xr_sight_draw(&sight,frame,8,1,box,gun,gun2,0),"second sight learned before 30 distinct frames");
    require(lg_xr_sight_draw(&sight,79,8,1,box,gun,gun2,0)&&sight.secondary==8,"second gun sight did not learn");
    box[2]=box[0]+.8f;require(!lg_xr_sight_draw(&sight,80,7,1,box,gun,gun2,0),"large HUD rectangle was hidden");
    std::puts("ok: shared crosshair recognition preserves unlearned and stationary HUD sprites");
}

static void test_settings_panel()
{
    StereoScene scene({});std::array<StereoPose,2> eyes;scene.publish(eyes);
    ControllerSettings controls;controls.auto_coins=false;ControllerInput input(controls);
    LgXrSettings settings=LG_XR_SETTINGS_DEFAULT;settings.auto_coins=0;
    SettingsPanel panel(settings.width,settings.resolution,"Test");ControllerSample sample;sample.valid={1,1};sample.menu=1;
    const std::vector<float> rates{72,90,120};int64_t time=1000000000;
    auto step=[&]{time+=16666667;return panel.update(settings,scene,input,sample,time,true,rates);};
    step();require(panel.open()&&panel.draws()[0].visible,"menu button did not open native panel");
    ControllerSnapshot snapshot;input.consume(snapshot);
    require(!snapshot.valid[0]&&snapshot.ray[0].x==-100000,"open panel leaked game aiming");
    require(panel.guns()[0].gun_valid&&panel.guns()[0].beam,"open panel lost its pointing overlay");
    step();require(panel.open(),"held menu button repeated its toggle");sample.menu=0;step();
    XpPanel placement{};float head[]={0,0,0},orientation[]={0,0,0,1};xp_place(&placement,head,orientation);
    auto point=[&](int row,int hand){
        const float v=float(64+8+row*36+18)/XM_H;
        std::array<float,3> direction;float length=0;
        for(int i=0;i<3;++i){direction[i]=placement.c[i]+placement.up[i]*(1-2*v)*placement.hh;length+=direction[i]*direction[i];}
        length=std::sqrt(length);for(auto &x:direction)x/=length;
        auto &q=sample.pose[hand].orientation;q={direction[1],-direction[0],0,1-direction[2]};
        float norm=0;for(float x:q)norm+=x*x;norm=std::sqrt(norm);for(auto &x:q)x/=norm;
    };
    auto click=[&](int row){sample.buttons.trigger[0]=sample.buttons.trigger[1]=0;step();const int hand=input.gun_hand();point(row,hand);step();
        sample.buttons.trigger[hand]=1;const auto effects=step();input.consume(snapshot);require(!(snapshot.held&CAB_P1_TRIGGER),"menu click fired the game");return effects;};
    require((click(XP_HAND)&XP_HAND_CHANGED)&&input.gun_hand()==0&&settings.left_handed,"native menu hand choice was undone by its selection trigger");
    require((click(XP_SCREEN)&XP_SCREEN_CHANGED)&&scene.settings().screen==4,"screen distance did not update live camera");
    require((click(XP_HUD)&XP_HUD_CHANGED)&&scene.settings().hud_pitch==-10,"HUD pitch did not update live camera");
    require(click(XP_COIN)&XP_SAVE,"coin action missing");require(snapshot.coins==1,"menu coin did not reach game-thread mailbox");
    require(click(XP_TEST)&XP_SAVE,"test action missing");require(snapshot.held&CAB_TEST,"menu test did not reach cabinet");
    require(click(XP_RESET)&XP_SAVE,"reset arm missing");require(click(XP_RESET)&XP_RESTART,"double reset confirmation missing");
    require(click(XP_CLOSE)&XP_SAVE,"close missing");require(!panel.open(),"close did not hide native panel");
    sample.buttons={};step();require(!panel.draws()[0].visible,"closed panel retained draw");
    panel.update(settings,scene,input,sample,time,false,rates);require(!panel.draws()[0].visible&&!panel.guns()[0].gun_valid,"focus loss left UI visible");
    SettingsPanel clocked(settings.width,settings.resolution,"Clock");ControllerInput timed_input(controls);
    settings.show_fps=1;sample={};sample.valid={1,1};
    for(int i=0;i<45;++i)clocked.update(settings,scene,timed_input,sample,1000000000+int64_t(i)*16666667,true,rates,1000000000+int64_t(i)*8333333);
    require(!clocked.draws()[1].visible,"FPS label used simulated input time instead of elapsed readout time");
    for(int i=45;i<80;++i)clocked.update(settings,scene,timed_input,sample,1000000000+int64_t(i)*16666667,true,rates,1000000000+int64_t(i)*8333333);
    require(clocked.draws()[1].visible,"FPS label ignored its elapsed readout clock");
    std::puts("ok: native menu actions, live camera settings, blocked game input and cabinet actions");
}

static void render_stereo(Device &device)
{
    TargetInfo info;
    info.width = info.height = 32;
    info.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Target target(device, info);
    PipelineCache cache(device, "");
    Renderer renderer(device, target, cache);
    RasterState state;
    renderer.prepare(state);
    renderer.prepare(state);
    require(renderer.pipeline_count() == 1, "pipeline key failed to deduplicate state");
    auto dormant=state;
    dormant.src_rgb=VK_BLEND_FACTOR_SRC_ALPHA;dormant.dst_rgb=VK_BLEND_FACTOR_ONE;
    dormant.blend_rgb=VK_BLEND_OP_SUBTRACT;dormant.stencil_compare=VK_COMPARE_OP_NEVER;
    dormant.stencil_pass=VK_STENCIL_OP_REPLACE;
    renderer.prepare(dormant);
    require(renderer.pipeline_count()==1, "disabled blend/stencil parameters created duplicate pipelines");
    auto no_depth=state;no_depth.depth_test=false;
    auto no_depth_other=no_depth;no_depth_other.depth_write=false;no_depth_other.depth_compare=VK_COMPARE_OP_GREATER;
    require(no_depth.key()==no_depth_other.key(), "disabled depth parameters changed the pipeline key");
    dormant.blend=true;require(!(dormant.key()==state.key()), "enabled blend state was lost from the key");
    dormant=state;dormant.stencil=true;require(!(dormant.key()==state.key()), "enabled stencil state was lost from the key");
    dormant=state;dormant.clockwise=!state.clockwise;
    require(!(dormant.key()==state.key()), "front-face shader semantics were lost with culling disabled");
    Readback output(device, 32 * 32 * 4 * 2);
    // Ring is declared last so outstanding work completes before resources die.
    FrameRing ring(device);
    auto cmd = ring.begin();
    renderer.begin(cmd, {0, 0, 0, 1});
    rejected([&] { renderer.prepare(state); }, "pipeline compilation entered draw recording");
    Vertex triangle[] = {
        {{-0.9f, -0.9f, 0, 1}, {1, 0, 0, 1}},
        {{ 0.1f, -0.9f, 0, 1}, {1, 0, 0, 1}},
        {{-0.4f,  0.9f, 0, 1}, {1, 0, 0, 1}}
    };
    Matrices matrices{};
    // The same camera/HUD projection helper used by the GL stereo backend
    // produces direct Vulkan clip transforms. No replay or host GL is needed.
    Matrix game_projection{};game_projection[0]=game_projection[5]=1;
    game_projection[10]=-2;game_projection[11]=-1;game_projection[14]=-3;
    const float tangents[]={1,1,1,1},screen_lens[]={1,1};
    for(unsigned eye=0;eye<2;++eye) {
        auto view=identity();view[12]=eye?1.2f:0;
        LgStereoProjection policy{view.data(),view.data(),tangents,screen_lens,3};
        Matrix effective;
        lg_stereo_eye_projection(game_projection.data(),LG_STEREO_WORLD,&policy,effective.data());
        require(lg_stereo_clip_reprojection(game_projection.data(),effective.data(),matrices.view_projection[eye]),"shared eye reprojection rejected a valid camera");
    }
    dormant=state;dormant.src_rgb=VK_BLEND_FACTOR_SRC_ALPHA;dormant.dst_rgb=VK_BLEND_FACTOR_ONE;
    renderer.draw(ring, triangle, 3, matrices, dormant);
    renderer.end();
    require(renderer.draw_count() == 1, "stereo required more than one draw command");
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2};
    copy.imageExtent = {32, 32, 1};
    vkCmdCopyImageToBuffer(cmd, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = output.buffer; barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    auto submitted = ring.submit();
    ring.wait(submitted);
    auto pixel = [&](unsigned eye, unsigned x, unsigned y, unsigned red) {
        const auto p = static_cast<const unsigned char *>(output.mapped) + ((eye * 32 + y) * 32 + x) * 4;
        require(p[0] == red && p[1] == 0 && p[2] == 0 && p[3] == 255, "Vulkan stereo raster output mismatch");
    };
    pixel(0, 8, 16, 255); pixel(0, 24, 16, 0);
    pixel(1, 8, 16, 0); pixel(1, 24, 16, 255);
    std::puts("ok: one Vulkan draw rendered distinct stereo views with build-time SPIR-V");

    // Exercise guest fixed-function matrix semantics and GL_QUADS through the
    // direct frontend, then verify actual raster output rather than GL calls.
    GuestState guest_state;
    guest_state.matrix_mode(guest::Projection);
    guest_state.ortho(-1, 1, -1, 1, -1, 1);
    guest_state.matrix_mode(guest::Modelview);
    guest_state.translate(0.5f, 0, 0);
    guest_state.push_matrix();
    guest_state.scale(0.5f, 0.5f, 1);
    guest_state.color(1, 0, 0, 1);
    guest_state.begin(guest::Quads);
    guest_state.vertex(-1, -1); guest_state.vertex(1, -1);
    guest_state.vertex(1, 1); guest_state.vertex(-1, 1);
    const auto batch = guest_state.end();
    require(batch && batch->vertices.size() == 4 && batch->indices.size() == 6,
            "guest quad was not lowered to indexed triangles");
    auto mvp = guest_state.modelview_projection();
    std::memcpy(matrices.view_projection[0], mvp.data(), sizeof mvp);
    mvp[12] -= 0.5f;
    std::memcpy(matrices.view_projection[1], mvp.data(), sizeof mvp);
    cmd = ring.begin();
    renderer.begin(cmd, {0, 0, 0, 1});
    renderer.draw_indexed(ring, batch->vertices.data(), static_cast<uint32_t>(batch->vertices.size()),
        batch->indices.data(), static_cast<uint32_t>(batch->indices.size()), matrices, state);
    renderer.end();
    vkCmdCopyImageToBuffer(cmd, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &copy);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    ring.wait(ring.submit());
    pixel(0, 24, 16, 255); pixel(0, 8, 16, 0);
    pixel(1, 16, 16, 255); pixel(1, 28, 16, 0);
    guest_state.pop_matrix();
    require(guest_state.current_matrix()[0] == 1 && guest_state.current_matrix()[12] == 0.5f,
            "guest matrix stack did not restore modelview");
    guest_state.pop_matrix();
    require(guest_state.error() == guest::StackUnderflow && guest_state.error() == guest::NoError,
            "guest matrix error state did not clear");
    guest_state.begin(guest::TriangleStrip);
    guest_state.vertex(-1, -1); guest_state.vertex(1, -1);
    guest_state.vertex(-1, 1); guest_state.vertex(1, 1);
    auto strip = guest_state.end();
    require(strip->indices == std::vector<uint32_t>({0, 1, 2, 2, 1, 3}), "triangle strip winding changed");
    require(renderer.pipeline_count() == 1, "guest state translation recompiled a cached pipeline");
    std::puts("ok: guest matrices, quad/strip assembly and indexed Vulkan stereo rasterization");

    ShaderPack pack(fixture_path("LV_TEST_SHADER_DIR", LV_TEST_SHADER_DIR, "fixture.lvsp"));
    auto vertex_source = source(fixture_path("LV_TEST_SOURCE_DIR", LV_TEST_SOURCE_DIR, "legacy.vert"));
    require(!vertex_source.empty() && vertex_source.back() == '\n', "fixture must exercise EOF newline normalization");
    vertex_source.pop_back();
    const auto vs_hash = shader_source_digest(vertex_source);
    const auto fs_hash = shader_source_digest(source(fixture_path("LV_TEST_SOURCE_DIR", LV_TEST_SOURCE_DIR, "legacy.frag")));
    const auto record = pack.find(vs_hash, fs_hash, {});
    require(record && pack.programs().size() == 1, "offline shader archive lookup failed");
    require(!pack.find(SourceDigest{}, fs_hash, {}), "unknown shader source silently accepted");
    const auto &vs = record->vertex, &fs = record->fragment;
    const auto uniform_layout = std::find_if(record->uniforms.begin(), record->uniforms.end(),
                                             [](const auto &b) { return b.set == 1 && b.binding == 0; });
    require(uniform_layout != record->uniforms.end() && uniform_layout->bytes == 16,
            "shader archive lost the reflected guest uniform layout");
    UniformData guest_uniforms(*uniform_layout);
    const auto tint_location = guest_uniforms.find("guestTint");
    require(tint_location >= 0 && guest_uniforms.find("not_a_uniform") == -1, "uniform name lookup changed");
    const auto legacy_layout = std::find_if(record->uniforms.begin(),record->uniforms.end(),[](const auto &b){return b.set==0;});
    require(legacy_layout!=record->uniforms.end(), "missing legacy uniform block");
    UniformData legacy_data(*legacy_layout);
    UniformSet legacy_uniforms(device,ring,{{0,legacy_layout->bytes}});
    auto bind_legacy = [&](Renderer &r) {
        auto offset=legacy_uniforms.upload(0,legacy_data.bytes().data(),legacy_data.bytes().size());
        auto ds=legacy_uniforms.current_set();r.bind_descriptors(0,1,&ds,1,&offset);
    };
    UniformSet uniforms(device, ring, {{0, 16}});
    TextureInfo texture_info{2, 2, 2};
    if (device.capabilities.image_view_format_swizzle) {
        texture_info.format = VK_FORMAT_R8_UNORM;
        texture_info.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                   VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE};
    }
    Texture legacy_texture(device, texture_info);
    Texture texture(device, {2, 2, 2});
    Sampler sampler(device);
    SampledSet material(device, {{0, texture.array_view(), sampler.handle()}});
    SampledSet legacy_material(device, {{0, legacy_texture.array_view(), sampler.handle()}});
    ProgramSource legacy{vs.data(), fs.data(), vs.size() * 4, fs.size() * 4,
        {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}},
        {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, position)},
         {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}}, {legacy_uniforms.layout(), uniforms.layout(), material.layout()}};
    Renderer converted(device, target, cache, legacy);
    converted.prepare(state);
    for (auto &m : matrices.view_projection) {
        std::fill(m, m + 16, 0);
        for (unsigned i = 0; i < 4; i++) m[i * 5] = 1;
    }
    matrices.view_projection[1][12] = 0.8f;
    cmd = ring.begin();
    const uint32_t white[] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    const bool luminance = texture_info.format == VK_FORMAT_R8_UNORM;
    legacy_texture.upload(cmd, ring, 0, 0, white, luminance ? 4 : sizeof white);
    legacy_texture.upload(cmd, ring, 1, 0, white, luminance ? 1 : 4);
    converted.begin(cmd, {0, 0, 0, 1});
    bind_legacy(converted);
    auto sampled = legacy_material.handle();
    converted.bind_descriptors(2, 1, &sampled);
    const float tint[] = {1, 1, 1, 1};
    guest_uniforms.write(static_cast<uint32_t>(tint_location), tint, sizeof tint);
    const auto version = guest_uniforms.version();
    guest_uniforms.write(static_cast<uint32_t>(tint_location), tint, sizeof tint);
    require(guest_uniforms.version() == version, "unchanged uniform marked dirty");
    auto offset = uniforms.upload(0, guest_uniforms.bytes().data(), guest_uniforms.bytes().size());
    auto descriptors = uniforms.current_set();
    converted.bind_descriptors(1, 1, &descriptors, 1, &offset);
    converted.draw(ring, triangle, 3, matrices, state);
    converted.end();
    vkCmdCopyImageToBuffer(cmd, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &copy);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    ring.wait(ring.submit());
    pixel(0, 8, 16, 255); pixel(0, 24, 16, 0);
    pixel(1, 8, 16, 0); pixel(1, 24, 16, 255);
    std::puts("ok: converted GLSL 1.20 shaders preserve front-face color and multiview raster output");

    // Game loading uses packed BGRA8888_REV and RGB565. Sample the original
    // guest byte order directly; checking channels catches a mistaken swizzle.
    for (auto format : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_R4G4B4A4_UNORM_PACK16}) {
        TextureInfo packed_info{1,1,1,1,format};
        const bool is_4444=format==VK_FORMAT_R4G4B4A4_UNORM_PACK16;
        if (is_4444) packed_info.components={VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_R};
        Texture packed(device, packed_info);
        SampledSet packed_material(device, {{0,packed.array_view(),sampler.handle()}});
        cmd=ring.begin();
        const uint8_t bgra[]={0,0,255,255}, rgb565[]={0,248}, bgra4444[]={0,255};
        const bool is_565=format==VK_FORMAT_R5G6B5_UNORM_PACK16;
        packed.upload(cmd,ring,0,0,is_565?rgb565:is_4444?bgra4444:bgra,(is_565||is_4444)?2:4);
        converted.begin(cmd,{0,0,0,1});bind_legacy(converted);
        auto ds=packed_material.handle();converted.bind_descriptors(2,1,&ds);
        auto u=uniforms.upload(0,guest_uniforms.bytes().data(),guest_uniforms.bytes().size());
        ds=uniforms.current_set();converted.bind_descriptors(1,1,&ds,1,&u);
        auto white_triangle=std::vector<Vertex>(std::begin(triangle),std::end(triangle));
        for (auto &vertex:white_triangle) for (auto &channel:vertex.color) channel=1;
        converted.draw(ring,white_triangle.data(),3,matrices,state);converted.end();
        vkCmdCopyImageToBuffer(cmd,target.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.buffer,1,&copy);
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                             0,0,nullptr,1,&barrier,0,nullptr);
        ring.wait(ring.submit());
        const auto rgba=static_cast<const uint8_t *>(output.mapped)+(16*32+8)*4;
        require(rgba[0]==255 && rgba[1]==0 && rgba[2]==0 && rgba[3]==255,
                "packed guest texture channels changed");
    }
    std::puts("ok: native BGRA8888, RGB565 and swizzled BGRA4444 guest storage samples without pixel conversion");

    // Copy both rendered eye images into an array texture, then sample that
    // texture in another multiview pass. A layer-0-only copy or sampler would
    // put the left eye's triangle in the right eye and fail these pixel checks.
    Texture eye_copy(device,{32,32,1,2});
    SampledSet eye_material(device,{{0,eye_copy.array_view(),sampler.handle()}});
    require(eye_copy.array_view()==eye_copy.array_view(),"array texture view was recreated");
    cmd=ring.begin();
    std::vector<uint8_t> old_pixels(32*32*4,0);
    eye_copy.upload(cmd,ring,0,0,old_pixels.data(),old_pixels.size()); // mixed destination layouts
    eye_copy.copy_from(cmd,target,0,0,0,0,0,32,32,0,0,2);
    rejected([&]{eye_copy.copy_from(cmd,target,0,0,0,0,0,32,32,1,0,2);},"invalid stereo source layers accepted");
    rejected([&]{eye_copy.copy_from(cmd,target,0,0,0,0,0,32,32,0,1,2);},"invalid stereo destination layers accepted");
    converted.begin(cmd,{0,0,0,1});bind_legacy(converted);
    auto eyes_ds=eye_material.handle();converted.bind_descriptors(2,1,&eyes_ds);
    auto eyes_offset=uniforms.upload(0,guest_uniforms.bytes().data(),guest_uniforms.bytes().size());
    eyes_ds=uniforms.current_set();converted.bind_descriptors(1,1,&eyes_ds,1,&eyes_offset);
    Matrices full_screen{};for(auto &m:full_screen.view_projection)for(unsigned i=0;i<4;++i)m[i*5]=1;
    const Vertex quad[]={{{-1,-1,0,1},{1,1,1,1}},{{1,-1,0,1},{1,1,1,1}},{{1,1,0,1},{1,1,1,1}},
                         {{-1,-1,0,1},{1,1,1,1}},{{1,1,0,1},{1,1,1,1}},{{-1,1,0,1},{1,1,1,1}}};
    converted.draw(ring,quad,6,full_screen,state);converted.end();
    vkCmdCopyImageToBuffer(cmd,target.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.buffer,1,&copy);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
    ring.wait(ring.submit());
    pixel(0,8,16,255);pixel(0,24,16,0);pixel(1,8,16,0);pixel(1,24,16,255);
    std::puts("ok: both eyes survive a GPU framebuffer copy and converted postprocessing sample");


    // Different draws and frames must retain independent uniform snapshots even
    // with several frames in flight and descriptor sets recycled by slot.
    constexpr unsigned frames = 12, image_bytes = 32 * 32 * 4 * 2;
    Readback history(device, frames * image_bytes);
    rejected([&] { uniforms.current_set(); }, "uniform set accepted outside frame");
    Submission last{};
    for (unsigned frame = 0; frame < frames; ++frame) {
        cmd = ring.begin();
        converted.begin(cmd, {0, 0, 0, 1});
    bind_legacy(converted);
        converted.bind_descriptors(2, 1, &sampled);
        const float color[] = {float(frame & 1), 1, 1, 1};
        rejected([&] { uniforms.upload(0, color, 12); }, "truncated uniform block accepted");
        guest_uniforms.write(static_cast<uint32_t>(tint_location), color, sizeof color);
        offset = uniforms.upload(0, guest_uniforms.bytes().data(), guest_uniforms.bytes().size());
        require(offset % device.capabilities.properties.limits.minUniformBufferOffsetAlignment == 0,
                "uniform upload violates device alignment");
        descriptors = uniforms.current_set();
        converted.bind_descriptors(1, 1, &descriptors, 1, &offset);
        converted.draw(ring, triangle, 3, matrices, state);
        // Change the state after the draw: its previous snapshot must survive.
        const float black[] = {0, 0, 0, 1};
        uniforms.upload(0, black, sizeof black);
        converted.end();
        copy.bufferOffset = frame * image_bytes;
        vkCmdCopyImageToBuffer(cmd, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history.buffer, 1, &copy);
        barrier.buffer = history.buffer; barrier.offset = copy.bufferOffset; barrier.size = image_bytes;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
        last = ring.submit();
    }
    ring.wait(last);
    for (unsigned frame = 0; frame < frames; ++frame) {
        const auto bytes = static_cast<const unsigned char *>(history.mapped) + frame * image_bytes;
        const auto left = bytes + (16 * 32 + 8) * 4;
        const auto right = bytes + ((32 + 16) * 32 + 24) * 4;
        require(left[0] == (frame & 1) * 255 && right[0] == left[0] && left[3] == 255 && right[3] == 255,
                "buffered guest uniform state corrupted across in-flight frames");
    }
    require(converted.pipeline_count() == 1, "uniform changes created new pipelines");
    std::puts("ok: converted shader uniforms retain snapshots across 12 asynchronously submitted frames");

    // Feed native formats directly: separate float positions and normalized
    // byte colors, with an immutable uint16 index buffer. Upload geometry once.
    ProgramSource separate = legacy;
    separate.bindings = {{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 4, VK_VERTEX_INPUT_RATE_VERTEX}};
    separate.attributes = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {3, 1, VK_FORMAT_R8G8B8A8_UNORM, 0}};
    std::unique_ptr<Renderer> textured, constant_color;
    const float positions[][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1}};
    const uint32_t colors[] = {0xff808080, 0xff808080, 0xff808080, 0xff808080};
    const uint16_t elements[] = {0, 1, 2, 0, 2, 3};
    GeometryBuffer positions_gpu(device, sizeof positions), colors_gpu(device, sizeof colors), indices_gpu(device, sizeof elements);
    GuestArrays arrays;
    arrays.inputs({0, 3});
    arrays.bind_array_buffer(7);
    arrays.pointer(0, 4, 0x1406, false, 0, 0);
    arrays.enable(0, true);
    arrays.bind_array_buffer(8);
    arrays.pointer(3, 4, 0x1401, true, 0, 0);
    arrays.enable(3, true);
    arrays.current(3, {128.0f / 255, 128.0f / 255, 128.0f / 255, 1});
    arrays.bind_array_buffer(9); // cannot change the buffer captured by pointer()
    const GuestMemoryView memory{colors, sizeof colors, 0xfffffff0};
    require(memory.read(0xfffffff0, 16) == colors, "high guest client address was truncated");
    rejected([&] { memory.read(0xfffffffc, 8); }, "client array crossed the 32-bit address limit");
    const BufferLookup buffers = [&](uint32_t name) -> GuestBufferView {
        if (name == 7) return {positions_gpu.handle(), sizeof positions};
        if (name == 8) return {colors_gpu.handle(), sizeof colors};
        throw std::runtime_error("vertex pointer did not capture its array-buffer binding");
    };
    IndexStream elements_gpu{indices_gpu.handle(), 0, VK_INDEX_TYPE_UINT16};
    SamplerInfo mip_info;
    mip_info.min_lod = mip_info.max_lod = 1;
    Sampler mip_sampler(device, mip_info);
    SampledSet mip_material(device, {{0, texture.array_view(), mip_sampler.handle()}});
    const uint8_t texels[] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
    const uint8_t mip[] = {255,255,0,255};
    for (auto &m : matrices.view_projection) m[12] = 0;
    for (unsigned frame = 0; frame < frames; ++frame) {
        cmd = ring.begin();
        if (!frame) {
            positions_gpu.upload(cmd, ring, positions, sizeof positions);
            colors_gpu.upload(cmd, ring, colors, sizeof colors);
            indices_gpu.upload(cmd, ring, elements, sizeof elements);
            texture.upload(cmd, ring, 0, 0, texels, sizeof texels);
            texture.upload(cmd, ring, 1, 0, mip, sizeof mip);
        }
        if (!frame) {
            const auto &offset_plan = arrays.prepare(device, ring, 1, 3, memory, buffers);
            require(offset_plan.streams[0].offset == 16 && offset_plan.streams[1].offset == 4,
                    "guest DrawArrays first vertex offset changed");
        }
        arrays.enable(3, frame % 3 != 2);
        if (frame % 3 == 1) {
            arrays.bind_array_buffer(0);
            arrays.pointer(3, 4, 0x1401, true, 0, 0xfffffff0);
            rejected([&] { arrays.prepare(device, ring, 1, 4, memory, buffers); },
                     "client array overflow across the 32-bit window accepted");
        } else {
            arrays.bind_array_buffer(8);
            arrays.pointer(3, 4, 0x1401, true, 0, 0);
        }
        const auto &plan = arrays.prepare(device, ring, 0, 4, memory, buffers);
        require(plan.streams[0].buffer == positions_gpu.handle() && plan.streams[0].offset == 0,
                "guest static geometry was copied instead of bound directly");
        if (!frame) {
            separate.bindings = plan.bindings; separate.attributes = plan.attributes;
            textured = std::make_unique<Renderer>(device, target, cache, separate);
            textured->prepare(state);
            separate.bindings[1].stride = 0;
            separate.attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            constant_color = std::make_unique<Renderer>(device, target, cache, separate);
            constant_color->prepare(state);
        }
        auto &draw = frame % 3 == 2 ? *constant_color : *textured;
        draw.begin(cmd, {0, 0, 0, 1});
        bind_legacy(draw);
        offset = uniforms.upload(0, tint, sizeof tint);
        descriptors = uniforms.current_set();
        draw.bind_descriptors(1, 1, &descriptors, 1, &offset);
        sampled = (frame & 1) ? mip_material.handle() : material.handle();
        draw.bind_descriptors(2, 1, &sampled);
        draw.draw_buffers(plan.streams.data(), static_cast<uint32_t>(plan.streams.size()), 6, matrices, state, &elements_gpu);
        draw.end();
        copy.bufferOffset = frame * image_bytes;
        vkCmdCopyImageToBuffer(cmd, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history.buffer, 1, &copy);
        barrier.offset = copy.bufferOffset;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
        last = ring.submit();
    }
    ring.wait(last);
    for (unsigned frame = 0; frame < frames; ++frame) {
        const auto bytes = static_cast<const uint8_t *>(history.mapped) + frame * image_bytes;
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto base = bytes + eye * image_bytes / 2;
            auto sample = [&](unsigned x, unsigned y, const uint8_t *expected) {
                const auto p = base + (y * 32 + x) * 4;
                const auto source = (frame & 1) ? mip : expected;
                const uint8_t modulated[] = {uint8_t(source[0] ? 128 : 0), uint8_t(source[1] ? 128 : 0),
                                             uint8_t(source[2] ? 128 : 0), 255};
                require(std::memcmp(p, modulated, 4) == 0,
                        "texture orientation/mip, normalized colors or GPU vertex/index buffer raster output changed");
            };
            sample(8, 8, texels + 8); sample(24, 8, texels + 12);
            sample(8, 24, texels); sample(24, 24, texels + 4);
        }
    }
    require(textured->draw_count() + constant_color->draw_count() == frames &&
            textured->pipeline_count() == 1 && constant_color->pipeline_count() == 1,
            "static textured stereo geometry was replayed or recompiled");
    std::puts("ok: guest VBO/client/current arrays render directly with textures, uint16 indices and mip descriptors");
}


static void render_copy(Device &device)
{
    TargetInfo info{16,16,1}; info.guest_bottom_origin=true;info.final_layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Target target(device,info);PipelineCache cache(device,"");Renderer renderer(device,target,cache);
    RasterState raster;renderer.prepare(raster);
    // Regression: the game's RGBA framebuffer-copy texture is allocated with
    // LUMINANCE upload bytes. Storage must still accept subsequent full color.
    std::vector<uint8_t> narrow(16*16,0);
    require(guest::canonical_color_storage(guest::RGBA,guest::Luminance,guest::UnsignedByte,16,16,narrow)==guest::RGBA && narrow.size()==16*16*4,
            "RGBA internal storage inherited luminance-only upload channels");
    std::vector<uint8_t> alpha{32};
    guest::canonical_color_storage(guest::RGB,guest::Alpha,guest::UnsignedByte,1,1,alpha);
    require(alpha==std::vector<uint8_t>({0,0,0,255}), "RGB internal storage retained upload alpha");
    Texture color(device,{16,16}),depth(device,{16,16,1,1,Target::depth_format});
    Readback output(device,16*16*4*3);FrameRing ring(device);
    Matrices matrices{};for(auto &m:matrices.view_projection)for(unsigned i=0;i<4;++i)m[i*5]=1;
    Vertex triangle[]={{{-1,-1,0,1},{1,0,0,1}},{{0,-1,0,1},{1,0,0,1}},{{-.5f,1,0,1},{1,0,0,1}}};
    auto cmd=ring.begin();renderer.begin(cmd,{0,0,1,1});renderer.draw(ring,triangle,3,matrices,raster);renderer.end();
    // Resuming a compatible pass must retain both color and depth. A triangle
    // behind the previous one cannot overwrite its red pixels.
    for(auto &v:triangle){v.position[2]=.8f;v.color[0]=0;v.color[1]=1;}
    renderer.begin(cmd,{0,1,0,1},true);renderer.draw(ring,triangle,3,matrices,raster);renderer.end();
    color.copy_from(cmd,target,0,0,0,0,0,16,16);depth.copy_from(cmd,target,0,0,0,0,0,16,16);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={16,16,1};
    vkCmdCopyImageToBuffer(cmd,target.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.buffer,1,&copy);
    for(unsigned i=0;i<2;++i) {
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};image.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        image.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;image.image=i?depth.image():color.image();
        image.subresourceRange={VkImageAspectFlags(i?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
        copy.bufferOffset=(i+1)*16*16*4;copy.imageSubresource.aspectMask=i?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdCopyImageToBuffer(cmd,image.image,image.newLayout,output.buffer,1,&copy);
    }
    VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    ready.srcQueueFamilyIndex=ready.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ready.buffer=output.buffer;ready.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&ready,0,nullptr);
    ring.wait(ring.submit());auto bytes=static_cast<const uint8_t *>(output.mapped);
    require(!std::memcmp(bytes,bytes+16*16*4,16*16*4),"pbuffer color copy changed raster pixels");
    auto pixel=bytes+(2*16+4)*4;require(pixel[0]==255&&pixel[1]==0&&pixel[2]==0,"preserved pbuffer color/depth or bottom-origin viewport changed");
    pixel=bytes+(14*16+4)*4;require(pixel[0]==0&&pixel[2]==255,"guest pbuffer rows were flipped");
    auto z=reinterpret_cast<const float *>(bytes+16*16*4*2);
    require(z[2*16+4]==.5f&&z[14*16+4]==1,"GPU pbuffer depth copy changed retained depth");
    std::puts("ok: preserving pbuffer passes, guest bottom-origin viewport and GPU color/depth copies");
}

static void render_stereo_depth_copy(Device &device)
{
    TargetInfo info{16,16,2};info.guest_bottom_origin=true;
    Target target(device,info);PipelineCache cache(device,"");Renderer renderer(device,target,cache);
    RasterState raster;renderer.prepare(raster);
    Texture depth(device,{16,16,1,2,Target::depth_format});
    Texture routed(device,{16,16,1,2,Target::depth_format});
    Readback output(device,16*16*4*6);FrameRing ring(device);
    auto cmd=ring.begin();
    std::vector<float> old_depth(16*16,.9f);
    depth.upload(cmd,ring,0,0,old_depth.data(),old_depth.size()*4);
    routed.upload(cmd,ring,0,1,old_depth.data(),old_depth.size()*4);
    Matrices matrices{};for(auto &m:matrices.view_projection)for(unsigned i=0;i<4;++i)m[i*5]=1;
    matrices.view_projection[1][14]=.5f;
    Vertex triangle[]={{{-1,-1,0,1},{1,0,0,1}},{{0,-1,0,1},{1,0,0,1}},{{-.5f,1,0,1},{1,0,0,1}}};
    renderer.begin(cmd,{0,0,0,1});renderer.draw(ring,triangle,3,matrices,raster);renderer.end();
    depth.copy_from(cmd,target,0,0,0,0,0,16,16,0,0,2);
    routed.copy_from(cmd,target,0,0,0,0,0,16,16,1,0,1);
    const VkImage images[]={target.depth_image(),depth.image(),routed.image()};
    VkImageMemoryBarrier barriers[3]{};
    for(unsigned i=0;i<3;++i) {
        auto &b=barriers[i];b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.image=images[i];b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.oldLayout=i?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        b.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,2};
    }
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,3,barriers);
    for(unsigned i=0;i<3;++i) {
        VkBufferImageCopy copy{};copy.bufferOffset=i*16*16*4*2;
        copy.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,2};copy.imageExtent={16,16,1};
        vkCmdCopyImageToBuffer(cmd,images[i],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.buffer,1,&copy);
    }
    VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    ready.srcQueueFamilyIndex=ready.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ready.buffer=output.buffer;ready.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&ready,0,nullptr);
    ring.wait(ring.submit());const auto *z=static_cast<const float *>(output.mapped);
    require(!std::memcmp(z,z+512,512*4),"stereo GPU depth copy lost an eye");
    require(z[2*16+4]==.5f && z[256+2*16+4]==.75f,"stereo depth fixture lacks distinct eye depths");
    require(!std::memcmp(z+256,z+1024,256*4),"explicit source eye was not routed to destination eye");
    for(unsigned i=0;i<256;++i) require(z[1280+i]==.9f,"copy overwrote an untouched destination eye");
    std::puts("ok: stereo GPU depth copies preserve each eye, mixed layouts and explicit layer routing");
}

static void test_controller_input()
{
    StereoSettings settings;settings.head_locked=true;StereoScene scene(settings);
    std::array<StereoPose,2> eyes;eyes[0].position={-.032f,0,0};eyes[1].position={.032f,0,0};scene.publish(eyes);
    StereoPose controller;controller.position={0,0,-.3f};StereoRay ray;
    require(scene.aim(controller,1024,768,true,15,ray)&&ray.x==512&&ray.y==384,"center aim did not use cabinet input dimensions");
    const float yaw=.4f;const std::array<float,4> q{0,std::sin(yaw/2),0,std::cos(yaw/2)};
    for(auto &eye:eyes){auto old=eye.position;lg_stereo_rotate(q.data(),old.data(),eye.position.data());eye.orientation=q;}
    auto old=controller.position;lg_stereo_rotate(q.data(),old.data(),controller.position.data());controller.orientation=q;scene.publish(eyes);
    require(scene.aim(controller,1024,768,true,15,ray)&&std::abs(ray.x-512)<=1&&std::abs(ray.y-384)<=1,"head-locked HUD aim moved after a common head/controller turn");
    ControllerSettings cfg;cfg.width=1024;cfg.height=768;cfg.dual=true;cfg.aim_on_hud=true;
    ControllerInput input(cfg);ControllerSnapshot snapshot;
    require(!input.consume(snapshot),"controller input invented an initial snapshot");
    ControllerSample sample;sample.valid={1,1};sample.pose[0]=sample.pose[1]=controller;
    sample.pose[0].position[0]-=.1f;sample.pose[1].position[0]+=.1f;
    sample.buttons.trigger[0]=1;sample.buttons.trigger[1]=1;
    const auto haptics=input.update(scene,sample,1000000000,true);
    require(input.overlay_frames()[0].gun_valid&&input.overlay_frames()[1].gun_valid&&input.overlay_frames()[1].p2,
            "controller poses did not populate separate overlay guns");
    require(haptics[0].milliseconds==35&&haptics[1].milliseconds==35,"dual guns did not receive independent recoil haptics");
    require(input.consume(snapshot)&&snapshot.dual&&snapshot.valid[0]&&snapshot.valid[1]&&snapshot.coins==1,"dual input/rays or P2 join credit were lost");
    require(snapshot.ray[0].origin!=snapshot.ray[1].origin,"both player shot hooks received the same gun ray");
    require((snapshot.held&((1<<4)|(1<<8)))==((1<<4)|(1<<8)),"dual triggers were not mapped to both cabinet players");
    require(input.consume(snapshot)&&snapshot.coins==0,"input consumption duplicated a credit");
    sample.buttons={};input.update(scene,sample,1010000000,true);
    sample.buttons.trigger[1]=1;input.update(scene,sample,1020000000,true);
    sample.buttons.trigger[1]=0;input.update(scene,sample,1030000000,true);
    require(input.consume(snapshot)&&(snapshot.held&(1<<4)),"mailbox lost a trigger pulse between game-thread reads");
    require(input.consume(snapshot)&&!(snapshot.held&(1<<4)),"mailbox replayed an already consumed trigger pulse");
    sample.buttons.trigger[1]=1;input.update(scene,sample,1031000000,true);
    sample.buttons.blocked=1;input.update(scene,sample,1032000000,true);
    require(input.consume(snapshot)&&!(snapshot.held&((1<<4)|(1<<8)))&&!snapshot.valid[0]&&!snapshot.valid[1],
            "menu opening leaked buffered fire or game shot rays");
    require(input.overlay_frames()[0].gun_valid&&input.overlay_frames()[1].gun_valid,"blocked game aim removed pointing guns");
    sample.buttons.blocked=0;
    input.update(scene,sample,1040000000,false);
    require(!input.overlay_frames()[0].gun_valid&&!input.overlay_frames()[1].gun_valid,"focus loss left stale overlay guns");
    require(input.consume(snapshot)&&snapshot.held==0&&!snapshot.valid[0]&&!snapshot.valid[1],"focus loss retained firing or old gun rays");
    scene.invalidate();sample.buttons.trigger[1]=1;input.update(scene,sample,1050000000,true);
    require(input.consume(snapshot)&&snapshot.held==0&&!snapshot.valid[0],"invalid tracking published active gun input");
    std::puts("ok: shared controller policy, head-locked HUD aim, dual shot rays and game-thread input delivery");
}

static void test_live_stereo_scene()
{
    StereoSettings settings;settings.head_locked=true;settings.hud_lens=1.8107f;
    StereoScene scene(settings);
    std::array<StereoPose,2> eyes;eyes[0].position={.032f,0,0};eyes[1].position={-.032f,0,0};
    scene.publish(eyes);
    auto camera=identity();camera[0]=1.954f;camera[5]=2.6053f;camera[10]=-2;camera[11]=-1;camera[14]=-3;camera[15]=0;
    auto hud_camera=camera;hud_camera[0]=1.8107f;
    require(scene.world_camera(camera)&&!scene.world_camera(hud_camera),"profile HUD lens was treated as world geometry");
    const auto world=scene.transforms(camera,LG_STEREO_WORLD);
    for(unsigned i=0;i<1000;++i) scene.transforms(camera,LG_STEREO_WORLD);
    require(scene.transform_builds()==1,"unchanged draws rebuilt stereo camera transforms");
    const auto hud=scene.transforms(hud_camera,LG_STEREO_HUD);
    scene.transforms(camera,LG_STEREO_HUD);
    require(scene.transform_builds()==2,"HUD transform unnecessarily depends on guest projection");
    const float yaw=.4f;
    const std::array<float,4> q{0,std::sin(yaw/2),0,std::cos(yaw/2)};
    for(auto &eye:eyes) {auto old=eye.position;lg_stereo_rotate(q.data(),old.data(),eye.position.data());eye.orientation=q;}
    scene.publish(eyes);
    const auto turned_world=scene.transforms(camera,LG_STEREO_WORLD),turned_hud=scene.transforms(hud_camera,LG_STEREO_HUD);
    require(std::fabs(turned_world.view_projection[0][0]-world.view_projection[0][0])>.01f,"head turn did not change the world projection");
    for(unsigned eye=0;eye<2;++eye)for(unsigned i=0;i<16;++i)
        require(std::fabs(turned_hud.view_projection[eye][i]-hud.view_projection[eye][i])<1e-5f,"head-locked HUD followed the world rotation");
    auto another=camera;another[0]=2.3f;another[5]=3.0667f;
    scene.observe(camera,LG_STEREO_WORLD);scene.observe(camera,LG_STEREO_WORLD);scene.observe(another,LG_STEREO_WORLD);scene.finish_frame();
    require(scene.screen_lens()[0]==camera[0]&&scene.screen_lens()[1]==camera[5],"HUD screen did not use the dominant world camera");
    auto zoom=camera;zoom[0]=9;
    for(unsigned i=0;i<6;++i) {require(!scene.zoomed(zoom),"transient camera zoom was treated as a scope");scene.observe(zoom,LG_STEREO_WORLD);scene.finish_frame();}
    require(scene.zoomed(zoom),"held scope lens was not recognized");
    LgStereoDrawFacts f{};f.perspective=1;
    require(lg_stereo_classify_draw(&f)==LG_STEREO_WORLD,"world class changed");
    f.menu=1;require(lg_stereo_classify_draw(&f)==LG_STEREO_HUD,"menu model escaped HUD plane");
    f.offscreen=1;require(lg_stereo_classify_draw(&f)==LG_STEREO_PASS,"offscreen sprite camera was reprojected");
    f={};f.texture_view=1;f.covers_viewport=1;f.multiview=1;f.fills_screen=1;
    require(lg_stereo_classify_draw(&f)==LG_STEREO_WORLD,"completed world pass was flattened onto HUD");
    f.covers_viewport=0;f.fills_screen=0;
    require(lg_stereo_classify_draw(&f)==LG_STEREO_HUD,"cropped view sprite escaped HUD plane");
    f.texture_view=0;f.fills_screen=1;
    require(lg_stereo_classify_draw(&f)==LG_STEREO_FILL,"full-screen fade was put on a floating plane");
    LgStereoTargetSize size{1024,768,800,600,2064,2280};float sx,sy;
    lg_stereo_target_scale(&size,1024,768,0,&sx,&sy);
    require(sx==2064.f/1024 && sy==2280.f/768,"full picture did not use the configured eye size");
    lg_stereo_target_scale(&size,1024,1024,0,&sx,&sy);
    require(sx==1&&sy==1,"shadow map was expanded beyond the 1024-pixel cap");
    lg_stereo_target_scale(&size,256,256,0,&sx,&sy);
    require(sx==2&&sy==2,"small offscreen target did not retain bounded GL scaling");
    lg_stereo_target_scale(&size,800,600,0,&sx,&sy);
    require(sx==2064.f/800 && sy==2280.f/600,"separate world target was not eye-sized");
    GuestArrays arrays;arrays.bind_array_buffer(7);arrays.pointer(0,3,0x1406,false,16,16);arrays.enable(0,true);arrays.bind_array_buffer(99);
    const float positions[]={0,0,0,0, 1,2,3,1, 4,5,6,1};std::array<float,4> v;
    require(arrays.position(1,{nullptr,0},[&](uint32_t id){require(id==7,"HUD bounds lost the pointer's captured VBO");return GuestMemoryView{positions,sizeof positions};},v) && v==std::array<float,4>{4,5,6,1},"HUD position shadow read changed stride/offset semantics");
    rejected([&]{arrays.position(2,{nullptr,0},[&](uint32_t){return GuestMemoryView{positions,sizeof positions};},v);},"HUD bounds escaped the CPU buffer shadow");
    scene.invalidate();const auto inactive=scene.transforms(camera,LG_STEREO_WORLD);
    require(inactive.view_projection[0][0]==1 && inactive.view_projection[0][12]==0,"invalid tracking reused an old camera");
    std::puts("ok: live stereo pose publication, HUD/world classification, dominant lens and cached transforms");
}

static void test_shared_projection()
{
    auto p=identity(),view=identity(),head=identity();
    p[10]=-2;p[11]=-1;p[14]=-3;p[15]=0;
    const float tan[]={1,1,1,1},lens[]={1,1};
    LgStereoProjection eye{view.data(),head.data(),tan,lens,3};
    Matrix effective{},reprojection{};
    lg_stereo_eye_projection(p.data(),LG_STEREO_PASS,&eye,effective.data());
    require(effective==p,"offscreen pass changed guest projection");
    lg_stereo_eye_projection(p.data(),LG_STEREO_WORLD,&eye,effective.data());
    require(effective==p,"matching eye camera changed finite guest projection");
    require(lg_stereo_clip_reprojection(p.data(),effective.data(),reprojection.data()),"finite guest clip inverse failed");
    const auto unit=identity();
    for(unsigned i=0;i<16;++i) require(std::fabs(reprojection[i]-unit[i])<1e-6f,"matching camera clip transform is not identity");
    view[12]=1;
    auto ortho=identity();
    lg_stereo_eye_projection(ortho.data(),LG_STEREO_HUD,&eye,effective.data());
    // A head-locked screen stays centred despite a translated world view.
    require(effective[12]==0 && effective[15]==3 && std::fabs(effective[0]-3)<1e-6f,"HUD screen no longer follows the selected head view");
    eye.hud_view=view.data();
    lg_stereo_eye_projection(ortho.data(),LG_STEREO_HUD,&eye,effective.data());
    require(effective[12]==1,"world-locked HUD did not use world view");
    lg_stereo_eye_projection(p.data(),LG_STEREO_FILL,&eye,effective.data());
    for(unsigned c=0;c<4;++c) require(effective[4*c+2]==-.999f*effective[4*c+3],"fill did not cover the eye at the foreground plane");
    // Infinite-far matrices are invertible even when their float Z scale is -1.
    p[10]=-1;p[14]=-.2f;
    lg_stereo_eye_projection(p.data(),LG_STEREO_WORLD,&eye,effective.data());
    for(float v:effective) require(std::isfinite(v),"infinite far projection generated nonfinite coefficients");
    require(effective[10]==-1 && effective[14]==-.2f,"infinite guest depth coefficients were lost");
    require(lg_stereo_clip_reprojection(p.data(),effective.data(),reprojection.data()),"infinite-far clip inverse failed");
    Matrix singular{};auto unchanged=reprojection;
    require(!lg_stereo_clip_reprojection(singular.data(),effective.data(),reprojection.data()) && reprojection==unchanged,"singular inverse modified output or was silently accepted");
    std::puts("ok: shared world/HUD/pass/fill projection and deep-scene clip reprojection");
}

int main(int argc, char **argv)
try {
    test_shared_projection();
    test_live_stereo_scene();
    test_controller_input();
    require(shader_source_digest("a\r\n") == shader_source_digest("a \n"),
            "shader identity differs from guest CR normalization");
    require(shader_source_digest("a") == shader_source_digest("a\n"),
            "shader identity depends on optional final newline");
    require(shader_source_digest("a\n\n") != shader_source_digest("a\n"),
            "shader identity discarded significant trailing source");
    require(hex(source_digest("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "empty shader source hash mismatch");
    require(hex(source_digest("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "shader source SHA-256 mismatch");
    for (const auto &test : std::vector<std::pair<unsigned, std::string>>{
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"}})
        require(hex(source_digest(std::string(test.first, 'a'))) == test.second, "shader hash padding/block mismatch");
    // Non-square matrices in arrays catch both std140 column padding and
    // transpose bugs. These offsets model build-time reflection metadata.
    UniformLayout matrix_layout{1, 0, 128, {{"matrix", 16, 0, 2, 3, 2, 48, 16}}};
    UniformData matrix_data(matrix_layout);
    const float row_major[] = {1,2, 3,4, 5,6};
    matrix_data.write(0, row_major, sizeof row_major, 1, 1, true);
    const float col0[] = {1,3,5}, col1[] = {2,4,6};
    require(!std::memcmp(matrix_data.bytes().data()+64, col0, sizeof col0) &&
            !std::memcmp(matrix_data.bytes().data()+80, col1, sizeof col1) && matrix_data.bytes()[76] == 0,
            "std140 matrix/array stride or transpose mismatch");
    matrix_data.write(0, row_major, sizeof row_major, 0, 1, false);
    require(!std::memcmp(matrix_data.bytes().data()+16, row_major, 12) &&
            !std::memcmp(matrix_data.bytes().data()+32, row_major+3, 12) && matrix_data.bytes()[28] == 0,
            "column-major std140 padding mismatch");
    rejected([&] { matrix_data.write(0, row_major, sizeof row_major, 2); }, "out-of-range uniform array accepted");
    DeviceOptions options;
    options.allow_software = true; // This test deliberately runs on lavapipe.
    options.require_foveation = false;
    auto device = Device::create(options);
    const auto &h = device->handles;
    require(device->capabilities.multiview, "stereo multiview not available");
    auto limited_caps = device->capabilities;
    limited_caps.image_view_format_swizzle = false;
    auto limited = Device::borrow(h, limited_caps);
    TextureInfo luminance{1, 1};
    luminance.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                            VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE};
    rejected([&] { Texture unsupported(*limited, luminance); },
             "unsupported texture component swizzle accepted");
    render_stereo(*device);
    test_sight_policy();
    test_settings_panel();
    render_panels(*device);
    render_overlay(*device);
    if(device->capabilities.density_map&&device->capabilities.density_map_non_subsampled){render_overlay(*device,true);render_panels(*device,true);}
    render_density_reuse(*device);
    render_copy(*device);
    render_stereo_depth_copy(*device);
    if (argc == 2) {
        ShaderPack game_pack(argv[1]);
        size_t fields = 0;
        for (const auto &p : game_pack.programs()) {
            require(game_pack.find(p.vertex_source, p.fragment_source, p.attributes) == &p,
                    "shader archive source/binding selection changed");
            for (const auto &block : p.uniforms) fields += block.fields.size();
            for (const auto *module : {&p.vertex, &p.fragment}) {
                VkShaderModuleCreateInfo create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                create.codeSize = module->size() * 4; create.pCode = module->data();
                VkShaderModule handle;
                check(vkCreateShaderModule(h.device, &create, nullptr, &handle), "archived game SPIR-V module");
                vkDestroyShaderModule(h.device, handle, nullptr);
            }
        }
        std::printf("ok: loaded %zu archived game shader pairs and %zu reflected fields without a runtime GLSL compiler\n",
                    game_pack.programs().size(), fields);
    }
    // Borrowing must not accidentally destroy a device owned by OpenXR.
    {
        auto borrowed = Device::borrow(h, device->capabilities);
        require(borrowed->handles.device == h.device, "borrowed Vulkan device changed");
    }

    constexpr uint32_t frames = 12, words = 16;
    Readback output(*device, frames * words * sizeof(uint32_t));
    {
        FrameRing ring(*device, 3, 4096);
        rejected([&] { ring.submit(); }, "empty submission accepted");
        rejected([&] { ring.upload(nullptr, 1); }, "upload outside frame accepted");
        Submission last{};
        for (uint32_t frame = 0; frame < frames; frame++) {
            auto cmd = ring.begin();
            rejected([&] { ring.begin(); }, "overlapping frame recording accepted");
            std::array<uint32_t, words> values;
            for (uint32_t i = 0; i < words; i++) values[i] = 0x12340000u + frame * 100 + i;
            ring.upload(values.data(), 3, 1);
            auto source = ring.upload(values.data(), sizeof values, 256);
            require(source.offset == 256, "uniform upload alignment not honored");
            rejected([&] { ring.upload(values.data(), 4096); }, "oversized frame upload accepted");
            VkBufferCopy copy{source.offset, frame * sizeof values, sizeof values};
            vkCmdCopyBuffer(cmd, source.buffer, output.buffer, 1, &copy);
            VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ready.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            ready.srcQueueFamilyIndex = ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ready.buffer = output.buffer;
            ready.offset = copy.dstOffset;
            ready.size = copy.size;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 0, nullptr, 1, &ready, 0, nullptr);
            last = ring.submit();
        }
        require(ring.submissions() == frames, "frame submission count mismatch");
        ring.wait(last);
        auto read = static_cast<const uint32_t *>(output.mapped);
        for (uint32_t frame = 0; frame < frames; frame++)
            for (uint32_t i = 0; i < words; i++)
                require(read[frame * words + i] == 0x12340000u + frame * 100 + i,
                        "GPU upload corrupted across buffered frame reuse");
    }

    const auto path = (std::filesystem::temp_directory_path() /
        ("lindbergh-vulkan-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
    // Invalid driver cache bytes must not reach vkCreatePipelineCache.
    {
        std::ofstream invalid(path, std::ios::binary);
        std::array<char, 128> junk{};
        invalid.write(junk.data(), junk.size());
    }
    {
        PipelineCache cache(*device, path);
        require(!cache.loaded(), "invalid Vulkan cache accepted");
        cache.save();
    }
    {
        PipelineCache cache(*device, path);
        require(cache.loaded(), "saved Vulkan cache could not be restored");
    }
    std::filesystem::remove(path);

    if (!device->capabilities.density_map && !device->capabilities.shading_rate) {
        options.require_foveation = true;
        rejected([&] { Device::create(options); }, "required foveation silently disabled");
        std::puts("ok: unsupported required foveation rejected explicitly");
    }
    std::puts("ok: native Vulkan device ownership, asynchronous frame uploads and GPU readback, pipeline cache validation");
    return 0;
} catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL Vulkan: %s\n", e.what());
    return 1;
}
