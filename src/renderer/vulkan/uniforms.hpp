#pragma once
#include "frame_ring.hpp"

namespace lindbergh::vk {

// One shader UBO binding, in ascending Vulkan binding order. Ranges come from
// offline SPIR-V reflection, including std140 padding.
struct UniformBlock { uint32_t binding; VkDeviceSize bytes; };

// Descriptor sets are written once, one per buffered upload slot. Each draw
// copies changed guest state into persistent storage and selects dynamic
// offsets; no descriptor allocation/update or driver synchronization is needed.
// The ring and this object must outlive all work referencing these descriptors.
class UniformSet {
public:
    UniformSet(Device &device, FrameRing &ring, std::vector<UniformBlock> blocks);
    ~UniformSet();
    UniformSet(const UniformSet &) = delete;
    UniformSet &operator=(const UniformSet &) = delete;
    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet current_set() const;
    uint32_t upload(uint32_t block, const void *data, size_t bytes);

private:
    Device &device_;
    FrameRing &ring_;
    std::vector<UniformBlock> blocks_;
    std::vector<VkDescriptorSet> sets_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDeviceSize alignment_;
    void release();
};

} // namespace lindbergh::vk
