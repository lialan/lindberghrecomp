#include "frame_ring.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace lindbergh::vk {
namespace {
uint64_t ticks()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
uint64_t thread_ticks()
{
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    const uint64_t k=(uint64_t(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime;
    const uint64_t u=(uint64_t(user.dwHighDateTime)<<32)|user.dwLowDateTime;
    return (k+u)*100;
#else
    struct timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1000000000 + uint64_t(ts.tv_nsec);
#endif
}
} // namespace
bool FrameRing::profile(const std::string &path)
{
    if (recording_ || submissions_ || profile_)
        throw std::logic_error("Frame profiling must start before recording");
    if (path.empty())
        return false;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device_.handles.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device_.handles.physical, &count, families.data());
    timestamp_bits_ = families.at(device_.handles.queue_family).timestampValidBits;
    if (!timestamp_bits_ || timestamp_bits_ > 64 ||
        !std::isfinite(device_.capabilities.properties.limits.timestampPeriod) ||
        device_.capabilities.properties.limits.timestampPeriod <= 0) {
        std::fprintf(stderr, "[vulkan-profile] GPU timestamps unsupported on this queue\n");
        return false;
    }
    profile_ = std::fopen(path.c_str(), "w");
    if (!profile_)
        throw std::runtime_error("Cannot open Vulkan submission timing output");
    try {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = uint32_t(slots_.size()) * 2;
        check(vkCreateQueryPool(device_.handles.device, &info, nullptr, &queries_),
              "submission timing queries");
    } catch (...) {
        std::fclose(profile_);
        profile_ = nullptr;
        throw;
    }
    std::fprintf(profile_,
                 "# measurement=GPU submission spans and CPU recording/slot retirement; not display timing\n"
                 "submission,frame,slot,gpu_valid,gpu_ns,reuse_wait_ns,pool_reset_ns,retire_ns,record_ns,"
                 "submit_ns,upload_bytes,retained_resources,record_thread_ns,record_thread_valid\n");
    return true;
}
void FrameRing::report(uint32_t slot)
{
    auto &s = slots_[slot];
    auto &t = s.timing;
    if (!profile_ || !t.pending)
        return;
    uint64_t stamp[2]{};
    const auto result = vkGetQueryPoolResults(device_.handles.device, queries_, slot * 2, 2, sizeof stamp,
                                              stamp, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    const auto mask = timestamp_bits_ == 64 ? UINT64_MAX : (uint64_t(1) << timestamp_bits_) - 1;
    const double ns =
        double((stamp[1] - stamp[0]) & mask) * device_.capabilities.properties.limits.timestampPeriod;
    std::fprintf(profile_, "%llu,%llu,%u,%u,%.0f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u\n",
                 (unsigned long long)t.submission, (unsigned long long)t.frame, slot,
                 unsigned(result == VK_SUCCESS), ns, (unsigned long long)t.reuse_wait,
                 (unsigned long long)t.reset, (unsigned long long)t.retire, (unsigned long long)t.record,
                 (unsigned long long)t.submit, (unsigned long long)t.uploads, (unsigned long long)t.retained,
                 (unsigned long long)t.record_thread, unsigned(t.cpu_valid));
    t.pending = false;
}

FrameRing::FrameRing(Device &device, uint32_t frames, VkDeviceSize bytes)
    : device_(device), slots_(frames), capacity_(bytes)
{
    if (!frames || frames > 8 || !bytes)
        throw std::invalid_argument("Invalid Vulkan frame ring size");
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
            memory.memoryTypeIndex =
                device_.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
    if (recording_)
        throw std::logic_error("Vulkan frame already recording");
    auto &s = slots_[current_];
    const auto waiting = profile_ ? ticks() : 0;
    if (s.submitted) {
        check(vkWaitForFences(device_.handles.device, 1, &s.fence, VK_TRUE, UINT64_MAX), "reuse frame fence");
        s.submitted = false;
    }
    const auto waited = profile_ ? ticks() - waiting : 0;
    if (profile_)
        report(current_);
    const auto resetting = profile_ ? ticks() : 0;
    check(vkResetCommandPool(device_.handles.device, s.pool, 0), "reset frame pool");
    const auto reset = profile_ ? ticks() - resetting : 0;
    const auto retiring = profile_ ? ticks() : 0;
    s.resources.clear();
    if (profile_) {
        s.timing = {};
        s.timing.frame = frame_id_;
        s.timing.reuse_wait = waited;
        s.timing.reset = reset;
        s.timing.retire = ticks() - retiring;
        s.timing.started = ticks();
        s.timing.cpu_started = thread_ticks();
        s.timing.cpu_thread = std::this_thread::get_id();
    }
    s.used = 0;
    s.external_uploads=0;
    ++s.generation;
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(s.cmd, &info), "begin frame commands");
    if (profile_) {
        vkCmdResetQueryPool(s.cmd, queries_, current_ * 2, 2);
        vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, current_ * 2);
    }
    recording_ = true;
    return s.cmd;
}

