#pragma once
#include "resources.hpp"
#include "uniforms.hpp"
#include "guest_state.hpp"
#include "../../runtime/xr_overlay.h"
namespace lindbergh::vk {
// One multiview draw per overlay primitive, persistent gun meshes, buffered
// dynamic vertices/uniforms. All shader modules are compiled at build time.
class Overlay {
public:
    Overlay(Device &device,FrameRing &ring,PipelineCache &cache);
    void prepare(Target &target); // all targets/pipelines before gameplay
    void draw(VkCommandBuffer cmd,Target &target,const Matrices &views,
              const std::array<std::array<float,3>,2> &eyes,const std::array<XoFrame,2> &frames,
              Renderer *previous=nullptr,bool preserve=true);
    size_t mesh_buffer_count() const {return meshes_.size();}
    size_t target_count() const {return targets_.size();}
    uint64_t draw_count() const;
    uint64_t render_pass_starts() const {return pass_starts_;}
private:
    struct Mesh {std::unique_ptr<GeometryBuffer> buffer;uint32_t vertices=0;float rail=0;};
    struct Bundle {std::unique_ptr<Renderer> gun,beam;};
    Device &device_;FrameRing &ring_;PipelineCache &cache_;UniformSet uniforms_;
    std::array<Mesh,9> meshes_;
    std::unordered_map<Target *,Bundle> targets_;
    uint64_t pass_starts_=0;
};
}
