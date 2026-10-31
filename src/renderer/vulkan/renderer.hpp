#pragma once
#include "frame_ring.hpp"
#include <array>
#include <unordered_map>

namespace lindbergh::vk {

struct Vertex { float position[4], color[4]; };
struct Matrices { float view_projection[2][16]; };
struct RasterState {
    bool depth_test = true;
    bool depth_write = true;
    bool blend = false;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkCompareOp depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    uint32_t key() const;
};

struct ProgramSource {
    const uint32_t *vertex = nullptr, *fragment = nullptr;
    size_t vertex_bytes = 0, fragment_bytes = 0;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkDescriptorSetLayout> descriptor_layouts;
};

struct TargetInfo {
    uint32_t width = 0, height = 0, layers = 2;
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage color = VK_NULL_HANDLE; // optional borrowed OpenXR swapchain image
    VkImageLayout final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkImageView density_map = VK_NULL_HANDLE; // borrowed runtime foveation image
};

class Target {
public:
    Target(Device &device, TargetInfo info);
    ~Target();
    Target(const Target &) = delete;
    Target &operator=(const Target &) = delete;
    const TargetInfo info;
    VkImage color = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;

private:
    Device &device_;
    VkImage depth_ = VK_NULL_HANDLE;
    VkImageView color_view_ = VK_NULL_HANDLE, depth_view_ = VK_NULL_HANDLE;
    VkDeviceMemory color_memory_ = VK_NULL_HANDLE, depth_memory_ = VK_NULL_HANDLE;
    void release();
};

// No GL API, context or shader compiler is involved. Shader modules are built
// to SPIR-V ahead of time. Draws use pre-created pipeline objects and persistent
// frame uploads; prepare() is explicitly outside the recording path.
class Renderer {
public:
    Renderer(Device &device, Target &target, PipelineCache &cache);
    Renderer(Device &device, Target &target, PipelineCache &cache, const ProgramSource &program);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    void prepare(RasterState state);
    void begin(VkCommandBuffer cmd, std::array<float, 4> clear);
    void draw(FrameRing &ring, const Vertex *vertices, uint32_t count,
              const Matrices &matrices, RasterState state);
    void draw_indexed(FrameRing &ring, const Vertex *vertices, uint32_t vertex_count,
                      const uint32_t *indices, uint32_t index_count,
                      const Matrices &matrices, RasterState state);
    void end();
    uint64_t draw_count() const { return draws_; }
    size_t pipeline_count() const { return pipelines_.size(); }

private:
    Device &device_;
    Target &target_;
    PipelineCache &cache_;
    VkShaderModule vertex_ = VK_NULL_HANDLE, fragment_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkPipeline bound_ = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkPipeline> pipelines_;
    uint64_t draws_ = 0;
    std::vector<VkVertexInputBindingDescription> bindings_;
    std::vector<VkVertexInputAttributeDescription> attributes_;
    void release();
};

} // namespace lindbergh::vk
