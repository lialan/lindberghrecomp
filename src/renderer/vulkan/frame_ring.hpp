#pragma once
#include "device.hpp"
#include <cstddef>
#include <cstdint>

namespace lindbergh::vk {

struct Submission { uint32_t slot; uint64_t generation; };
struct Upload { VkBuffer buffer; VkDeviceSize offset; };

// Persistent uploads and one command pool/fence per in-flight frame. Submission
// never waits for the GPU; beginning a reused slot is the backpressure point.
// All calls are made by the renderer thread, not arbitrary guest threads.
class FrameRing {
public:
    FrameRing(Device &device, uint32_t frames = 3, VkDeviceSize upload_bytes = 4u << 20);
    ~FrameRing();
    FrameRing(const FrameRing &) = delete;
    FrameRing &operator=(const FrameRing &) = delete;
    VkCommandBuffer begin();
    Upload upload(const void *data, size_t bytes, VkDeviceSize alignment = 16);
    Submission submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE,
                      VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    void wait(Submission submission);
    uint64_t submissions() const { return submissions_; }
    uint32_t slot_count() const { return static_cast<uint32_t>(slots_.size()); }
    uint32_t recording_slot() const;
    VkBuffer upload_buffer(uint32_t slot) const;
    VkDeviceSize upload_capacity() const { return capacity_; }

private:
    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkBuffer upload = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = nullptr;
        VkDeviceSize used = 0;
        uint64_t generation = 0;
        bool submitted = false;
    };
    Device &device_;
    std::vector<Slot> slots_;
    VkDeviceSize capacity_;
    uint32_t current_ = 0;
    bool recording_ = false;
    uint64_t submissions_ = 0;
    void release();
};

} // namespace lindbergh::vk
