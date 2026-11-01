#pragma once
#include "frame_ring.hpp"
#include <array>
#include <unordered_map>

namespace lindbergh::vk {

struct Vertex { float position[4], color[4]; };
struct Matrices { float view_projection[2][16]; };
struct RasterKey {
    uint64_t a, b;
    bool operator==(const RasterKey &v) const { return a==v.a && b==v.b; }
};
struct RasterHash { size_t operator()(const RasterKey &k) const { return size_t(k.a ^ (k.b * 0x9e3779b97f4a7c15ull)); } };
struct RasterState {
    bool depth_test = true;
    bool depth_write = true;
    bool blend = false;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkCompareOp depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkBlendFactor src_rgb=VK_BLEND_FACTOR_ONE, dst_rgb=VK_BLEND_FACTOR_ZERO;
    VkBlendFactor src_alpha=VK_BLEND_FACTOR_ONE, dst_alpha=VK_BLEND_FACTOR_ZERO;
    VkBlendOp blend_rgb=VK_BLEND_OP_ADD, blend_alpha=VK_BLEND_OP_ADD;
    uint32_t color_mask=15, alpha_func=0, fixed_texture=0, fixed_env=0, fog_mode=0;
    bool stencil=false, depth_bias=false, clockwise=false;
    VkCompareOp stencil_compare=VK_COMPARE_OP_ALWAYS;
    VkStencilOp stencil_fail=VK_STENCIL_OP_KEEP, stencil_depth_fail=VK_STENCIL_OP_KEEP, stencil_pass=VK_STENCIL_OP_KEEP;
    RasterKey key() const;
};

struct ProgramSource {
    const uint32_t *vertex = nullptr, *fragment = nullptr;
    size_t vertex_bytes = 0, fragment_bytes = 0;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkDescriptorSetLayout> descriptor_layouts;
};

struct IndexStream {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkIndexType type = VK_INDEX_TYPE_UINT32;
};

struct TargetInfo {
    uint32_t width = 0, height = 0, layers = 2;
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage color = VK_NULL_HANDLE; // optional borrowed OpenXR swapchain image
    VkImageLayout final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkImageView density_map = VK_NULL_HANDLE; // borrowed runtime foveation image
    bool guest_bottom_origin = false; // offscreen rows follow guest GL texture storage
};

class Target {
public:
    Target(Device &device, TargetInfo info);
    ~Target();
    Target(const Target &) = delete;
    Target &operator=(const Target &) = delete;
    const TargetInfo info;
    VkImage color = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE, load_pass = VK_NULL_HANDLE;
    VkImage depth_image() const { return depth_; }
    static constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    // Pre-register all runtime views during target creation, then select only
    // the map belonging to the currently acquired OpenXR image each frame.
    void prepare_density_maps(const std::vector<VkImageView> &maps);
    void select_density_map(VkImageView map);
    size_t density_framebuffer_count() const { return density_framebuffers_.size(); }

private:
    Device &device_;
    VkImage depth_ = VK_NULL_HANDLE;
    VkImageView color_view_ = VK_NULL_HANDLE, depth_view_ = VK_NULL_HANDLE;
    VkDeviceMemory color_memory_ = VK_NULL_HANDLE, depth_memory_ = VK_NULL_HANDLE;
    std::vector<std::pair<VkImageView,VkFramebuffer>> density_framebuffers_;
    VkFramebuffer make_framebuffer(VkImageView density_map);
    void release();
};

// No GL API, context or shader compiler is involved. Shader modules are built
// to SPIR-V ahead of time. Draws use pre-created pipeline objects and persistent
// frame uploads; prepare() is explicitly outside the recording path.
class Renderer {
public:
    enum class Command : unsigned { Pipeline, Viewport, Scissor, StencilReference, StencilCompare,
        StencilWrite, DepthBias, BlendConstants, Descriptors, VertexBuffer, IndexBuffer, PushConstants, Count };
    static constexpr size_t command_count = size_t(Command::Count);
    struct CommandStats { std::array<uint64_t,command_count> issued{}, filtered{}; };
    Renderer(Device &device, Target &target, PipelineCache &cache);
    Renderer(Device &device, Target &target, PipelineCache &cache, const ProgramSource &program);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    void prepare(RasterState state);
    // Called only when a new variant is created, never for a cached draw.
    std::function<void(RasterState)> pipeline_created;
    void begin(VkCommandBuffer cmd, std::array<float, 4> clear, bool preserve = false);
    void viewport(float x, float y, float width, float height, VkRect2D scissor,float min_depth=0,float max_depth=1);
    void dynamic_state(uint32_t stencil_ref, uint32_t stencil_compare_mask, uint32_t stencil_write_mask,
                       float depth_bias_factor, float depth_bias_units, const float blend_constants[4]);
    void clear(VkImageAspectFlags aspects, std::array<float, 4> color, float depth, uint32_t stencil, VkRect2D rect);
    void attach(VkCommandBuffer cmd);
    void detach();
    void bind_descriptors(uint32_t first_set, uint32_t count, const VkDescriptorSet *sets,
                          uint32_t offset_count = 0, const uint32_t *dynamic_offsets = nullptr);
    void draw(FrameRing &ring, const Vertex *vertices, uint32_t count,
              const Matrices &matrices, RasterState state);
    void draw_indexed(FrameRing &ring, const Vertex *vertices, uint32_t vertex_count,
                      const uint32_t *indices, uint32_t index_count,
                      const Matrices &matrices, RasterState state);
    // Streams follow ProgramSource::bindings order. Already uploaded/static
    // geometry can be drawn repeatedly without another CPU copy.
    void draw_buffers(const Upload *streams, uint32_t stream_count, uint32_t count,
                      const Matrices &matrices, RasterState state,
                      const IndexStream *indices = nullptr);
    void end();
    uint64_t draw_count() const { return draws_; }
    size_t pipeline_count() const { return pipelines_.size(); }
    void filter_state(bool enabled);
    void collect_command_stats(bool enabled) { count_commands_=enabled; }
    const CommandStats &command_stats() const { return commands_; }
    static CommandStats global_command_stats();
    static bool command_stats_enabled();
    static const char *command_name(Command);

private:
    Device &device_;
    Target &target_;
    PipelineCache &cache_;
    VkShaderModule vertex_ = VK_NULL_HANDLE, fragment_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkPipeline bound_ = VK_NULL_HANDLE;
    std::unordered_map<RasterKey, VkPipeline, RasterHash> pipelines_;
    uint64_t draws_ = 0;
    std::vector<VkVertexInputBindingDescription> bindings_;
    std::vector<VkVertexInputAttributeDescription> attributes_;
    uint32_t descriptor_count_ = 0;
    bool filter_=true, count_commands_=false, viewport_valid_=false, scissor_valid_=false;
    bool streams_valid_=false, index_valid_=false, matrices_valid_=false;
    uint32_t dynamic_valid_=0, stencil_ref_=0, stencil_compare_=0, stencil_write_=0;
    VkViewport viewport_{};
    VkRect2D scissor_{};
    std::array<float,2> depth_bias_{};
    std::array<float,4> blend_constants_{};
    Matrices matrices_{};
    std::vector<Upload> streams_;
    IndexStream index_{};
    struct DescriptorBinding {
        bool valid=false;
        std::vector<VkDescriptorSet> sets;
        std::vector<uint32_t> offsets;
    };
    std::vector<DescriptorBinding> descriptors_;
    CommandStats commands_;
    bool emit(Command,bool redundant,bool always_filter=false);
    void invalidate_state();
    void release();
};

} // namespace lindbergh::vk
