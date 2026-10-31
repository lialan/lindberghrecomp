#include "window.hpp"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <stdexcept>
namespace lindbergh::vk {
DeviceOptions desktop_options(SDL_Window *window)
{
    unsigned count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr)) throw std::runtime_error(SDL_GetError());
    DeviceOptions options;
    options.require_foveation = false; // desktop bring-up, never the Frame launch policy
    options.instance_extensions.resize(count);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &count, options.instance_extensions.data())) throw std::runtime_error(SDL_GetError());
    options.device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    options.create_surface = [window](VkInstance instance) {
        VkSurfaceKHR surface;
        if (!SDL_Vulkan_CreateSurface(window, instance, &surface)) throw std::runtime_error(SDL_GetError());
        return surface;
    };
    return options;
}
Window::Window(Device &device, SDL_Window *window, uint32_t frames) : device_(device)
{
    if (!device.handles.surface || !frames) throw std::invalid_argument("Vulkan desktop surface unavailable");
    const auto &h = device.handles;
    try {
        VkSurfaceCapabilitiesKHR caps;
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(h.physical, h.surface, &caps), "surface capabilities");
        uint32_t count = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(h.physical, h.surface, &count, nullptr), "surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(h.physical, h.surface, &count, formats.data()), "surface formats");
        if (formats.empty()) throw std::runtime_error("No Vulkan surface format");
        auto format = formats[0];
        for (const auto &f : formats) if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) format = f;
        auto extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            int w, h; SDL_Vulkan_GetDrawableSize(window, &w, &h);
            extent = {std::clamp(uint32_t(w), caps.minImageExtent.width, caps.maxImageExtent.width),
                      std::clamp(uint32_t(h), caps.minImageExtent.height, caps.maxImageExtent.height)};
        }
        VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = h.surface;
        create.minImageCount = std::max(caps.minImageCount, frames);
        if (caps.maxImageCount) create.minImageCount = std::min(create.minImageCount, caps.maxImageCount);
        create.imageFormat = format.format; create.imageColorSpace = format.colorSpace;
        create.imageExtent = extent; create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        create.preTransform = caps.currentTransform;
        create.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
        create.presentMode = VK_PRESENT_MODE_FIFO_KHR; create.clipped = VK_TRUE;
        check(vkCreateSwapchainKHR(h.device, &create, nullptr, &swapchain_), "desktop swapchain");
        check(vkGetSwapchainImagesKHR(h.device, swapchain_, &count, nullptr), "swapchain images");
        std::vector<VkImage> images(count);
        check(vkGetSwapchainImagesKHR(h.device, swapchain_, &count, images.data()), "swapchain images");
        for (const auto image : images) {
            TargetInfo info{extent.width, extent.height, 1, format.format, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
            targets_.push_back(std::make_unique<Target>(device, info));
        }
        acquire_.resize(frames); present_.resize(count);
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto &s : acquire_) check(vkCreateSemaphore(h.device, &semaphore, nullptr, &s), "acquire semaphore");
        for (auto &s : present_) check(vkCreateSemaphore(h.device, &semaphore, nullptr, &s), "present semaphore");
    } catch (...) { release(); throw; }
}
Target &Window::acquire(FrameRing &ring)
{
    if (acquired_) throw std::logic_error("Desktop Vulkan image already acquired");
    slot_ = ring.recording_slot();
    if (slot_ >= acquire_.size()) throw std::logic_error("Vulkan frame count mismatch");
    const auto result = vkAcquireNextImageKHR(device_.handles.device, swapchain_, UINT64_MAX, acquire_[slot_], VK_NULL_HANDLE, &image_);
    if (result != VK_SUBOPTIMAL_KHR) check(result, "acquire desktop image (resize requires swapchain recreation)");
    acquired_ = true;
    return *targets_.at(image_);
}
Submission Window::present(FrameRing &ring)
{
    if (!acquired_ || ring.recording_slot() != slot_) throw std::logic_error("No matching acquired desktop image");
    const auto submitted = ring.submit(acquire_[slot_], present_[image_]);
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1; info.pWaitSemaphores = &present_[image_];
    info.swapchainCount = 1; info.pSwapchains = &swapchain_; info.pImageIndices = &image_;
    const auto result = vkQueuePresentKHR(device_.handles.queue, &info);
    acquired_ = false;
    if (result != VK_SUBOPTIMAL_KHR) check(result, "present desktop image (resize requires swapchain recreation)");
    return submitted;
}
void Window::release()
{
    const auto d = device_.handles.device;
    if (swapchain_) vkDeviceWaitIdle(d); // window shutdown only
    targets_.clear();
    for (auto s : acquire_) if (s) vkDestroySemaphore(d, s, nullptr);
    for (auto s : present_) if (s) vkDestroySemaphore(d, s, nullptr);
    if (swapchain_) vkDestroySwapchainKHR(d, swapchain_, nullptr);
}
Window::~Window() { release(); }
} // namespace lindbergh::vk
