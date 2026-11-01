// Source-owned Android Activity probe: no game assets or GL context.
#include "openxr.hpp"
#include "frame_ring.hpp"
#include <android/native_activity.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
using namespace lindbergh::vk;
namespace {
struct Probe {
    std::atomic<bool> stop{false};
    std::thread worker;
    void run(ANativeActivity *activity) {
        JNIEnv *env = nullptr;
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        try {
            OpenXR xr(false, {activity->vm, activity->clazz});
            auto device = Device::create(xr.device_options());
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
            __android_log_print(ANDROID_LOG_INFO,"LindberghVulkanProbe","PASS: 120 Vulkan OpenXR stereo frames; no game or performance validation implied");
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
