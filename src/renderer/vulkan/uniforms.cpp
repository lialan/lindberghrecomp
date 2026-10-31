#include "uniforms.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lindbergh::vk {

UniformSet::UniformSet(Device &device, FrameRing &ring, std::vector<UniformBlock> blocks)
    : device_(device), ring_(ring), blocks_(std::move(blocks))
{
    const auto &limits = device.capabilities.properties.limits;
    alignment_ = std::max<VkDeviceSize>(1, limits.minUniformBufferOffsetAlignment);
    if (blocks_.empty() || blocks_.size() > limits.maxDescriptorSetUniformBuffersDynamic ||
        ring.upload_capacity() > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("Invalid Vulkan dynamic uniform set size");
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const auto &block : blocks_) {
        if (!block.bytes || block.bytes > limits.maxUniformBufferRange || block.bytes > ring.upload_capacity() ||
            (!bindings.empty() && block.binding <= bindings.back().binding))
            throw std::invalid_argument("Invalid or unsorted Vulkan uniform block");
        bindings.push_back({block.binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    }
    const auto d = device.handles.device;
    try {
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = static_cast<uint32_t>(bindings.size());
        layout.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(d, &layout, nullptr, &layout_), "uniform descriptor layout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            ring.slot_count() * static_cast<uint32_t>(blocks_.size())};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = ring.slot_count(); pool.poolSizeCount = 1; pool.pPoolSizes = &size;
        check(vkCreateDescriptorPool(d, &pool, nullptr, &pool_), "buffered uniform descriptor pool");
        sets_.resize(ring.slot_count());
        std::vector<VkDescriptorSetLayout> layouts(sets_.size(), layout_);
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool_; alloc.descriptorSetCount = ring.slot_count(); alloc.pSetLayouts = layouts.data();
        check(vkAllocateDescriptorSets(d, &alloc, sets_.data()), "buffered uniform descriptor sets");
        for (uint32_t slot = 0; slot < ring.slot_count(); ++slot) {
            std::vector<VkDescriptorBufferInfo> buffers;
            for (const auto &b : blocks_) buffers.push_back({ring.upload_buffer(slot), 0, b.bytes});
            std::vector<VkWriteDescriptorSet> writes;
            for (size_t i = 0; i < blocks_.size(); ++i) {
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = sets_[slot]; write.dstBinding = blocks_[i].binding;
                write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                write.pBufferInfo = &buffers[i];
                writes.push_back(write);
            }
            vkUpdateDescriptorSets(d, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    } catch (...) { release(); throw; }
}

VkDescriptorSet UniformSet::current_set() const { return sets_[ring_.recording_slot()]; }

uint32_t UniformSet::upload(uint32_t block, const void *data, size_t bytes)
{
    if (block >= blocks_.size() || bytes != blocks_[block].bytes || !data)
        throw std::invalid_argument("Uniform data disagrees with reflected block size");
    return static_cast<uint32_t>(ring_.upload(data, bytes, alignment_).offset);
}

void UniformSet::release()
{
    if (pool_) vkDestroyDescriptorPool(device_.handles.device, pool_, nullptr);
    if (layout_) vkDestroyDescriptorSetLayout(device_.handles.device, layout_, nullptr);
}
UniformSet::~UniformSet() { release(); }

} // namespace lindbergh::vk
