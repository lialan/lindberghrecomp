#include "device.hpp"
#include "frame_ring.hpp"
#include "renderer.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace lindbergh::vk;
static void require(bool test, const char *what)
{
    if (!test) throw std::runtime_error(what);
}

template<class F> static void rejected(F fn, const char *what)
{
    bool threw = false;
    try { fn(); } catch (const std::exception &) { threw = true; }
    require(threw, what);
}

struct Readback {
    Device &device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    explicit Readback(Device &d, VkDeviceSize size) : device(d)
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(d.handles.device, &info, nullptr, &buffer), "readback buffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(d.handles.device, buffer, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = d.memory_type(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(d.handles.device, &alloc, nullptr, &memory), "readback memory");
        check(vkBindBufferMemory(d.handles.device, buffer, memory, 0), "bind readback");
        check(vkMapMemory(d.handles.device, memory, 0, size, 0, &mapped), "map readback");
    }
    ~Readback()
    {
        if (mapped) vkUnmapMemory(device.handles.device, memory);
        if (buffer) vkDestroyBuffer(device.handles.device, buffer, nullptr);
        if (memory) vkFreeMemory(device.handles.device, memory, nullptr);
    }
};

static void render_stereo(Device &device)
{
    TargetInfo info;
    info.width = info.height = 32;
    info.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Target target(device, info);
    PipelineCache cache(device, "");
    Renderer renderer(device, target, cache);
    RasterState state;
    renderer.prepare(state);
    renderer.prepare(state);
    require(renderer.pipeline_count() == 1, "pipeline key failed to deduplicate state");
    Readback output(device, 32 * 32 * 4 * 2);
    // Ring is declared last so outstanding work completes before resources die.
    FrameRing ring(device);
    auto cmd = ring.begin();
    renderer.begin(cmd, {0, 0, 0, 1});
    rejected([&] { renderer.prepare(state); }, "pipeline compilation entered draw recording");
    Vertex triangle[] = {
        {{-0.9f, -0.9f, 0, 1}, {1, 0, 0, 1}},
        {{ 0.1f, -0.9f, 0, 1}, {1, 0, 0, 1}},
        {{-0.4f,  0.9f, 0, 1}, {1, 0, 0, 1}}
    };
    Matrices matrices{};
    for (auto &m : matrices.view_projection) for (unsigned i = 0; i < 4; i++) m[i * 5] = 1;
    matrices.view_projection[1][12] = 0.8f;
    renderer.draw(ring, triangle, 3, matrices, state);
    renderer.end();
    require(renderer.draw_count() == 1, "stereo required more than one draw command");
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2};
    copy.imageExtent = {32, 32, 1};
    vkCmdCopyImageToBuffer(cmd, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = output.buffer; barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    auto submitted = ring.submit();
    ring.wait(submitted);
    auto pixel = [&](unsigned eye, unsigned x, unsigned y, unsigned red) {
        const auto p = static_cast<const unsigned char *>(output.mapped) + ((eye * 32 + y) * 32 + x) * 4;
        require(p[0] == red && p[1] == 0 && p[2] == 0 && p[3] == 255, "Vulkan stereo raster output mismatch");
    };
    pixel(0, 8, 16, 255); pixel(0, 24, 16, 0);
    pixel(1, 8, 16, 0); pixel(1, 24, 16, 255);
    std::puts("ok: one Vulkan draw rendered distinct stereo views with build-time SPIR-V");
}

int main()
try {
    DeviceOptions options;
    options.allow_software = true; // This test deliberately runs on lavapipe.
    options.require_foveation = false;
    auto device = Device::create(options);
    const auto &h = device->handles;
    require(device->capabilities.multiview, "stereo multiview not available");
    render_stereo(*device);
    // Borrowing must not accidentally destroy a device owned by OpenXR.
    {
        auto borrowed = Device::borrow(h, device->capabilities);
        require(borrowed->handles.device == h.device, "borrowed Vulkan device changed");
    }

    constexpr uint32_t frames = 12, words = 16;
    Readback output(*device, frames * words * sizeof(uint32_t));
    {
        FrameRing ring(*device, 3, 4096);
        rejected([&] { ring.submit(); }, "empty submission accepted");
        rejected([&] { ring.upload(nullptr, 1); }, "upload outside frame accepted");
        Submission last{};
        for (uint32_t frame = 0; frame < frames; frame++) {
            auto cmd = ring.begin();
            rejected([&] { ring.begin(); }, "overlapping frame recording accepted");
            std::array<uint32_t, words> values;
            for (uint32_t i = 0; i < words; i++) values[i] = 0x12340000u + frame * 100 + i;
            ring.upload(values.data(), 3, 1);
            auto source = ring.upload(values.data(), sizeof values, 256);
            require(source.offset == 256, "uniform upload alignment not honored");
            rejected([&] { ring.upload(values.data(), 4096); }, "oversized frame upload accepted");
            VkBufferCopy copy{source.offset, frame * sizeof values, sizeof values};
            vkCmdCopyBuffer(cmd, source.buffer, output.buffer, 1, &copy);
            VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ready.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            ready.srcQueueFamilyIndex = ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ready.buffer = output.buffer;
            ready.offset = copy.dstOffset;
            ready.size = copy.size;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 0, nullptr, 1, &ready, 0, nullptr);
            last = ring.submit();
        }
        require(ring.submissions() == frames, "frame submission count mismatch");
        ring.wait(last);
        auto read = static_cast<const uint32_t *>(output.mapped);
        for (uint32_t frame = 0; frame < frames; frame++)
            for (uint32_t i = 0; i < words; i++)
                require(read[frame * words + i] == 0x12340000u + frame * 100 + i,
                        "GPU upload corrupted across buffered frame reuse");
    }

    const auto path = (std::filesystem::temp_directory_path() /
        ("lindbergh-vulkan-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
    // Invalid driver cache bytes must not reach vkCreatePipelineCache.
    {
        std::ofstream invalid(path, std::ios::binary);
        std::array<char, 128> junk{};
        invalid.write(junk.data(), junk.size());
    }
    {
        PipelineCache cache(*device, path);
        require(!cache.loaded(), "invalid Vulkan cache accepted");
        cache.save();
    }
    {
        PipelineCache cache(*device, path);
        require(cache.loaded(), "saved Vulkan cache could not be restored");
    }
    std::filesystem::remove(path);

    if (!device->capabilities.density_map && !device->capabilities.shading_rate) {
        options.require_foveation = true;
        rejected([&] { Device::create(options); }, "required foveation silently disabled");
        std::puts("ok: unsupported required foveation rejected explicitly");
    }
    std::puts("ok: native Vulkan device ownership, asynchronous frame uploads and GPU readback, pipeline cache validation");
    return 0;
} catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL Vulkan: %s\n", e.what());
    return 1;
}
