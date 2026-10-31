#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <string>
#include <vector>

namespace lindbergh::vk {

void check(VkResult result, const char *operation);

struct Capabilities {
    VkPhysicalDeviceProperties properties{};
    bool multiview = false;
    bool density_map = false;
    bool density_map_non_subsampled = false;
    bool shading_rate = false;
};

Capabilities query_capabilities(VkPhysicalDevice physical);

struct DeviceOptions {
    bool allow_software = false; // explicit opt-in for device-free tests
    bool require_foveation = true;
    bool require_multiview = true;
};

// Device creation through OpenXR will supply these same handles. A borrowed
// device is never destroyed by this object; all resources we create still are.
struct DeviceHandles {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
};

class Device {
public:
    static std::unique_ptr<Device> create(const DeviceOptions &options);
    static std::unique_ptr<Device> borrow(DeviceHandles handles, Capabilities enabled);
    ~Device();
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    DeviceHandles handles;
    Capabilities capabilities;
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) const;

private:
    Device() = default;
    bool owned_ = false;
};

// Cache bytes are opaque and tied to the exact GPU/driver. Invalid or foreign
// cache data is ignored before vkCreatePipelineCache ever sees it.
class PipelineCache {
public:
    PipelineCache(Device &device, std::string path);
    ~PipelineCache();
    PipelineCache(const PipelineCache &) = delete;
    PipelineCache &operator=(const PipelineCache &) = delete;
    VkPipelineCache handle() const { return cache_; }
    bool loaded() const { return loaded_; }
    void save();

private:
    Device &device_;
    std::string path_;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    bool loaded_ = false;
};

} // namespace lindbergh::vk
