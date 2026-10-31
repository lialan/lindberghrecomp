#include "openxr.hpp"
#include <cstdio>
int main() {
    try {
        lindbergh::vk::OpenXR xr;
        auto device=lindbergh::vk::Device::create(xr.device_options());
        xr.attach(*device);
        std::puts("OpenXR Vulkan session and stereo swapchain created; no rendering/hardware validation implied");
        xr.detach(); return 0;
    } catch(const std::exception &e) { std::fprintf(stderr,"OpenXR Vulkan unavailable: %s\n",e.what()); return 2; }
}
