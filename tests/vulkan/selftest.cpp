#include "device.hpp"
#include "frame_ring.hpp"
#include "renderer.hpp"
#include "guest_state.hpp"
#include "uniforms.hpp"
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

static std::vector<uint32_t> shader(const std::string &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(bool(input), "missing compiled legacy shader fixture");
    const auto n = input.tellg();
    require(n >= 20 && n % 4 == 0, "invalid compiled legacy shader fixture");
    std::vector<uint32_t> data(static_cast<size_t>(n) / 4);
    input.seekg(0);
    require(bool(input.read(reinterpret_cast<char *>(data.data()), n)), "could not read legacy shader fixture");
    return data;
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

    // Exercise guest fixed-function matrix semantics and GL_QUADS through the
    // direct frontend, then verify actual raster output rather than GL calls.
    GuestState guest_state;
    guest_state.matrix_mode(guest::Projection);
    guest_state.ortho(-1, 1, -1, 1, -1, 1);
    guest_state.matrix_mode(guest::Modelview);
    guest_state.translate(0.5f, 0, 0);
    guest_state.push_matrix();
    guest_state.scale(0.5f, 0.5f, 1);
    guest_state.color(1, 0, 0, 1);
    guest_state.begin(guest::Quads);
    guest_state.vertex(-1, -1); guest_state.vertex(1, -1);
    guest_state.vertex(1, 1); guest_state.vertex(-1, 1);
    const auto batch = guest_state.end();
    require(batch && batch->vertices.size() == 4 && batch->indices.size() == 6,
            "guest quad was not lowered to indexed triangles");
    auto mvp = guest_state.modelview_projection();
    std::memcpy(matrices.view_projection[0], mvp.data(), sizeof mvp);
    mvp[12] -= 0.5f;
    std::memcpy(matrices.view_projection[1], mvp.data(), sizeof mvp);
    cmd = ring.begin();
    renderer.begin(cmd, {0, 0, 0, 1});
    renderer.draw_indexed(ring, batch->vertices.data(), static_cast<uint32_t>(batch->vertices.size()),
        batch->indices.data(), static_cast<uint32_t>(batch->indices.size()), matrices, state);
    renderer.end();
    vkCmdCopyImageToBuffer(cmd, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &copy);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    ring.wait(ring.submit());
    pixel(0, 24, 16, 255); pixel(0, 8, 16, 0);
    pixel(1, 16, 16, 255); pixel(1, 28, 16, 0);
    guest_state.pop_matrix();
    require(guest_state.current_matrix()[0] == 1 && guest_state.current_matrix()[12] == 0.5f,
            "guest matrix stack did not restore modelview");
    guest_state.pop_matrix();
    require(guest_state.error() == guest::StackUnderflow && guest_state.error() == guest::NoError,
            "guest matrix error state did not clear");
    guest_state.begin(guest::TriangleStrip);
    guest_state.vertex(-1, -1); guest_state.vertex(1, -1);
    guest_state.vertex(-1, 1); guest_state.vertex(1, 1);
    auto strip = guest_state.end();
    require(strip->indices == std::vector<uint32_t>({0, 1, 2, 2, 1, 3}), "triangle strip winding changed");
    require(renderer.pipeline_count() == 1, "guest state translation recompiled a cached pipeline");
    std::puts("ok: guest matrices, quad/strip assembly and indexed Vulkan stereo rasterization");

    const auto vs = shader(std::string(LV_TEST_SHADER_DIR) + "/vert.spv");
    const auto fs = shader(std::string(LV_TEST_SHADER_DIR) + "/frag.spv");
    UniformSet uniforms(device, ring, {{0, 16}});
    ProgramSource legacy{vs.data(), fs.data(), vs.size() * 4, fs.size() * 4,
        {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}},
        {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, position)},
         {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}}, {uniforms.layout(), uniforms.layout()}};
    Renderer converted(device, target, cache, legacy);
    converted.prepare(state);
    for (auto &m : matrices.view_projection) {
        std::fill(m, m + 16, 0);
        for (unsigned i = 0; i < 4; i++) m[i * 5] = 1;
    }
    matrices.view_projection[1][12] = 0.8f;
    cmd = ring.begin();
    converted.begin(cmd, {0, 0, 0, 1});
    const float tint[] = {1, 1, 1, 1};
    auto offset = uniforms.upload(0, tint, sizeof tint);
    auto descriptors = uniforms.current_set();
    converted.bind_descriptors(1, 1, &descriptors, 1, &offset);
    converted.draw(ring, triangle, 3, matrices, state);
    converted.end();
    vkCmdCopyImageToBuffer(cmd, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &copy);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    ring.wait(ring.submit());
    pixel(0, 8, 16, 255); pixel(0, 24, 16, 0);
    pixel(1, 8, 16, 0); pixel(1, 24, 16, 255);
    std::puts("ok: converted GLSL 1.20 shaders preserve front-face color and multiview raster output");

    // Different draws and frames must retain independent uniform snapshots even
    // with several frames in flight and descriptor sets recycled by slot.
    constexpr unsigned frames = 12, image_bytes = 32 * 32 * 4 * 2;
    Readback history(device, frames * image_bytes);
    rejected([&] { uniforms.current_set(); }, "uniform set accepted outside frame");
    Submission last{};
    for (unsigned frame = 0; frame < frames; ++frame) {
        cmd = ring.begin();
        converted.begin(cmd, {0, 0, 0, 1});
        const float color[] = {float(frame & 1), 1, 1, 1};
        rejected([&] { uniforms.upload(0, color, 12); }, "truncated uniform block accepted");
        offset = uniforms.upload(0, color, sizeof color);
        require(offset % device.capabilities.properties.limits.minUniformBufferOffsetAlignment == 0,
                "uniform upload violates device alignment");
        descriptors = uniforms.current_set();
        converted.bind_descriptors(1, 1, &descriptors, 1, &offset);
        converted.draw(ring, triangle, 3, matrices, state);
        // Change the state after the draw: its previous snapshot must survive.
        const float black[] = {0, 0, 0, 1};
        uniforms.upload(0, black, sizeof black);
        converted.end();
        copy.bufferOffset = frame * image_bytes;
        vkCmdCopyImageToBuffer(cmd, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history.buffer, 1, &copy);
        barrier.buffer = history.buffer; barrier.offset = copy.bufferOffset; barrier.size = image_bytes;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
        last = ring.submit();
    }
    ring.wait(last);
    for (unsigned frame = 0; frame < frames; ++frame) {
        const auto bytes = static_cast<const unsigned char *>(history.mapped) + frame * image_bytes;
        const auto left = bytes + (16 * 32 + 8) * 4;
        const auto right = bytes + ((32 + 16) * 32 + 24) * 4;
        require(left[0] == (frame & 1) * 255 && right[0] == left[0] && left[3] == 255 && right[3] == 255,
                "buffered guest uniform state corrupted across in-flight frames");
    }
    require(converted.pipeline_count() == 1, "uniform changes created new pipelines");
    std::puts("ok: converted shader uniforms retain snapshots across 12 asynchronously submitted frames");
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
