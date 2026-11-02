#include "openxr.hpp"
#include "../../runtime/stereo_projection.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <stdexcept>
#ifdef __ANDROID__
#include <unistd.h>
#endif
namespace lindbergh::vk {
void xr_check(XrResult result, const char *what)
{
    if (XR_FAILED(result)) throw std::runtime_error(std::string(what) + " failed (OpenXR " + std::to_string(result) + ")");
}
template<class T> static T function(XrInstance instance, const char *name)
{
    PFN_xrVoidFunction result = nullptr;
    xr_check(xrGetInstanceProcAddr(instance, name, &result), name);
    return reinterpret_cast<T>(result);
}
OpenXR::OpenXR(bool required, OpenXRPlatform platform, bool transfer_source)
    : OpenXR(OpenXRConfig{required,transfer_source},platform) {}
OpenXR::OpenXR(OpenXRConfig config,OpenXRPlatform platform)
    : config_(config),require_foveation_(config.require_foveation),transfer_source_(config.transfer_source)
{
    const bool required=config.require_foveation;
    try {
#ifdef __ANDROID__
        if (!platform.java_vm || !platform.activity)
            throw std::invalid_argument("Android OpenXR requires JavaVM and Activity");
        const auto initialize = function<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE, "xrInitializeLoaderKHR");
        XrLoaderInitInfoAndroidKHR loader{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loader.applicationVM = platform.java_vm;
        loader.applicationContext = platform.activity;
        xr_check(initialize(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader)),
                 "initialize Android OpenXR loader");
#else
        (void)platform;
#endif
        uint32_t n = 0;
        xr_check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr), "enumerate runtime extensions (install/select an OpenXR runtime)");
        std::vector<XrExtensionProperties> available(n, {XR_TYPE_EXTENSION_PROPERTIES});
        xr_check(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, available.data()), "runtime extensions");
        auto has = [&](const char *name) { return std::any_of(available.begin(),available.end(),[&](const auto &v){return std::strcmp(v.extensionName,name)==0;}); };
        if (!has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) throw std::runtime_error("OpenXR runtime does not provide XR_KHR_vulkan_enable2");
        std::vector<const char *> extensions{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
#ifdef __ANDROID__
        if (!has(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
            throw std::runtime_error("OpenXR runtime does not provide XR_KHR_android_create_instance");
        extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
        if (required) {
            for (const auto name : {XR_FB_FOVEATION_EXTENSION_NAME, XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME,
                                  XR_FB_FOVEATION_VULKAN_EXTENSION_NAME, XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME}) {
                if (!has(name)) throw std::runtime_error(std::string("Required OpenXR foveation extension unavailable: ")+name);
                extensions.push_back(name);
            }
            // Eye-tracked foveation where the headset has it: by default on
            // desktop OpenXR (Steam Frame), off on Android (Quest 3 has no eye
            // tracking). LINDBERGH_XR_EYE_FOVEATION=0|1 overrides.
#ifdef __ANDROID__
            bool want_eye=false;
#else
            bool want_eye=true;
#endif
            if(const char *v=std::getenv("LINDBERGH_XR_EYE_FOVEATION");v&&*v)want_eye=*v!='0';
            if(want_eye&&has(XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME)) {
                extensions.push_back(XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME);eye_tracked_foveation_=true;
            } else if(want_eye) std::fprintf(stderr,"[vulkan-xr] runtime has no XR_META_foveation_eye_tracked: fixed foveation\n");
        }
        performance_control_=has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
        if(performance_control_)extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
#ifdef __ANDROID__
        thread_control_=has(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
        if(thread_control_)extensions.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
#endif
        refresh_control_=has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        if (refresh_control_) extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
#ifdef __ANDROID__
        XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        android.applicationVM = platform.java_vm;
        android.applicationActivity = platform.activity;
        create.next = &android;
#endif
        std::strcpy(create.applicationInfo.applicationName,"Lindbergh Vulkan");
        // Match the shared GL host's tested API level. On the tested Quest
        // runtime, requesting 1.0.0 left chained density-map outputs empty.
        create.applicationInfo.apiVersion = XR_MAKE_VERSION(1,0,34);
        create.enabledExtensionCount=static_cast<uint32_t>(extensions.size()); create.enabledExtensionNames=extensions.data();
        xr_check(xrCreateInstance(&create,&instance_),"create OpenXR instance");
        XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO}; system.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        xr_check(xrGetSystem(instance_,&system,&system_),"OpenXR HMD system");
        if(eye_tracked_foveation_) {
            XrSystemFoveationEyeTrackedPropertiesMETA eye{XR_TYPE_SYSTEM_FOVEATION_EYE_TRACKED_PROPERTIES_META};
            XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};props.next=&eye;
            if(XR_FAILED(xrGetSystemProperties(instance_,system_,&props))||!eye.supportsFoveationEyeTracked) {
                eye_tracked_foveation_=false;
                std::fprintf(stderr,"[vulkan-xr] headset does not support eye-tracked foveation: fixed foveation\n");
            }
        }
        create_instance_=function<PFN_xrCreateVulkanInstanceKHR>(instance_,"xrCreateVulkanInstanceKHR");
        create_device_=function<PFN_xrCreateVulkanDeviceKHR>(instance_,"xrCreateVulkanDeviceKHR");
        get_device_=function<PFN_xrGetVulkanGraphicsDevice2KHR>(instance_,"xrGetVulkanGraphicsDevice2KHR");
        auto requirements_fn=function<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance_,"xrGetVulkanGraphicsRequirements2KHR");
        XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        xr_check(requirements_fn(instance_,system_,&requirements),"OpenXR Vulkan requirements");
        const auto version=XR_MAKE_VERSION(1,1,0);
        if(version<requirements.minApiVersionSupported || version>requirements.maxApiVersionSupported)
            throw std::runtime_error("OpenXR runtime does not support the backend's Vulkan 1.1 application version");
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        xr_check(xrGetInstanceProperties(instance_,&properties),"OpenXR runtime properties");
        std::fprintf(stderr,"[vulkan-xr] runtime %s; Vulkan enable2; required foveation %s\n",properties.runtimeName,required?"yes":"no");
    } catch (...) { release(); throw; }
}
DeviceOptions OpenXR::device_options()
{
    DeviceOptions options;
    options.require_foveation=require_foveation_;
    options.create_instance=[this](const VkInstanceCreateInfo *info,VkInstance *instance) {
        XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        create.systemId=system_; create.pfnGetInstanceProcAddr=vkGetInstanceProcAddr; create.vulkanCreateInfo=info;
        VkResult result=VK_ERROR_INITIALIZATION_FAILED;
        xr_check(create_instance_(instance_,&create,instance,&result),"OpenXR Vulkan instance factory"); return result;
    };
    options.select_physical=[this](VkInstance instance) {
        XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        info.systemId=system_; info.vulkanInstance=instance;
        VkPhysicalDevice physical;
        xr_check(get_device_(instance_,&info,&physical),"OpenXR selected Vulkan GPU"); return physical;
    };
    options.create_device=[this](VkPhysicalDevice physical,const VkDeviceCreateInfo *info,VkDevice *device) {
        XrVulkanDeviceCreateInfoKHR create{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        create.systemId=system_; create.pfnGetInstanceProcAddr=vkGetInstanceProcAddr;
        create.vulkanPhysicalDevice=physical; create.vulkanCreateInfo=info;
        VkResult result=VK_ERROR_INITIALIZATION_FAILED;
        xr_check(create_device_(instance_,&create,device,&result),"OpenXR Vulkan device factory"); return result;
    };
    return options;
}
void OpenXR::attach(Device &device)
{
    if(session_) throw std::logic_error("OpenXR session already attached");
    if(require_foveation_ && (!device.capabilities.density_map || !device.capabilities.density_map_non_subsampled))
        throw std::runtime_error("OpenXR foveation requires enabled Vulkan density maps for non-subsampled images");
    device_=&device;
    try {
        const auto &h=device.handles;
        XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        binding.instance=h.instance; binding.physicalDevice=h.physical; binding.device=h.device;
        binding.queueFamilyIndex=h.queue_family; binding.queueIndex=0;
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO}; create.next=&binding; create.systemId=system_;
        xr_check(xrCreateSession(instance_,&create,&session_),"Vulkan OpenXR session");
        if(performance_control_ && config_.sustained_cpu) {
            auto set=function<PFN_xrPerfSettingsSetPerformanceLevelEXT>(instance_,"xrPerfSettingsSetPerformanceLevelEXT");
            const auto level=config_.cpu_boost?XR_PERF_SETTINGS_LEVEL_BOOST_EXT:XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
            const auto result=set(session_,XR_PERF_SETTINGS_DOMAIN_CPU_EXT,level);
            std::fprintf(stderr,"[vulkan-xr] CPU %s performance request: %d\n",config_.cpu_boost?"boost":"sustained high",int(result));
        }
        if(performance_control_) {
            // The GPU otherwise stays at the runtime's default clock level; a
            // full-resolution stereo frame needs the sustained-high level.
            const char *v=std::getenv("LINDBERGH_GPU_LEVEL");
            if(!v||*v!='0') {
                auto set=function<PFN_xrPerfSettingsSetPerformanceLevelEXT>(instance_,"xrPerfSettingsSetPerformanceLevelEXT");
                const auto result=set(session_,XR_PERF_SETTINGS_DOMAIN_GPU_EXT,XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                std::fprintf(stderr,"[vulkan-xr] GPU sustained high performance request: %d\n",int(result));
            }
        }
#ifdef __ANDROID__
        if(thread_control_) {
            set_thread_=function<PFN_xrSetAndroidApplicationThreadKHR>(instance_,"xrSetAndroidApplicationThreadKHR");
            const auto result=set_thread_(session_,XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR,uint32_t(gettid()));
            std::fprintf(stderr,"[vulkan-xr] game thread %d registered as application main: %d\n",gettid(),int(result));
            renderer_thread();
        }
#endif
        if (refresh_control_) {
            request_refresh_=function<PFN_xrRequestDisplayRefreshRateFB>(instance_,"xrRequestDisplayRefreshRateFB");
            get_refresh_=function<PFN_xrGetDisplayRefreshRateFB>(instance_,"xrGetDisplayRefreshRateFB");
            requested_refresh_=config_.refresh;
            const auto enumerate=function<PFN_xrEnumerateDisplayRefreshRatesFB>(instance_,"xrEnumerateDisplayRefreshRatesFB");
            uint32_t count=0;xr_check(enumerate(session_,0,&count,nullptr),"enumerate refresh rate count");
            refresh_rates_.resize(count);if(count)xr_check(enumerate(session_,count,&count,refresh_rates_.data()),"enumerate refresh rates");
            std::sort(refresh_rates_.begin(),refresh_rates_.end());
        }
        XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL; space.poseInReferenceSpace.orientation.w=1;
        xr_check(xrCreateReferenceSpace(session_,&space,&space_),"OpenXR local space");
        if(config_.anchor_head)xr_check(xrCreateReferenceSpace(session_,&space,&base_space_),"OpenXR base local space");
        anchor_pending_=config_.anchor_head;
        if(config_.enable_input)xr_check(lg_xr_actions_create(&actions_,instance_,session_),"shared controller bindings");
        uint32_t count=0;
        xr_check(xrEnumerateViewConfigurationViews(instance_,system_,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr),"OpenXR view count");
        if(count!=2) throw std::runtime_error("OpenXR backend requires two stereo views");
        std::array<XrViewConfigurationView,2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        xr_check(xrEnumerateViewConfigurationViews(instance_,system_,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,views.data()),"OpenXR eye sizes");
        width_=std::max(views[0].recommendedImageRectWidth,views[1].recommendedImageRectWidth);
        height_=std::max(views[0].recommendedImageRectHeight,views[1].recommendedImageRectHeight);
        if(config_.height) {
            width_=config_.width?config_.width:uint32_t(uint64_t(width_)*config_.height/height_);
            height_=config_.height;
        } else if(config_.width)width_=config_.width;
        for(const auto &view:views)if(!width_ || !height_ || width_>view.maxImageRectWidth || height_>view.maxImageRectHeight)
            throw std::runtime_error("Configured OpenXR resolution exceeds runtime eye limits");
        xr_check(xrEnumerateSwapchainFormats(session_,0,&count,nullptr),"OpenXR formats");
        std::vector<int64_t> formats(count);
        xr_check(xrEnumerateSwapchainFormats(session_,count,&count,formats.data()),"OpenXR formats");
        VkFormat format=VK_FORMAT_UNDEFINED;
        for(auto preferred:{VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM})
            if(std::find(formats.begin(),formats.end(),int64_t(preferred))!=formats.end()) {format=preferred;break;}
        if(format==VK_FORMAT_UNDEFINED) throw std::runtime_error("No supported OpenXR Vulkan color format");
        XrSwapchainCreateInfoFoveationFB fov{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
        fov.flags=XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
        XrSwapchainCreateInfo swap{XR_TYPE_SWAPCHAIN_CREATE_INFO}; swap.next=require_foveation_?&fov:nullptr;
        // The guest writes sRGB-encoded colour, as on the cabinet. Like the GL
        // host (GL_FRAMEBUFFER_SRGB off), store those bytes unchanged: render
        // through a UNORM view of the sRGB swapchain, which the compositor then
        // decodes once. An sRGB view would encode them again (washed out).
        const VkFormat render_format=format==VK_FORMAT_R8G8B8A8_SRGB?VK_FORMAT_R8G8B8A8_UNORM:
                                     format==VK_FORMAT_B8G8R8A8_SRGB?VK_FORMAT_B8G8R8A8_UNORM:format;
        swap.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | (transfer_source_ ? XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT : 0) |
                        (render_format!=format ? XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT : 0); swap.format=format;
        swap.sampleCount=1; swap.width=width_; swap.height=height_; swap.faceCount=1; swap.arraySize=2; swap.mipCount=1;
        xr_check(xrCreateSwapchain(session_,&swap,&swapchain_),"OpenXR stereo array swapchain");
        if(require_foveation_) {
            create_profile_=function<PFN_xrCreateFoveationProfileFB>(instance_,"xrCreateFoveationProfileFB");
            destroy_profile_=function<PFN_xrDestroyFoveationProfileFB>(instance_,"xrDestroyFoveationProfileFB");
            update_swapchain_=function<PFN_xrUpdateSwapchainFB>(instance_,"xrUpdateSwapchainFB");
            set_foveation(config_.foveation_level);
        }
        xr_check(xrEnumerateSwapchainImages(swapchain_,0,&count,nullptr),"OpenXR swapchain image count");
        std::vector<XrSwapchainImageVulkanKHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        std::vector<XrSwapchainImageFoveationVulkanFB> density(count,{XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB});
        if(require_foveation_) for(uint32_t i=0;i<count;++i) images[i].next=&density[i];
        xr_check(xrEnumerateSwapchainImages(swapchain_,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),"OpenXR Vulkan images");
        for(uint32_t i=0;i<count;++i) {
            VkImageView map=VK_NULL_HANDLE;
            if(require_foveation_) {
                if(!density[i].image || !density[i].width || !density[i].height) throw std::runtime_error("OpenXR did not provide the required Vulkan density map");
                if(i && density_extent_!=std::array<uint32_t,2>{density[i].width,density[i].height})
                    throw std::runtime_error("OpenXR density-map extents differ across swapchain images");
                density_extent_={density[i].width,density[i].height};
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=density[i].image;
                view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; view.format=VK_FORMAT_R8G8_UNORM;
                view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,2};
                check(vkCreateImageView(h.device,&view,nullptr,&map),"OpenXR density-map image view");
                density_views_.push_back(map);
            }
            TargetInfo info{width_,height_,2,render_format,images[i].image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,map};
            targets_.push_back(std::make_unique<Target>(device,info));
        }
        foveation_=require_foveation_;
        std::fprintf(stderr,"[vulkan-xr] direct two-layer world targets; density-map attachments %s; swapchain format %d rendered as %d\n",foveation_?"configured":"disabled by desktop policy",int(format),int(render_format));
    } catch (...) { detach(); throw; }
}
bool OpenXR::input(LgXrActionSample &sample)
{
    sample={};
    if(!config_.enable_input||!frame_open_||session_state_!=XR_SESSION_STATE_FOCUSED)return false;
    const auto result=lg_xr_actions_sample(&actions_,space_,frame_.predictedDisplayTime,&sample);
    if(result==XR_SESSION_NOT_FOCUSED)return false;
    xr_check(result,"shared controller input");return result==XR_SUCCESS;
}
void OpenXR::rumble(int hand,float amplitude,int milliseconds)
{
    if(config_.enable_input&&session_state_==XR_SESSION_STATE_FOCUSED)
        lg_xr_actions_rumble(&actions_,hand,amplitude,milliseconds);
}
void OpenXR::renderer_thread()
{
#ifdef __ANDROID__
    const auto tid=uint32_t(gettid());
    if(!session_||!set_thread_||renderer_tid_==tid)return;
    const auto result=set_thread_(session_,XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR,tid);
    if(XR_SUCCEEDED(result))renderer_tid_=tid;
    std::fprintf(stderr,"[vulkan-xr] render owner thread %u registered: %d\n",tid,int(result));
#endif
}
void OpenXR::poll()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    XrResult result;
    while((result=xrPollEvent(instance_,&event))==XR_SUCCESS) {
        if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto &change=*reinterpret_cast<XrEventDataSessionStateChanged *>(&event);
            if(change.session==session_) {
                session_state_=change.state;
                if(config_.anchor_head&&change.state==XR_SESSION_STATE_FOCUSED)anchor_pending_=true;
                std::fprintf(stderr,"[vulkan-xr] session state %d\n",int(change.state));
                if(change.state==XR_SESSION_STATE_READY && !running_) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    xr_check(xrBeginSession(session_,&begin),"begin OpenXR session"); running_=true;
                    if (request_refresh_ && requested_refresh_>0) {
                        const auto result=request_refresh_(session_,requested_refresh_);
                        float observed=0;
                        const auto got=get_refresh_(session_,&observed);
                        std::fprintf(stderr,"[vulkan-xr] requested %.2f Hz: %d; reported %.2f Hz: %d\n",requested_refresh_,int(result),observed,int(got));
                    }
                } else if(change.state==XR_SESSION_STATE_STOPPING) {
                    xr_check(xrEndSession(session_),"end OpenXR session"); running_=false;
                } else if(change.state==XR_SESSION_STATE_EXITING||change.state==XR_SESSION_STATE_LOSS_PENDING) exiting_=true;
            }
        } else if(event.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            if(config_.anchor_head)anchor_pending_=true;
        } else if(event.type==XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            const auto &rate=*reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB *>(&event);
            std::fprintf(stderr,"[vulkan-xr] runtime refresh changed %.2f -> %.2f Hz\n",rate.fromDisplayRefreshRate,rate.toDisplayRefreshRate);
        } else if(event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) exiting_=true;
        event={XR_TYPE_EVENT_DATA_BUFFER};
    }
    if(result!=XR_EVENT_UNAVAILABLE) xr_check(result,"poll OpenXR events");
}
// LINDBERGH_XR_TIMING=1: where the render thread waits on the runtime, every 300 frames.
static void xr_time(int what,std::chrono::steady_clock::time_point a,std::chrono::steady_clock::time_point b)
{
    static const bool on=[]{const auto *v=std::getenv("LINDBERGH_XR_TIMING");return v&&*v=='1';}();
    if(!on)return;
    static double sum[4];static unsigned n;
    sum[what]+=std::chrono::duration<double,std::milli>(b-a).count();
    if(what==3&&++n==300) {
        std::fprintf(stderr,"[xr-timing] ms per frame: xrWaitFrame %.2f, xrBeginFrame %.2f, xrWaitSwapchainImage %.2f, xrEndFrame %.2f\n",
                     sum[0]/n,sum[1]/n,sum[2]/n,sum[3]/n);
        sum[0]=sum[1]=sum[2]=sum[3]=0;n=0;
    }
}
Target *OpenXR::begin_frame()
{
    if(!session_||frame_open_) throw std::logic_error("Invalid OpenXR frame begin");
    poll(); if(!running_||exiting_) return nullptr;
    const auto t0=std::chrono::steady_clock::now();
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO}; xr_check(xrWaitFrame(session_,&wait,&frame_),"wait OpenXR frame");
    const auto t1=std::chrono::steady_clock::now();
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO}; xr_check(xrBeginFrame(session_,&begin),"begin OpenXR frame"); frame_open_=true;
    xr_time(0,t0,t1);xr_time(1,t1,std::chrono::steady_clock::now());
    if(!frame_.shouldRender) return nullptr;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO}; locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime=frame_.predictedDisplayTime;locate.space=anchor_pending_?base_space_:space_;
    XrViewState state{XR_TYPE_VIEW_STATE}; uint32_t count=0;
    xr_check(xrLocateViews(session_,&locate,&state,2,&count,views_.data()),"locate OpenXR views");
    if(count!=2 || !(state.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return nullptr;
    if(anchor_pending_) {
        if(!(state.viewStateFlags&XR_VIEW_STATE_POSITION_VALID_BIT))return nullptr;
        const auto &q=views_[0].pose.orientation;
        const float quaternion[]={q.x,q.y,q.z,q.w},back[]={0,0,-1};float forward[3];
        lg_stereo_rotate(quaternion,back,forward);
        const float yaw=std::atan2(-forward[0],-forward[2]);
        XrReferenceSpaceCreateInfo anchor{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};anchor.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;
        anchor.poseInReferenceSpace.orientation={0,std::sin(yaw/2),0,std::cos(yaw/2)};
        anchor.poseInReferenceSpace.position={(views_[0].pose.position.x+views_[1].pose.position.x)/2,
                                            (views_[0].pose.position.y+views_[1].pose.position.y)/2,
                                            (views_[0].pose.position.z+views_[1].pose.position.z)/2};
        XrSpace replacement;
        xr_check(xrCreateReferenceSpace(session_,&anchor,&replacement),"anchor OpenXR camera on head");
        xrDestroySpace(space_);space_=replacement;anchor_pending_=false;
        locate.space=space_;
        xr_check(xrLocateViews(session_,&locate,&state,2,&count,views_.data()),"locate anchored OpenXR views");
        if(count!=2 || !(state.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT))return nullptr;
        std::fprintf(stderr,"[vulkan-xr] camera anchored on head at %.2f %.2f %.2f m; yaw %.1f degrees\n",anchor.poseInReferenceSpace.position.x,anchor.poseInReferenceSpace.position.y,anchor.poseInReferenceSpace.position.z,yaw*57.2957795f);
    }
    if(config_.symmetric_fov)for(auto &view:views_) {
        const auto tx=std::max(std::fabs(std::tan(view.fov.angleLeft)),std::fabs(std::tan(view.fov.angleRight)));
        const auto ty=std::max(std::fabs(std::tan(view.fov.angleDown)),std::fabs(std::tan(view.fov.angleUp)));
        view.fov={-std::atan(tx),std::atan(tx),std::atan(ty),-std::atan(ty)};
    }
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xr_check(xrAcquireSwapchainImage(swapchain_,&acquire,&image_),"acquire OpenXR eye array"); acquired_=true;
    XrSwapchainImageWaitInfo ready{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; ready.timeout=XR_INFINITE_DURATION;
    const auto t2=std::chrono::steady_clock::now();
    xr_check(xrWaitSwapchainImage(swapchain_,&ready),"wait OpenXR eye array");
    xr_time(2,t2,std::chrono::steady_clock::now());
    return targets_.at(image_).get();
}
void OpenXR::set_foveation(int level)
{
    if(!create_profile_||!update_swapchain_||!swapchain_)return;
    static const XrFoveationLevelFB levels[]={XR_FOVEATION_LEVEL_NONE_FB,XR_FOVEATION_LEVEL_LOW_FB,XR_FOVEATION_LEVEL_MEDIUM_FB,XR_FOVEATION_LEVEL_HIGH_FB};
    XrFoveationLevelProfileCreateInfoFB info{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    info.level=levels[level<0||level>3?2:level]; info.dynamic=XR_FOVEATION_DYNAMIC_DISABLED_FB;
    // The level sets how far the periphery drops; with eye tracking its
    // centre is where the player looks rather than the lens centre.
    XrFoveationEyeTrackedProfileCreateInfoMETA eye{XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META};
    if(eye_tracked_foveation_&&level>0)info.next=&eye;
    XrFoveationProfileCreateInfoFB profile{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB}; profile.next=&info;
    XrFoveationProfileFB created=XR_NULL_HANDLE;
    xr_check(create_profile_(session_,&profile,&created),"OpenXR foveation profile");
    XrSwapchainStateFoveationFB state{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB}; state.profile=created;
    xr_check(update_swapchain_(swapchain_,reinterpret_cast<XrSwapchainStateBaseHeaderFB *>(&state)),"OpenXR foveation swapchain state");
    if(profile_&&destroy_profile_)destroy_profile_(profile_);
    profile_=created;
    std::fprintf(stderr,"[vulkan-xr] foveation level %d%s\n",level,info.next?", eye-tracked":"");
}
void OpenXR::request_refresh(float hz)
{
    requested_refresh_=hz;
    if(request_refresh_&&running_){const auto result=request_refresh_(session_,hz);
        std::fprintf(stderr,"[vulkan-xr] menu refresh request %.2f Hz: %d\n",hz,int(result));}
}
void OpenXR::end_frame(bool rendered)
{
    if(!frame_open_) return;
    if(rendered&&!acquired_) throw std::logic_error("OpenXR frame has no acquired eye image");
    if(acquired_) { XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xr_check(xrReleaseSwapchainImage(swapchain_,&release),"release OpenXR eye array"); acquired_=false; }
    std::array<XrCompositionLayerProjectionView,2> views{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
    for(unsigned i=0;i<2;++i) {
        views[i].pose=views_[i].pose; views[i].fov=views_[i].fov;
        views[i].subImage.swapchain=swapchain_; views[i].subImage.imageRect.extent={int32_t(width_),int32_t(height_)}; views[i].subImage.imageArrayIndex=i;
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION}; projection.space=space_; projection.viewCount=2; projection.views=views.data();
    const auto layer=reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO}; end.displayTime=frame_.predictedDisplayTime;
    end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE; end.layerCount=rendered?1:0; end.layers=&layer;
    const auto t3=std::chrono::steady_clock::now();
    xr_check(xrEndFrame(session_,&end),"submit OpenXR frame"); frame_open_=false;
    xr_time(3,t3,std::chrono::steady_clock::now());
}
void OpenXR::detach()
{
    if(device_) vkDeviceWaitIdle(device_->handles.device); // session shutdown, not frame submission
    targets_.clear();
    if(device_) for(auto view:density_views_) vkDestroyImageView(device_->handles.device,view,nullptr);
    density_views_.clear();density_extent_={};
    if(profile_&&destroy_profile_) destroy_profile_(profile_); profile_=XR_NULL_HANDLE;
    if(swapchain_) xrDestroySwapchain(swapchain_); swapchain_=XR_NULL_HANDLE;
    if(space_) xrDestroySpace(space_); space_=XR_NULL_HANDLE;
    if(base_space_)xrDestroySpace(base_space_);base_space_=XR_NULL_HANDLE;
    lg_xr_actions_destroy(&actions_);
    if(session_) xrDestroySession(session_); session_=XR_NULL_HANDLE;
    renderer_tid_=0;
#ifdef __ANDROID__
    set_thread_=nullptr;
#endif
    refresh_rates_.clear();requested_refresh_=0; session_state_=XR_SESSION_STATE_UNKNOWN;
    device_=nullptr; running_=false; frame_open_=false; acquired_=false; foveation_=false;
}
void OpenXR::release() { detach(); if(instance_) xrDestroyInstance(instance_); instance_=XR_NULL_HANDLE; }
OpenXR::~OpenXR() { release(); }
} // namespace lindbergh::vk
