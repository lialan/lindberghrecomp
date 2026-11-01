#pragma once
#include "renderer.hpp"
#include "../../runtime/xr_actions.h"
#ifdef __ANDROID__
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#endif
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace lindbergh::vk {
// Android callers retain the Activity reference until OpenXR is destroyed.
// Opaque fields keep JNI out of the desktop host interface.
struct OpenXRPlatform { void *java_vm = nullptr; void *activity = nullptr; };
struct OpenXRConfig {
    bool require_foveation=false,transfer_source=false,anchor_head=false,symmetric_fov=false;
    uint32_t width=0,height=0; // zero: runtime recommendation; width-only zero: preserve recommended aspect
    float refresh=120; // request only; predicted runtime timing remains authoritative
    bool sustained_cpu=true,cpu_boost=false,enable_input=false;
    int foveation_level=2; // 0 off, 1 low, 2 medium, 3 high (keep last: positional initializers)
};
// Common Vulkan/OpenXR host, independent of SDL and the guest's graphics ABI.
class OpenXR {
public:
    explicit OpenXR(bool require_foveation = false, OpenXRPlatform platform = {}, bool transfer_source = false);
    explicit OpenXR(OpenXRConfig config,OpenXRPlatform platform = {});
    ~OpenXR();
    OpenXR(const OpenXR &) = delete;
    OpenXR &operator=(const OpenXR &) = delete;
    DeviceOptions device_options();
    // The device must outlive the attached session; detach before destroying it.
    void attach(Device &device);
    void detach();
    bool input(LgXrActionSample &sample);
    void rumble(int hand,float amplitude,int milliseconds);
    const std::vector<float> &refresh_rates() const {return refresh_rates_;}
    void request_refresh(float hz);
    void recenter() {if(config_.anchor_head)anchor_pending_=true;}
    void renderer_thread(); // call when the shared worker takes rendering ownership
    // A null target still requires end_frame(false): shouldRender can be false.
    // Returns null before READY without opening a frame.
    Target *begin_frame();
    // Submit Vulkan commands before calling this; no per-frame CPU GPU wait.
    void end_frame(bool rendered);
    const std::array<XrView, 2> &views() const { return views_; }
    XrDuration display_period() const { return frame_.predictedDisplayPeriod; }
    XrTime display_time() const { return frame_.predictedDisplayTime; }
    XrSessionState session_state() const { return session_state_; }
    bool exiting() const { return exiting_; }
    bool frame_open() const { return frame_open_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool foveation_active() const { return foveation_; }
    // Changes the swapchain's fixed foveation level (0 off .. 3 high) live.
    void set_foveation(int level);
    // Borrowed views remain valid until detach. Use only the current acquired
    // image's map for GPU work; other views may be used to prepare framebuffers.
    const std::vector<VkImageView> &density_maps() const { return density_views_; }
    std::array<uint32_t,2> density_extent() const { return density_extent_; }
    const std::vector<std::unique_ptr<Target>> &render_targets() const {return targets_;}
private:
    LgXrActions actions_{};
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE,base_space_ = XR_NULL_HANDLE;
    OpenXRConfig config_;
    bool anchor_pending_=true;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    XrFoveationProfileFB profile_ = XR_NULL_HANDLE;
    Device *device_ = nullptr;
    bool require_foveation_, transfer_source_, foveation_ = false, running_ = false, exiting_ = false, frame_open_ = false, acquired_ = false;
    uint32_t width_ = 0, height_ = 0, image_ = 0;
    XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
    XrFrameState frame_{XR_TYPE_FRAME_STATE};
    std::array<XrView,2> views_{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
    std::vector<std::unique_ptr<Target>> targets_;
    std::vector<VkImageView> density_views_;
    std::array<uint32_t,2> density_extent_{};
    PFN_xrCreateVulkanInstanceKHR create_instance_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_device_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device_ = nullptr;
    bool refresh_control_ = false,performance_control_=false,thread_control_=false;
    // XR_META_foveation_eye_tracked: the density map follows the gaze
    // (Steam Frame); otherwise the FB level profile is fixed.
    bool eye_tracked_foveation_=false;
    uint32_t renderer_tid_=0;
#ifdef __ANDROID__
    PFN_xrSetAndroidApplicationThreadKHR set_thread_=nullptr;
#endif
    std::vector<float> refresh_rates_;
    float requested_refresh_ = 0;
    PFN_xrRequestDisplayRefreshRateFB request_refresh_ = nullptr;
    PFN_xrGetDisplayRefreshRateFB get_refresh_ = nullptr;
    PFN_xrDestroyFoveationProfileFB destroy_profile_ = nullptr;
    PFN_xrCreateFoveationProfileFB create_profile_ = nullptr;
    PFN_xrUpdateSwapchainFB update_swapchain_ = nullptr;
    void poll();
    void release();
};
void xr_check(XrResult result, const char *what);
} // namespace lindbergh::vk
