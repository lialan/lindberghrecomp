#pragma once
#include "renderer.hpp"
struct SDL_Window;
namespace lindbergh::vk {
DeviceOptions desktop_options(SDL_Window *window);
// Desktop swapchain: the game renders directly into its acquired image.
// Acquire semaphores follow frame slots; present semaphores follow images.
class Window {
public:
    Window(Device &device, SDL_Window *window, uint32_t frames);
    ~Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;
    Target &acquire(FrameRing &ring);
    // Submit before presentation (guest glFlush) without reacquiring the image.
    Submission flush(FrameRing &ring);
    Submission present(FrameRing &ring);
private:
    Device &device_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<Target>> targets_;
    std::vector<VkSemaphore> acquire_, present_;
    uint32_t image_ = 0, slot_ = 0;
    bool acquired_ = false, acquire_pending_ = false;
    void release();
};
} // namespace lindbergh::vk