bool FrameRing::can_upload(size_t bytes,VkDeviceSize alignment) const
{
    if(!recording_||!alignment||(alignment&(alignment-1)))return false;
    const auto used=slots_[current_].used;
    const auto padding=(alignment-(used&(alignment-1)))&(alignment-1);
    return padding<=capacity_-used&&bytes<=capacity_-used-padding;
}
void FrameRing::record_external_upload(size_t bytes)
{
    if(!recording_)throw std::logic_error("External upload outside frame recording");
    if(profile_) {
        auto &s=slots_[current_];
        if(bytes>UINT64_MAX-s.used-s.external_uploads)throw std::overflow_error("Upload timing counter overflow");
        s.external_uploads+=bytes;
    }
}

Upload FrameRing::upload(const void *data, size_t bytes, VkDeviceSize alignment)
{
    if (!recording_)
        throw std::logic_error("Upload outside Vulkan frame");
    if (!alignment || (alignment & (alignment - 1)) || (!data && bytes))
        throw std::invalid_argument("Invalid Vulkan upload data or alignment");
    auto &s = slots_[current_];
    // Check before adding, including caller-controlled sizes.
    const auto padding = (alignment - (s.used & (alignment - 1))) & (alignment - 1);
    if (padding > capacity_ - s.used || bytes > capacity_ - s.used - padding)
        throw std::overflow_error("Vulkan frame upload capacity exhausted");
    const auto offset = s.used + padding;
    if (bytes)
        std::memcpy(static_cast<char *>(s.mapped) + offset, data, bytes);
    s.used = offset + bytes;
    return {s.upload, offset};
}

uint32_t FrameRing::recording_slot() const
{
    if (!recording_)
        throw std::logic_error("No Vulkan frame recording");
    return current_;
}

VkBuffer FrameRing::upload_buffer(uint32_t slot) const
{
    if (slot >= slots_.size())
        throw std::out_of_range("Unknown Vulkan upload slot");
    return slots_[slot].upload;
}

void FrameRing::keep_alive(std::shared_ptr<void> resource)
{
    if (!recording_)
        throw std::logic_error("Resource retention outside frame");
    slots_[current_].resources.push_back(std::move(resource));
}

Submission FrameRing::submit(VkSemaphore wait, VkSemaphore signal, VkPipelineStageFlags wait_stage)
{
    if (!recording_)
        throw std::logic_error("Submit outside Vulkan frame");
    auto &s = slots_[current_];
    const auto d = device_.handles.device;
    if (profile_)
        vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, current_ * 2 + 1);
    check(vkEndCommandBuffer(s.cmd), "end frame commands");
    if (profile_) {
        s.timing.record = ticks() - s.timing.started;
        const auto cpu = thread_ticks();
        s.timing.cpu_valid = s.timing.cpu_thread == std::this_thread::get_id() && cpu >= s.timing.cpu_started;
        s.timing.record_thread = s.timing.cpu_valid ? cpu - s.timing.cpu_started : 0;
        s.timing.uploads = s.used+s.external_uploads;
        s.timing.retained = s.resources.size();
        s.timing.submission = submissions_ + 1;
    }
    check(vkResetFences(d, 1, &s.fence), "reset frame fence");
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &s.cmd;
    info.waitSemaphoreCount = wait ? 1 : 0;
    info.pWaitSemaphores = &wait;
    info.pWaitDstStageMask = &wait_stage;
    info.signalSemaphoreCount = signal ? 1 : 0;
    info.pSignalSemaphores = &signal;
    const auto submitting = profile_ ? ticks() : 0;
    check(vkQueueSubmit(device_.handles.queue, 1, &info, s.fence), "submit frame commands");
    if (profile_) {
        s.timing.submit = ticks() - submitting;
        s.timing.pending = true;
    }
    s.submitted = true;
    const Submission result{current_, s.generation};
    current_ = (current_ + 1) % static_cast<uint32_t>(slots_.size());
    recording_ = false;
    ++submissions_;
    return result;
}

void FrameRing::wait(Submission submission)
{
    if (submission.slot >= slots_.size())
        throw std::invalid_argument("Unknown Vulkan frame slot");
    auto &s = slots_[submission.slot];
    if (s.generation != submission.generation || !s.submitted)
        throw std::logic_error("Vulkan submission has already been recycled");
    check(vkWaitForFences(device_.handles.device, 1, &s.fence, VK_TRUE, UINT64_MAX), "wait submission");
}

void FrameRing::release()
{
    const auto d = device_.handles.device;
    for (auto &s : slots_) {
        if (s.submitted)
            vkWaitForFences(d, 1, &s.fence, VK_TRUE, UINT64_MAX);
        if (profile_)
            report(uint32_t(&s - slots_.data()));
        if (s.mapped)
            vkUnmapMemory(d, s.memory);
        if (s.upload)
            vkDestroyBuffer(d, s.upload, nullptr);
        if (s.memory)
            vkFreeMemory(d, s.memory, nullptr);
        if (s.fence)
            vkDestroyFence(d, s.fence, nullptr);
        if (s.pool)
            vkDestroyCommandPool(d, s.pool, nullptr);
        s = {};
    }
    if (queries_) {
        vkDestroyQueryPool(d, queries_, nullptr);
        queries_ = VK_NULL_HANDLE;
    }
    if (profile_) {
        std::fclose(profile_);
        profile_ = nullptr;
    }
}

FrameRing::~FrameRing() { release(); }

} // namespace lindbergh::vk
