#pragma once
#include "renderer.hpp"
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace lindbergh::vk {
// Common Vulkan/OpenXR host, independent of SDL and the guest's graphics ABI.
class OpenXR {
public:
    explicit OpenXR(bool require_foveation = false);
    ~OpenXR();
    OpenXR(const OpenXR &) = delete;
    OpenXR &operator=(const OpenXR &) = delete;
    DeviceOptions device_options();
    // The device must outlive the attached session; detach before destroying it.
    void attach(Device &device);
    void detach();
    // A null target still requires end_frame(false): shouldRender can be false.
    // Returns null before READY without opening a frame.
    Target *begin_frame();
    // Submit Vulkan commands before calling this; no per-frame CPU GPU wait.
    void end_frame(bool rendered);
    const std::array<XrView, 2> &views() const { return views_; }
    XrTime display_time() const { return frame_.predictedDisplayTime; }
    bool exiting() const { return exiting_; }
    bool frame_open() const { return frame_open_; }
    bool foveation_active() const { return foveation_; }
private:
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    XrFoveationProfileFB profile_ = XR_NULL_HANDLE;
    Device *device_ = nullptr;
    bool require_foveation_, foveation_ = false, running_ = false, exiting_ = false, frame_open_ = false, acquired_ = false;
    uint32_t width_ = 0, height_ = 0, image_ = 0;
    XrFrameState frame_{XR_TYPE_FRAME_STATE};
    std::array<XrView,2> views_{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
    std::vector<std::unique_ptr<Target>> targets_;
    std::vector<VkImageView> density_views_;
    PFN_xrCreateVulkanInstanceKHR create_instance_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_device_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device_ = nullptr;
    PFN_xrDestroyFoveationProfileFB destroy_profile_ = nullptr;
    void poll();
    void release();
};
void xr_check(XrResult result, const char *what);
} // namespace lindbergh::vk
