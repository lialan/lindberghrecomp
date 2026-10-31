#include "device.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace lindbergh::vk {

void check(VkResult r, const char *op)
{
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(op) + ": VkResult " + std::to_string(r));
}

static bool extension(const std::vector<VkExtensionProperties> &all, const char *name)
{
    return std::any_of(all.begin(), all.end(), [name](const auto &p) {
        return std::strcmp(p.extensionName, name) == 0;
    });
}

Capabilities query_capabilities(VkPhysicalDevice physical)
{
    Capabilities out;
    vkGetPhysicalDeviceProperties(physical, &out.properties);
    uint32_t count = 0;
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "device extensions");
    std::vector<VkExtensionProperties> ext(count);
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, ext.data()), "device extensions");

    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT density{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR rate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    features.pNext = &multiview;
    if (extension(ext, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME)) {
        density.pNext = multiview.pNext;
        multiview.pNext = &density;
    }
    if (extension(ext, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) {
        rate.pNext = multiview.pNext;
        multiview.pNext = &rate;
    }
    vkGetPhysicalDeviceFeatures2(physical, &features);
    VkPhysicalDeviceMultiviewProperties views{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &views;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    out.multiview = multiview.multiview && views.maxMultiviewViewCount >= 2;
    out.density_map = density.fragmentDensityMap;
    out.density_map_non_subsampled = density.fragmentDensityMapNonSubsampledImages;
    out.shading_rate = rate.attachmentFragmentShadingRate;
    out.texture_compression_bc = features.features.textureCompressionBC;
    return out;
}

std::unique_ptr<Device> Device::create(const DeviceOptions &options)
{
    auto out = std::unique_ptr<Device>(new Device);
    out->owned_ = true;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Lindbergh Vulkan renderer";
    app.pEngineName = "lindberghrecomp";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance.pApplicationInfo = &app;
    check(vkCreateInstance(&instance, nullptr, &out->handles.instance), "vkCreateInstance");
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(out->handles.instance, &count, nullptr), "physical devices");
    std::vector<VkPhysicalDevice> physical(count);
    check(vkEnumeratePhysicalDevices(out->handles.instance, &count, physical.data()), "physical devices");
    int best = -1;
    for (auto p : physical) {
        auto caps = query_capabilities(p);
        if (caps.properties.apiVersion < VK_API_VERSION_1_1) continue;
        if (!options.allow_software && caps.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        if (options.require_multiview && !caps.multiview) continue;
        if (options.require_foveation && !caps.density_map && !caps.shading_rate) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &families, nullptr);
        std::vector<VkQueueFamilyProperties> q(families);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &families, q.data());
        for (uint32_t i = 0; i < families; i++) {
            if (!(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            int score = caps.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                        caps.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (score > best) {
                best = score;
                out->handles.physical = p;
                out->handles.queue_family = i;
                out->capabilities = caps;
            }
            break;
        }
    }
    if (!out->handles.physical)
        throw std::runtime_error("No Vulkan device satisfies the requested hardware, multiview and foveation requirements");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = out->handles.queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceMultiviewFeatures views{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    views.multiview = out->capabilities.multiview;
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT density{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR rate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    std::vector<const char *> extensions;
    // Enable one mechanism, never both competing attachments in a render pass.
    if (out->capabilities.density_map) {
        extensions.push_back(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
        density.fragmentDensityMap = VK_TRUE;
        density.fragmentDensityMapNonSubsampledImages = out->capabilities.density_map_non_subsampled;
        views.pNext = &density;
        out->capabilities.shading_rate = false;
    } else if (out->capabilities.shading_rate) {
        extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        // Required for the render-pass-2 attachment form on a Vulkan 1.1 device.
        extensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
        rate.attachmentFragmentShadingRate = VK_TRUE;
        views.pNext = &rate;
    }
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceFeatures core{};
    core.textureCompressionBC = out->capabilities.texture_compression_bc;
    device.pEnabledFeatures = &core;
    device.pNext = &views;
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    device.ppEnabledExtensionNames = extensions.data();
    check(vkCreateDevice(out->handles.physical, &device, nullptr, &out->handles.device), "vkCreateDevice");
    vkGetDeviceQueue(out->handles.device, out->handles.queue_family, 0, &out->handles.queue);
    std::fprintf(stderr, "[vulkan] %s; multiview %s, density-map capability %s, shading-rate capability %s\n",
        out->capabilities.properties.deviceName, out->capabilities.multiview ? "yes" : "no",
        out->capabilities.density_map ? "yes" : "no", out->capabilities.shading_rate ? "yes" : "no");
    return out;
}

std::unique_ptr<Device> Device::borrow(DeviceHandles handles, Capabilities enabled)
{
    if (!handles.instance || !handles.physical || !handles.device || !handles.queue)
        throw std::invalid_argument("Cannot borrow incomplete Vulkan device handles");
    auto out = std::unique_ptr<Device>(new Device);
    out->handles = handles;
    out->capabilities = enabled;
    return out;
}

Device::~Device()
{
    if (owned_ && handles.device) {
        vkDeviceWaitIdle(handles.device); // shutdown only; never a draw-path wait
        vkDestroyDevice(handles.device, nullptr);
    }
    if (owned_ && handles.instance) vkDestroyInstance(handles.instance, nullptr);
}

uint32_t Device::memory_type(uint32_t bits, VkMemoryPropertyFlags flags) const
{
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(handles.physical, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("No Vulkan memory type satisfies resource requirements");
}

PipelineCache::PipelineCache(Device &device, std::string path) : device_(device), path_(std::move(path))
{
    std::vector<char> data;
    std::ifstream in(path_, std::ios::binary | std::ios::ate);
    auto bytes = in ? in.tellg() : std::streampos(0);
    if (bytes >= static_cast<std::streamoff>(sizeof(VkPipelineCacheHeaderVersionOne)) && bytes <= (64 << 20)) {
        data.resize(static_cast<size_t>(bytes));
        in.seekg(0);
        if (!in.read(data.data(), bytes)) data.clear();
    }
    if (!data.empty()) {
        VkPipelineCacheHeaderVersionOne h;
        std::memcpy(&h, data.data(), sizeof h);
        const auto &p = device_.capabilities.properties;
        loaded_ = h.headerSize == sizeof h && h.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                  h.vendorID == p.vendorID && h.deviceID == p.deviceID &&
                  std::memcmp(h.pipelineCacheUUID, p.pipelineCacheUUID, VK_UUID_SIZE) == 0;
        if (!loaded_) data.clear();
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.data();
    check(vkCreatePipelineCache(device_.handles.device, &info, nullptr, &cache_), "vkCreatePipelineCache");
}

void PipelineCache::save()
{
    if (path_.empty()) return;
    size_t n = 0;
    check(vkGetPipelineCacheData(device_.handles.device, cache_, &n, nullptr), "pipeline cache size");
    std::vector<char> data(n);
    check(vkGetPipelineCacheData(device_.handles.device, cache_, &n, data.data()), "pipeline cache data");
    const auto temp = path_ + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.write(data.data(), static_cast<std::streamsize>(n)))
            throw std::runtime_error("Cannot write Vulkan pipeline cache " + temp);
    }
    if (std::rename(temp.c_str(), path_.c_str()) != 0)
        throw std::runtime_error("Cannot replace Vulkan pipeline cache " + path_);
}

PipelineCache::~PipelineCache()
{
    if (cache_) vkDestroyPipelineCache(device_.handles.device, cache_, nullptr);
}

} // namespace lindbergh::vk
