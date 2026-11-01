#pragma once
#include "frame_ring.hpp"
#include "renderer.hpp"

namespace lindbergh::vk {

// GPU resources are versioned by the guest frontend. They must outlive the last
// submission referencing them; destruction never stalls the rendering queue.
// Upload commands are recorded before/outside a render pass, in queue order.
class GeometryBuffer {
public:
    GeometryBuffer(Device &device, VkDeviceSize bytes);
    ~GeometryBuffer();
    GeometryBuffer(const GeometryBuffer &) = delete;
    GeometryBuffer &operator=(const GeometryBuffer &) = delete;
    void upload(VkCommandBuffer cmd, FrameRing &ring, const void *data, size_t bytes, VkDeviceSize offset = 0,
                bool dedicated_staging = false);
    VkBuffer handle() const { return buffer_; }
private:
    Device &device_;
    VkDeviceSize bytes_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void release();
};

// Immutable density map, initialized once before rendering. Full rate is a
// safe owned attachment when no runtime swapchain image has been acquired.
class ConstantDensityMap {
public:
    ConstantDensityMap(Device &device,FrameRing &ring,uint32_t width,uint32_t height,uint32_t layers,float density=1);
    ~ConstantDensityMap();
    ConstantDensityMap(const ConstantDensityMap &)=delete;
    ConstantDensityMap &operator=(const ConstantDensityMap &)=delete;
    VkImageView view() const { return view_; }
private:
    Device &device_;
    VkImage image_=VK_NULL_HANDLE;VkDeviceMemory memory_=VK_NULL_HANDLE;VkImageView view_=VK_NULL_HANDLE;
    void release();
};

struct TextureInfo {
    uint32_t width, height;
    uint32_t levels = 1, layers = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkComponentMapping components{};
    bool cube = false; // six layers, sampled through a cube view
};

class Texture {
public:
    Texture(Device &device, TextureInfo info);
    // Views of a render target's colour image (one level), not owning it: the
    // target ends its passes in SHADER_READ_ONLY_OPTIMAL and outlives its use.
    Texture(Device &device, TextureInfo info, VkImage borrowed);
    ~Texture();
    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;
    void upload(VkCommandBuffer cmd, FrameRing &ring, uint32_t level, uint32_t layer,
                const void *data, size_t bytes);
    void copy_from(VkCommandBuffer cmd, Target &source, uint32_t level, int32_t dst_x, int32_t dst_y,
                   int32_t src_x, int32_t src_y, uint32_t width, uint32_t height,
                   uint32_t source_layer = 0, uint32_t destination_layer = 0, uint32_t layer_count = 1);
    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkImageView array_view(); // cached array view; one-layer static images are shared by both eyes
    const TextureInfo info;
private:
    Device &device_;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE, array_view_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    bool borrowed_ = false;
    std::vector<bool> initialized_;
    uint32_t block_width_, block_bytes_;
    void release();
};

struct SamplerInfo {
    VkFilter min_filter = VK_FILTER_NEAREST, mag_filter = VK_FILTER_NEAREST;
    VkSamplerMipmapMode mip_filter = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode u = VK_SAMPLER_ADDRESS_MODE_REPEAT, v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    float min_lod = 0, max_lod = 0;
    bool compare = false;
    VkCompareOp compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkBorderColor border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
};

class Sampler {
public:
    Sampler(Device &device, SamplerInfo info = {});
    ~Sampler();
    Sampler(const Sampler &) = delete;
    Sampler &operator=(const Sampler &) = delete;
    VkSampler handle() const { return sampler_; }
private:
    Device &device_;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

struct SampledImage { uint32_t binding; VkImageView view; VkSampler sampler; };

// Immutable material descriptors, prepared before drawing and reused until
// textures/samplers change. Referenced image views and samplers must outlive it.
class SampledSet {
public:
    SampledSet(Device &device, const std::vector<SampledImage> &images);
    ~SampledSet();
    SampledSet(const SampledSet &) = delete;
    SampledSet &operator=(const SampledSet &) = delete;
    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet handle() const { return set_; }
private:
    Device &device_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    void release();
};

} // namespace lindbergh::vk
