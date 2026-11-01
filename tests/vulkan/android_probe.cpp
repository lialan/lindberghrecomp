// Source-owned Android Activity probe: no game assets or GL context.
#include "openxr.hpp"
#include "frame_ring.hpp"
#include <android/native_activity.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <string>
using namespace lindbergh::vk;
namespace {
struct Probe {
    std::atomic<bool> stop{false};
    std::thread worker;
    void run(ANativeActivity *activity) {
        JNIEnv *env = nullptr;
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        try {
            // Quest default: exercise the required foveation path. An emulator
            // can explicitly opt out via `am start --ez foveation false`.
            auto activity_class=env->GetObjectClass(activity->clazz);
            auto get_intent=env->GetMethodID(activity_class,"getIntent","()Landroid/content/Intent;");
            auto intent=env->CallObjectMethod(activity->clazz,get_intent);
            auto intent_class=env->GetObjectClass(intent);
            auto get_boolean=env->GetMethodID(intent_class,"getBooleanExtra","(Ljava/lang/String;Z)Z");
            auto key=env->NewStringUTF("foveation");
            const bool required=env->CallBooleanMethod(intent,get_boolean,key,JNI_TRUE);
            env->DeleteLocalRef(key);
            auto get_string=env->GetMethodID(intent_class,"getStringExtra","(Ljava/lang/String;)Ljava/lang/String;");
            key=env->NewStringUTF("probe_run");
            auto value=static_cast<jstring>(env->CallObjectMethod(intent,get_string,key));
            std::string run="unspecified";
            if (value) {
                const char *text=env->GetStringUTFChars(value,nullptr);
                if(text) {run=text;env->ReleaseStringUTFChars(value,text);}
                env->DeleteLocalRef(value);
            }
            env->DeleteLocalRef(key);env->DeleteLocalRef(intent_class);
            env->DeleteLocalRef(intent);env->DeleteLocalRef(activity_class);
            if(env->ExceptionCheck()) {env->ExceptionClear();throw std::runtime_error("Cannot read probe Activity options");}
            __android_log_print(ANDROID_LOG_INFO,"LindberghVulkanProbe","START %s: direct Vulkan/OpenXR; required foveation %s",run.c_str(),required?"yes":"no");
            OpenXR xr(required, {activity->vm, activity->clazz});
            auto device = Device::create(xr.device_options());
            __android_log_print(ANDROID_LOG_INFO,"LindberghVulkanProbe","GPU: %s; multiview %d; density %d; nonsubsampled %d",
                                device->capabilities.properties.deviceName,device->capabilities.multiview,
                                device->capabilities.density_map,device->capabilities.density_map_non_subsampled);
            // Always detach the XR session before releasing its Vulkan device.
            struct Session { OpenXR &xr; ~Session() { xr.detach(); } } session{xr};
            xr.attach(*device);
            PipelineCache cache(*device, "");
            FrameRing ring(*device);
            std::unordered_map<Target *, std::unique_ptr<Renderer>> renderers;
            // Complete outstanding work before any renderer/frame storage is destroyed.
            struct Idle { Device &device; ~Idle() { vkDeviceWaitIdle(device.handles.device); } } idle{*device};
            Matrices matrices{};
            for (auto &m : matrices.view_projection) for (unsigned i=0;i<4;++i) m[i*5]=1;
            Vertex triangle[]={{{-0.6f,-0.6f,0.5f,1},{1,0,0,1}},
                               {{0.6f,-0.6f,0.5f,1},{0,1,0,1}},{{0,0.6f,0.5f,1},{0,0,1,1}}};
            RasterState raster;
            unsigned frames=0;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
            while (!stop && !xr.exiting() && frames<120 && std::chrono::steady_clock::now()<deadline) {
                auto *target=xr.begin_frame();
                if (target) {
                    if (!frames) __android_log_print(ANDROID_LOG_INFO,"LindberghVulkanProbe","FIRST: %ux%u per eye, %u layers, foveation %s",
                                                     target->info.width,target->info.height,target->info.layers,xr.foveation_active()?"active":"disabled");
                    auto &renderer=renderers[target];
                    if (!renderer) { renderer=std::make_unique<Renderer>(*device,*target,cache);renderer->prepare(raster); }
                    auto command=ring.begin();
                    renderer->begin(command,{0.02f,0.02f,0.05f,1});
                    renderer->draw(ring,triangle,3,matrices,raster);renderer->end();
                    ring.submit();xr.end_frame(true);++frames;
                } else {
                    xr.end_frame(false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            if (frames != 120) throw std::runtime_error("XR probe did not present 120 frames before stop/timeout");
            __android_log_print(ANDROID_LOG_INFO,"LindberghVulkanProbe","PASS: 120 Vulkan OpenXR stereo frames; foveation %s; no game or performance validation implied",xr.foveation_active()?"active":"disabled");
        } catch (const std::exception &e) {
            __android_log_print(ANDROID_LOG_ERROR,"LindberghVulkanProbe","FAIL: %s",e.what());
        }
        activity->vm->DetachCurrentThread();
    }
};
void destroy(ANativeActivity *activity) {
    auto *probe=static_cast<Probe *>(activity->instance);
    probe->stop=true;
    if (probe->worker.joinable()) probe->worker.join();
    delete probe;activity->instance=nullptr;
}
}
extern "C" void ANativeActivity_onCreate(ANativeActivity *activity,void *,size_t) {
    auto *probe=new Probe;
    activity->instance=probe;activity->callbacks->onDestroy=destroy;
    probe->worker=std::thread([probe,activity]{probe->run(activity);});
}
