#include "openxr.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
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
OpenXR::OpenXR(bool required, OpenXRPlatform platform) : require_foveation_(required)
{
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
        }
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
#ifdef __ANDROID__
        XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        android.applicationVM = platform.java_vm;
        android.applicationActivity = platform.activity;
        create.next = &android;
#endif
        std::strcpy(create.applicationInfo.applicationName,"Lindbergh Vulkan");
        create.applicationInfo.apiVersion = XR_MAKE_VERSION(1,0,0);
        create.enabledExtensionCount=static_cast<uint32_t>(extensions.size()); create.enabledExtensionNames=extensions.data();
        xr_check(xrCreateInstance(&create,&instance_),"create OpenXR instance");
        XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO}; system.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        xr_check(xrGetSystem(instance_,&system,&system_),"OpenXR HMD system");
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
        XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL; space.poseInReferenceSpace.orientation.w=1;
        xr_check(xrCreateReferenceSpace(session_,&space,&space_),"OpenXR local space");
        uint32_t count=0;
        xr_check(xrEnumerateViewConfigurationViews(instance_,system_,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr),"OpenXR view count");
        if(count!=2) throw std::runtime_error("OpenXR backend requires two stereo views");
        std::array<XrViewConfigurationView,2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        xr_check(xrEnumerateViewConfigurationViews(instance_,system_,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,views.data()),"OpenXR eye sizes");
        width_=std::max(views[0].recommendedImageRectWidth,views[1].recommendedImageRectWidth);
        height_=std::max(views[0].recommendedImageRectHeight,views[1].recommendedImageRectHeight);
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
        swap.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT; swap.format=format;
        swap.sampleCount=1; swap.width=width_; swap.height=height_; swap.faceCount=1; swap.arraySize=2; swap.mipCount=1;
        xr_check(xrCreateSwapchain(session_,&swap,&swapchain_),"OpenXR stereo array swapchain");
        if(require_foveation_) {
            auto make=function<PFN_xrCreateFoveationProfileFB>(instance_,"xrCreateFoveationProfileFB");
            destroy_profile_=function<PFN_xrDestroyFoveationProfileFB>(instance_,"xrDestroyFoveationProfileFB");
            auto update=function<PFN_xrUpdateSwapchainFB>(instance_,"xrUpdateSwapchainFB");
            XrFoveationLevelProfileCreateInfoFB level{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
            level.level=XR_FOVEATION_LEVEL_MEDIUM_FB; level.dynamic=XR_FOVEATION_DYNAMIC_DISABLED_FB;
            XrFoveationProfileCreateInfoFB profile{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB}; profile.next=&level;
            xr_check(make(session_,&profile,&profile_),"OpenXR foveation profile");
            XrSwapchainStateFoveationFB state{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB}; state.profile=profile_;
            xr_check(update(swapchain_,reinterpret_cast<XrSwapchainStateBaseHeaderFB *>(&state)),"OpenXR foveation swapchain state");
        }
        xr_check(xrEnumerateSwapchainImages(swapchain_,0,&count,nullptr),"OpenXR swapchain image count");
        std::vector<XrSwapchainImageVulkanKHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        std::vector<XrSwapchainImageFoveationVulkanFB> density(count,{XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB});
        if(require_foveation_) for(uint32_t i=0;i<count;++i) images[i].next=&density[i];
        xr_check(xrEnumerateSwapchainImages(swapchain_,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),"OpenXR Vulkan images");
        for(uint32_t i=0;i<count;++i) {
            VkImageView map=VK_NULL_HANDLE;
            if(require_foveation_) {
                if(!density[i].image) throw std::runtime_error("OpenXR did not provide the required Vulkan density map");
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=density[i].image;
                view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; view.format=VK_FORMAT_R8G8_UNORM;
                view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,2};
                check(vkCreateImageView(h.device,&view,nullptr,&map),"OpenXR density-map image view");
                density_views_.push_back(map);
            }
            TargetInfo info{width_,height_,2,format,images[i].image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,map};
            targets_.push_back(std::make_unique<Target>(device,info));
        }
        foveation_=require_foveation_;
        std::fprintf(stderr,"[vulkan-xr] direct two-layer world targets; density-map attachments %s\n",foveation_?"configured":"disabled by desktop policy");
    } catch (...) { detach(); throw; }
}
void OpenXR::poll()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    XrResult result;
    while((result=xrPollEvent(instance_,&event))==XR_SUCCESS) {
        if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto &change=*reinterpret_cast<XrEventDataSessionStateChanged *>(&event);
            if(change.session==session_) {
                if(change.state==XR_SESSION_STATE_READY && !running_) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    xr_check(xrBeginSession(session_,&begin),"begin OpenXR session"); running_=true;
                } else if(change.state==XR_SESSION_STATE_STOPPING) {
                    xr_check(xrEndSession(session_),"end OpenXR session"); running_=false;
                } else if(change.state==XR_SESSION_STATE_EXITING||change.state==XR_SESSION_STATE_LOSS_PENDING) exiting_=true;
            }
        } else if(event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) exiting_=true;
        event={XR_TYPE_EVENT_DATA_BUFFER};
    }
    if(result!=XR_EVENT_UNAVAILABLE) xr_check(result,"poll OpenXR events");
}
Target *OpenXR::begin_frame()
{
    if(!session_||frame_open_) throw std::logic_error("Invalid OpenXR frame begin");
    poll(); if(!running_||exiting_) return nullptr;
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO}; xr_check(xrWaitFrame(session_,&wait,&frame_),"wait OpenXR frame");
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO}; xr_check(xrBeginFrame(session_,&begin),"begin OpenXR frame"); frame_open_=true;
    if(!frame_.shouldRender) return nullptr;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO}; locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime=frame_.predictedDisplayTime; locate.space=space_;
    XrViewState state{XR_TYPE_VIEW_STATE}; uint32_t count=0;
    xr_check(xrLocateViews(session_,&locate,&state,2,&count,views_.data()),"locate OpenXR views");
    if(count!=2 || !(state.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return nullptr;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xr_check(xrAcquireSwapchainImage(swapchain_,&acquire,&image_),"acquire OpenXR eye array"); acquired_=true;
    XrSwapchainImageWaitInfo ready{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; ready.timeout=XR_INFINITE_DURATION;
    xr_check(xrWaitSwapchainImage(swapchain_,&ready),"wait OpenXR eye array");
    return targets_.at(image_).get();
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
    xr_check(xrEndFrame(session_,&end),"submit OpenXR frame"); frame_open_=false;
}
void OpenXR::detach()
{
    if(device_) vkDeviceWaitIdle(device_->handles.device); // session shutdown, not frame submission
    targets_.clear();
    if(device_) for(auto view:density_views_) vkDestroyImageView(device_->handles.device,view,nullptr);
    density_views_.clear();
    if(profile_&&destroy_profile_) destroy_profile_(profile_); profile_=XR_NULL_HANDLE;
    if(swapchain_) xrDestroySwapchain(swapchain_); swapchain_=XR_NULL_HANDLE;
    if(space_) xrDestroySpace(space_); space_=XR_NULL_HANDLE;
    if(session_) xrDestroySession(session_); session_=XR_NULL_HANDLE;
    device_=nullptr; running_=false; frame_open_=false; acquired_=false; foveation_=false;
}
void OpenXR::release() { detach(); if(instance_) xrDestroyInstance(instance_); instance_=XR_NULL_HANDLE; }
OpenXR::~OpenXR() { release(); }
} // namespace lindbergh::vk
