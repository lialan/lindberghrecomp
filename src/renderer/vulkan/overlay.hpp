#pragma once
#include "resources.hpp"
#include "uniforms.hpp"
#include "guest_state.hpp"
#include "../../runtime/xr_overlay.h"
#include "../../runtime/xr_menu.h"
namespace lindbergh::vk {
struct PanelDraw {bool visible=false;Matrix model=identity();};
// One multiview draw per overlay primitive, persistent gun meshes, buffered
// dynamic vertices/uniforms. All shader modules are compiled at build time.
class Overlay {
public:
    Overlay(Device &device,FrameRing &ring,PipelineCache &cache);
    void prepare(Target &target); // all targets/pipelines before gameplay
    void draw(VkCommandBuffer cmd,Target &target,const Matrices &views,
              const std::array<std::array<float,3>,2> &eyes,const std::array<XoFrame,2> &frames,
              Renderer *previous=nullptr,bool preserve=true,const std::array<PanelDraw,2> &panels={});
    // A flat frame (a menu, a scope held): the target's picture - the game's
    // own, across each eye - copied and shown on the screen quad, the rest of
    // the eye black (host_xr.c's xo_draw_screen). `screen` maps guest clip
    // space onto the screen per eye. Opens and ends its own pass.
    void draw_picture(VkCommandBuffer cmd,Target &target,const Matrices &screen);
    uint64_t picture_draws() const {return picture_draws_;}
    // Slot 0: settings; slot 1: FPS. Upload only dirty pictures before game passes.
    void upload_panel(VkCommandBuffer cmd,unsigned slot,const void *rgba,size_t bytes);
    uint64_t panel_uploads() const {return panel_uploads_;}
    size_t mesh_buffer_count() const {return meshes_.size();}
    size_t target_count() const {return targets_.size();}
    uint64_t draw_count() const;
    uint64_t render_pass_starts() const {return pass_starts_;}
private:
    struct Mesh {std::unique_ptr<GeometryBuffer> buffer;uint32_t vertices=0;float rail=0;};
    struct Bundle {std::unique_ptr<Renderer> gun,beam,panel;};
    Device &device_;FrameRing &ring_;PipelineCache &cache_;UniformSet uniforms_;
    std::array<Mesh,9> meshes_;
    std::unique_ptr<GeometryBuffer> quad_;
    std::unique_ptr<Sampler> panel_sampler_;
    std::array<std::unique_ptr<Texture>,2> panel_textures_;
    std::array<std::unique_ptr<SampledSet>,2> panel_sets_;
    std::array<bool,2> panel_ready_{};
    uint64_t panel_uploads_=0;
    std::unordered_map<Target *,Bundle> targets_;
    uint64_t pass_starts_=0,picture_draws_=0;
    // The flat frame's picture (one eye's layer), and its descriptor set:
    // replaced together, retired through the ring while frames use them.
    std::shared_ptr<Texture> picture_;
    std::shared_ptr<SampledSet> picture_set_;
};
}
