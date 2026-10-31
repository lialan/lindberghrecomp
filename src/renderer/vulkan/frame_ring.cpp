#include "frame_ring.hpp"
#include <cstring>
#include <stdexcept>

namespace lindbergh::vk {

FrameRing::FrameRing(Device &device, uint32_t frames, VkDeviceSize bytes)
    : device_(device), slots_(frames), capacity_(bytes)
{
    if (!frames || frames > 8 || !bytes) throw std::invalid_argument("Invalid Vulkan frame ring size");
    const auto d = device_.handles.device;
    try {
        for (auto &s : slots_) {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool.queueFamilyIndex = device_.handles.queue_family;
            check(vkCreateCommandPool(d, &pool, nullptr, &s.pool), "frame command pool");
            VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            command.commandPool = s.pool;
            command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            command.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(d, &command, &s.cmd), "frame command buffer");
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            check(vkCreateFence(d, &fence, nullptr, &s.fence), "frame fence");
            VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer.size = capacity_;
            buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateBuffer(d, &buffer, nullptr, &s.upload), "frame upload buffer");
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(d, s.upload, &req);
            VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            memory.allocationSize = req.size;
            memory.memoryTypeIndex = device_.memory_type(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            check(vkAllocateMemory(d, &memory, nullptr, &s.memory), "frame upload memory");
            check(vkBindBufferMemory(d, s.upload, s.memory, 0), "bind frame upload");
            check(vkMapMemory(d, s.memory, 0, capacity_, 0, &s.mapped), "map frame upload");
        }
    } catch (...) {
        release();
        throw;
    }
}

VkCommandBuffer FrameRing::begin()
{
    if (recording_) throw std::logic_error("Vulkan frame already recording");
    auto &s = slots_[current_];
    if (s.submitted) {
        check(vkWaitForFences(device_.handles.device, 1, &s.fence, VK_TRUE, UINT64_MAX), "reuse frame fence");
        s.submitted = false;
    }
    check(vkResetCommandPool(device_.handles.device, s.pool, 0), "reset frame pool");
    s.used = 0;
    ++s.generation;
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(s.cmd, &info), "begin frame commands");
    recording_ = true;
    return s.cmd;
}

Upload FrameRing::upload(const void *data, size_t bytes, VkDeviceSize alignment)
{
    if (!recording_) throw std::logic_error("Upload outside Vulkan frame");
    if (!alignment || (alignment & (alignment - 1)) || (!data && bytes))
        throw std::invalid_argument("Invalid Vulkan upload data or alignment");
    auto &s = slots_[current_];
    // Check before adding, including caller-controlled sizes.
    const auto padding = (alignment - (s.used & (alignment - 1))) & (alignment - 1);
    if (padding > capacity_ - s.used || bytes > capacity_ - s.used - padding)
        throw std::overflow_error("Vulkan frame upload capacity exhausted");
    const auto offset = s.used + padding;
    if (bytes) std::memcpy(static_cast<char *>(s.mapped) + offset, data, bytes);
    s.used = offset + bytes;
    return {s.upload, offset};
}

uint32_t FrameRing::recording_slot() const
{
    if (!recording_) throw std::logic_error("No Vulkan frame recording");
    return current_;
}

VkBuffer FrameRing::upload_buffer(uint32_t slot) const
{
    if (slot >= slots_.size()) throw std::out_of_range("Unknown Vulkan upload slot");
    return slots_[slot].upload;
}

Submission FrameRing::submit()
{
    if (!recording_) throw std::logic_error("Submit outside Vulkan frame");
    auto &s = slots_[current_];
    const auto d = device_.handles.device;
    check(vkEndCommandBuffer(s.cmd), "end frame commands");
    check(vkResetFences(d, 1, &s.fence), "reset frame fence");
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &s.cmd;
    check(vkQueueSubmit(device_.handles.queue, 1, &info, s.fence), "submit frame commands");
    s.submitted = true;
    const Submission result{current_, s.generation};
    current_ = (current_ + 1) % static_cast<uint32_t>(slots_.size());
    recording_ = false;
    ++submissions_;
    return result;
}

void FrameRing::wait(Submission submission)
{
    if (submission.slot >= slots_.size()) throw std::invalid_argument("Unknown Vulkan frame slot");
    auto &s = slots_[submission.slot];
    if (s.generation != submission.generation || !s.submitted)
        throw std::logic_error("Vulkan submission has already been recycled");
    check(vkWaitForFences(device_.handles.device, 1, &s.fence, VK_TRUE, UINT64_MAX), "wait submission");
}

void FrameRing::release()
{
    const auto d = device_.handles.device;
    for (auto &s : slots_) {
        if (s.submitted) vkWaitForFences(d, 1, &s.fence, VK_TRUE, UINT64_MAX);
        if (s.mapped) vkUnmapMemory(d, s.memory);
        if (s.upload) vkDestroyBuffer(d, s.upload, nullptr);
        if (s.memory) vkFreeMemory(d, s.memory, nullptr);
        if (s.fence) vkDestroyFence(d, s.fence, nullptr);
        if (s.pool) vkDestroyCommandPool(d, s.pool, nullptr);
        s = {};
    }
}

FrameRing::~FrameRing() { release(); }

} // namespace lindbergh::vk
