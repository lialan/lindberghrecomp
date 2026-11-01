#include "device.hpp"
#include "frame_ring.hpp"
#include "renderer.hpp"
#include "guest_state.hpp"
#include "guest_arrays.hpp"
#include "guest_pixels.hpp"
#include "uniforms.hpp"
#include "resources.hpp"
#include "shader_pack.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace lindbergh::vk;
static void require(bool test, const char *what)
{
    if (!test) throw std::runtime_error(what);
}

// Cross-compiled tests stage only source-owned fixtures onto the simulator.
static std::string fixture_path(const char *directory, const char *fallback, const char *name)
{
    const char *override = std::getenv(directory);
    return std::string(override ? override : fallback) + "/" + name;
}

static std::string source(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "missing source-owned shader fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
static std::string hex(const SourceDigest &hash)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : hash) { out += digits[byte >> 4]; out += digits[byte & 15]; }
    return out;
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
    auto dormant=state;
    dormant.src_rgb=VK_BLEND_FACTOR_SRC_ALPHA;dormant.dst_rgb=VK_BLEND_FACTOR_ONE;
    dormant.blend_rgb=VK_BLEND_OP_SUBTRACT;dormant.stencil_compare=VK_COMPARE_OP_NEVER;
    dormant.stencil_pass=VK_STENCIL_OP_REPLACE;
    renderer.prepare(dormant);
    require(renderer.pipeline_count()==1, "disabled blend/stencil parameters created duplicate pipelines");
    auto no_depth=state;no_depth.depth_test=false;
    auto no_depth_other=no_depth;no_depth_other.depth_write=false;no_depth_other.depth_compare=VK_COMPARE_OP_GREATER;
    require(no_depth.key()==no_depth_other.key(), "disabled depth parameters changed the pipeline key");
    dormant.blend=true;require(!(dormant.key()==state.key()), "enabled blend state was lost from the key");
    dormant=state;dormant.stencil=true;require(!(dormant.key()==state.key()), "enabled stencil state was lost from the key");
    dormant=state;dormant.clockwise=!state.clockwise;
    require(!(dormant.key()==state.key()), "front-face shader semantics were lost with culling disabled");
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
    dormant=state;dormant.src_rgb=VK_BLEND_FACTOR_SRC_ALPHA;dormant.dst_rgb=VK_BLEND_FACTOR_ONE;
    renderer.draw(ring, triangle, 3, matrices, dormant);
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

    ShaderPack pack(fixture_path("LV_TEST_SHADER_DIR", LV_TEST_SHADER_DIR, "fixture.lvsp"));
    auto vertex_source = source(fixture_path("LV_TEST_SOURCE_DIR", LV_TEST_SOURCE_DIR, "legacy.vert"));
    require(!vertex_source.empty() && vertex_source.back() == '\n', "fixture must exercise EOF newline normalization");
    vertex_source.pop_back();
    const auto vs_hash = shader_source_digest(vertex_source);
    const auto fs_hash = shader_source_digest(source(fixture_path("LV_TEST_SOURCE_DIR", LV_TEST_SOURCE_DIR, "legacy.frag")));
    const auto record = pack.find(vs_hash, fs_hash, {});
    require(record && pack.programs().size() == 1, "offline shader archive lookup failed");
    require(!pack.find(SourceDigest{}, fs_hash, {}), "unknown shader source silently accepted");
    const auto &vs = record->vertex, &fs = record->fragment;
    const auto uniform_layout = std::find_if(record->uniforms.begin(), record->uniforms.end(),
                                             [](const auto &b) { return b.set == 1 && b.binding == 0; });
    require(uniform_layout != record->uniforms.end() && uniform_layout->bytes == 16,
            "shader archive lost the reflected guest uniform layout");
    UniformData guest_uniforms(*uniform_layout);
    const auto tint_location = guest_uniforms.find("guestTint");
    require(tint_location >= 0 && guest_uniforms.find("not_a_uniform") == -1, "uniform name lookup changed");
    const auto legacy_layout = std::find_if(record->uniforms.begin(),record->uniforms.end(),[](const auto &b){return b.set==0;});
    require(legacy_layout!=record->uniforms.end(), "missing legacy uniform block");
    UniformData legacy_data(*legacy_layout);
    UniformSet legacy_uniforms(device,ring,{{0,legacy_layout->bytes}});
    auto bind_legacy = [&](Renderer &r) {
        auto offset=legacy_uniforms.upload(0,legacy_data.bytes().data(),legacy_data.bytes().size());
        auto ds=legacy_uniforms.current_set();r.bind_descriptors(0,1,&ds,1,&offset);
    };
    UniformSet uniforms(device, ring, {{0, 16}});
    TextureInfo texture_info{2, 2, 2};
    if (device.capabilities.image_view_format_swizzle) {
        texture_info.format = VK_FORMAT_R8_UNORM;
        texture_info.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                   VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE};
    }
    Texture legacy_texture(device, texture_info);
    Texture texture(device, {2, 2, 2});
    Sampler sampler(device);
    SampledSet material(device, {{0, texture.view(), sampler.handle()}});
    SampledSet legacy_material(device, {{0, legacy_texture.view(), sampler.handle()}});
    ProgramSource legacy{vs.data(), fs.data(), vs.size() * 4, fs.size() * 4,
        {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}},
        {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, position)},
         {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}}, {legacy_uniforms.layout(), uniforms.layout(), material.layout()}};
    Renderer converted(device, target, cache, legacy);
    converted.prepare(state);
    for (auto &m : matrices.view_projection) {
        std::fill(m, m + 16, 0);
        for (unsigned i = 0; i < 4; i++) m[i * 5] = 1;
    }
    matrices.view_projection[1][12] = 0.8f;
    cmd = ring.begin();
    const uint32_t white[] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    const bool luminance = texture_info.format == VK_FORMAT_R8_UNORM;
    legacy_texture.upload(cmd, ring, 0, 0, white, luminance ? 4 : sizeof white);
    legacy_texture.upload(cmd, ring, 1, 0, white, luminance ? 1 : 4);
    converted.begin(cmd, {0, 0, 0, 1});
    bind_legacy(converted);
    auto sampled = legacy_material.handle();
    converted.bind_descriptors(2, 1, &sampled);
    const float tint[] = {1, 1, 1, 1};
    guest_uniforms.write(static_cast<uint32_t>(tint_location), tint, sizeof tint);
    const auto version = guest_uniforms.version();
    guest_uniforms.write(static_cast<uint32_t>(tint_location), tint, sizeof tint);
    require(guest_uniforms.version() == version, "unchanged uniform marked dirty");
    auto offset = uniforms.upload(0, guest_uniforms.bytes().data(), guest_uniforms.bytes().size());
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

    // Game loading uses packed BGRA8888_REV and RGB565. Sample the original
    // guest byte order directly; checking channels catches a mistaken swizzle.
    for (auto format : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_R4G4B4A4_UNORM_PACK16}) {
        TextureInfo packed_info{1,1,1,1,format};
        const bool is_4444=format==VK_FORMAT_R4G4B4A4_UNORM_PACK16;
        if (is_4444) packed_info.components={VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_R};
        Texture packed(device, packed_info);
        SampledSet packed_material(device, {{0,packed.view(),sampler.handle()}});
        cmd=ring.begin();
        const uint8_t bgra[]={0,0,255,255}, rgb565[]={0,248}, bgra4444[]={0,255};
        const bool is_565=format==VK_FORMAT_R5G6B5_UNORM_PACK16;
        packed.upload(cmd,ring,0,0,is_565?rgb565:is_4444?bgra4444:bgra,(is_565||is_4444)?2:4);
        converted.begin(cmd,{0,0,0,1});bind_legacy(converted);
        auto ds=packed_material.handle();converted.bind_descriptors(2,1,&ds);
        auto u=uniforms.upload(0,guest_uniforms.bytes().data(),guest_uniforms.bytes().size());
        ds=uniforms.current_set();converted.bind_descriptors(1,1,&ds,1,&u);
        auto white_triangle=std::vector<Vertex>(std::begin(triangle),std::end(triangle));
        for (auto &vertex:white_triangle) for (auto &channel:vertex.color) channel=1;
        converted.draw(ring,white_triangle.data(),3,matrices,state);converted.end();
        vkCmdCopyImageToBuffer(cmd,target.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.buffer,1,&copy);
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                             0,0,nullptr,1,&barrier,0,nullptr);
        ring.wait(ring.submit());
        const auto rgba=static_cast<const uint8_t *>(output.mapped)+(16*32+8)*4;
        require(rgba[0]==255 && rgba[1]==0 && rgba[2]==0 && rgba[3]==255,
                "packed guest texture channels changed");
    }
    std::puts("ok: native BGRA8888, RGB565 and swizzled BGRA4444 guest storage samples without pixel conversion");

    // Different draws and frames must retain independent uniform snapshots even
    // with several frames in flight and descriptor sets recycled by slot.
    constexpr unsigned frames = 12, image_bytes = 32 * 32 * 4 * 2;
    Readback history(device, frames * image_bytes);
    rejected([&] { uniforms.current_set(); }, "uniform set accepted outside frame");
    Submission last{};
    for (unsigned frame = 0; frame < frames; ++frame) {
        cmd = ring.begin();
        converted.begin(cmd, {0, 0, 0, 1});
    bind_legacy(converted);
        converted.bind_descriptors(2, 1, &sampled);
        const float color[] = {float(frame & 1), 1, 1, 1};
        rejected([&] { uniforms.upload(0, color, 12); }, "truncated uniform block accepted");
        guest_uniforms.write(static_cast<uint32_t>(tint_location), color, sizeof color);
        offset = uniforms.upload(0, guest_uniforms.bytes().data(), guest_uniforms.bytes().size());
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

    // Feed native formats directly: separate float positions and normalized
    // byte colors, with an immutable uint16 index buffer. Upload geometry once.
    ProgramSource separate = legacy;
    separate.bindings = {{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 4, VK_VERTEX_INPUT_RATE_VERTEX}};
    separate.attributes = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {3, 1, VK_FORMAT_R8G8B8A8_UNORM, 0}};
    std::unique_ptr<Renderer> textured, constant_color;
    const float positions[][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1}};
    const uint32_t colors[] = {0xff808080, 0xff808080, 0xff808080, 0xff808080};
    const uint16_t elements[] = {0, 1, 2, 0, 2, 3};
    GeometryBuffer positions_gpu(device, sizeof positions), colors_gpu(device, sizeof colors), indices_gpu(device, sizeof elements);
    GuestArrays arrays;
    arrays.inputs({0, 3});
    arrays.bind_array_buffer(7);
    arrays.pointer(0, 4, 0x1406, false, 0, 0);
    arrays.enable(0, true);
    arrays.bind_array_buffer(8);
    arrays.pointer(3, 4, 0x1401, true, 0, 0);
    arrays.enable(3, true);
    arrays.current(3, {128.0f / 255, 128.0f / 255, 128.0f / 255, 1});
    arrays.bind_array_buffer(9); // cannot change the buffer captured by pointer()
    const GuestMemoryView memory{colors, sizeof colors, 0xfffffff0};
    require(memory.read(0xfffffff0, 16) == colors, "high guest client address was truncated");
    rejected([&] { memory.read(0xfffffffc, 8); }, "client array crossed the 32-bit address limit");
    const BufferLookup buffers = [&](uint32_t name) -> GuestBufferView {
        if (name == 7) return {positions_gpu.handle(), sizeof positions};
        if (name == 8) return {colors_gpu.handle(), sizeof colors};
        throw std::runtime_error("vertex pointer did not capture its array-buffer binding");
    };
    IndexStream elements_gpu{indices_gpu.handle(), 0, VK_INDEX_TYPE_UINT16};
    SamplerInfo mip_info;
    mip_info.min_lod = mip_info.max_lod = 1;
    Sampler mip_sampler(device, mip_info);
    SampledSet mip_material(device, {{0, texture.view(), mip_sampler.handle()}});
    const uint8_t texels[] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
    const uint8_t mip[] = {255,255,0,255};
    for (auto &m : matrices.view_projection) m[12] = 0;
    for (unsigned frame = 0; frame < frames; ++frame) {
        cmd = ring.begin();
        if (!frame) {
            positions_gpu.upload(cmd, ring, positions, sizeof positions);
            colors_gpu.upload(cmd, ring, colors, sizeof colors);
            indices_gpu.upload(cmd, ring, elements, sizeof elements);
            texture.upload(cmd, ring, 0, 0, texels, sizeof texels);
            texture.upload(cmd, ring, 1, 0, mip, sizeof mip);
        }
        if (!frame) {
            const auto &offset_plan = arrays.prepare(device, ring, 1, 3, memory, buffers);
            require(offset_plan.streams[0].offset == 16 && offset_plan.streams[1].offset == 4,
                    "guest DrawArrays first vertex offset changed");
        }
        arrays.enable(3, frame % 3 != 2);
        if (frame % 3 == 1) {
            arrays.bind_array_buffer(0);
            arrays.pointer(3, 4, 0x1401, true, 0, 0xfffffff0);
            rejected([&] { arrays.prepare(device, ring, 1, 4, memory, buffers); },
                     "client array overflow across the 32-bit window accepted");
        } else {
            arrays.bind_array_buffer(8);
            arrays.pointer(3, 4, 0x1401, true, 0, 0);
        }
        const auto &plan = arrays.prepare(device, ring, 0, 4, memory, buffers);
        require(plan.streams[0].buffer == positions_gpu.handle() && plan.streams[0].offset == 0,
                "guest static geometry was copied instead of bound directly");
        if (!frame) {
            separate.bindings = plan.bindings; separate.attributes = plan.attributes;
            textured = std::make_unique<Renderer>(device, target, cache, separate);
            textured->prepare(state);
            separate.bindings[1].stride = 0;
            separate.attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            constant_color = std::make_unique<Renderer>(device, target, cache, separate);
            constant_color->prepare(state);
        }
        auto &draw = frame % 3 == 2 ? *constant_color : *textured;
        draw.begin(cmd, {0, 0, 0, 1});
        bind_legacy(draw);
        offset = uniforms.upload(0, tint, sizeof tint);
        descriptors = uniforms.current_set();
        draw.bind_descriptors(1, 1, &descriptors, 1, &offset);
        sampled = (frame & 1) ? mip_material.handle() : material.handle();
        draw.bind_descriptors(2, 1, &sampled);
        draw.draw_buffers(plan.streams.data(), static_cast<uint32_t>(plan.streams.size()), 6, matrices, state, &elements_gpu);
        draw.end();
        copy.bufferOffset = frame * image_bytes;
        vkCmdCopyImageToBuffer(cmd, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history.buffer, 1, &copy);
        barrier.offset = copy.bufferOffset;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
        last = ring.submit();
    }
    ring.wait(last);
    for (unsigned frame = 0; frame < frames; ++frame) {
        const auto bytes = static_cast<const uint8_t *>(history.mapped) + frame * image_bytes;
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto base = bytes + eye * image_bytes / 2;
            auto sample = [&](unsigned x, unsigned y, const uint8_t *expected) {
                const auto p = base + (y * 32 + x) * 4;
                const auto source = (frame & 1) ? mip : expected;
                const uint8_t modulated[] = {uint8_t(source[0] ? 128 : 0), uint8_t(source[1] ? 128 : 0),
                                             uint8_t(source[2] ? 128 : 0), 255};
                require(std::memcmp(p, modulated, 4) == 0,
                        "texture orientation/mip, normalized colors or GPU vertex/index buffer raster output changed");
            };
            sample(8, 8, texels + 8); sample(24, 8, texels + 12);
            sample(8, 24, texels); sample(24, 24, texels + 4);
        }
    }
    require(textured->draw_count() + constant_color->draw_count() == frames &&
            textured->pipeline_count() == 1 && constant_color->pipeline_count() == 1,
            "static textured stereo geometry was replayed or recompiled");
    std::puts("ok: guest VBO/client/current arrays render directly with textures, uint16 indices and mip descriptors");
}


static void render_copy(Device &device)
{
    TargetInfo info{16,16,1}; info.guest_bottom_origin=true;info.final_layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Target target(device,info);PipelineCache cache(device,"");Renderer renderer(device,target,cache);
    RasterState raster;renderer.prepare(raster);
    // Regression: the game's RGBA framebuffer-copy texture is allocated with
    // LUMINANCE upload bytes. Storage must still accept subsequent full color.
    std::vector<uint8_t> narrow(16*16,0);
    require(guest::canonical_color_storage(guest::RGBA,guest::Luminance,guest::UnsignedByte,16,16,narrow)==guest::RGBA && narrow.size()==16*16*4,
            "RGBA internal storage inherited luminance-only upload channels");
    std::vector<uint8_t> alpha{32};
    guest::canonical_color_storage(guest::RGB,guest::Alpha,guest::UnsignedByte,1,1,alpha);
    require(alpha==std::vector<uint8_t>({0,0,0,255}), "RGB internal storage retained upload alpha");
    Texture color(device,{16,16}),depth(device,{16,16,1,1,Target::depth_format});
    Readback output(device,16*16*4*3);FrameRing ring(device);
    Matrices matrices{};for(auto &m:matrices.view_projection)for(unsigned i=0;i<4;++i)m[i*5]=1;
    Vertex triangle[]={{{-1,-1,0,1},{1,0,0,1}},{{0,-1,0,1},{1,0,0,1}},{{-.5f,1,0,1},{1,0,0,1}}};
    auto cmd=ring.begin();renderer.begin(cmd,{0,0,1,1});renderer.draw(ring,triangle,3,matrices,raster);renderer.end();
    // Resuming a compatible pass must retain both color and depth. A triangle
    // behind the previous one cannot overwrite its red pixels.
    for(auto &v:triangle){v.position[2]=.8f;v.color[0]=0;v.color[1]=1;}
    renderer.begin(cmd,{0,1,0,1},true);renderer.draw(ring,triangle,3,matrices,raster);renderer.end();
    color.copy_from(cmd,target,0,0,0,0,0,16,16);depth.copy_from(cmd,target,0,0,0,0,0,16,16);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={16,16,1};
    vkCmdCopyImageToBuffer(cmd,target.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.buffer,1,&copy);
    for(unsigned i=0;i<2;++i) {
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};image.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        image.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;image.image=i?depth.image():color.image();
        image.subresourceRange={VkImageAspectFlags(i?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
        copy.bufferOffset=(i+1)*16*16*4;copy.imageSubresource.aspectMask=i?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdCopyImageToBuffer(cmd,image.image,image.newLayout,output.buffer,1,&copy);
    }
    VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    ready.srcQueueFamilyIndex=ready.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ready.buffer=output.buffer;ready.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&ready,0,nullptr);
    ring.wait(ring.submit());auto bytes=static_cast<const uint8_t *>(output.mapped);
    require(!std::memcmp(bytes,bytes+16*16*4,16*16*4),"pbuffer color copy changed raster pixels");
    auto pixel=bytes+(2*16+4)*4;require(pixel[0]==255&&pixel[1]==0&&pixel[2]==0,"preserved pbuffer color/depth or bottom-origin viewport changed");
    pixel=bytes+(14*16+4)*4;require(pixel[0]==0&&pixel[2]==255,"guest pbuffer rows were flipped");
    auto z=reinterpret_cast<const float *>(bytes+16*16*4*2);
    require(z[2*16+4]==.5f&&z[14*16+4]==1,"GPU pbuffer depth copy changed retained depth");
    std::puts("ok: preserving pbuffer passes, guest bottom-origin viewport and GPU color/depth copies");
}

int main(int argc, char **argv)
try {
    require(shader_source_digest("a\r\n") == shader_source_digest("a \n"),
            "shader identity differs from guest CR normalization");
    require(shader_source_digest("a") == shader_source_digest("a\n"),
            "shader identity depends on optional final newline");
    require(shader_source_digest("a\n\n") != shader_source_digest("a\n"),
            "shader identity discarded significant trailing source");
    require(hex(source_digest("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "empty shader source hash mismatch");
    require(hex(source_digest("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "shader source SHA-256 mismatch");
    for (const auto &test : std::vector<std::pair<unsigned, std::string>>{
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"}})
        require(hex(source_digest(std::string(test.first, 'a'))) == test.second, "shader hash padding/block mismatch");
    // Non-square matrices in arrays catch both std140 column padding and
    // transpose bugs. These offsets model build-time reflection metadata.
    UniformLayout matrix_layout{1, 0, 128, {{"matrix", 16, 0, 2, 3, 2, 48, 16}}};
    UniformData matrix_data(matrix_layout);
    const float row_major[] = {1,2, 3,4, 5,6};
    matrix_data.write(0, row_major, sizeof row_major, 1, 1, true);
    const float col0[] = {1,3,5}, col1[] = {2,4,6};
    require(!std::memcmp(matrix_data.bytes().data()+64, col0, sizeof col0) &&
            !std::memcmp(matrix_data.bytes().data()+80, col1, sizeof col1) && matrix_data.bytes()[76] == 0,
            "std140 matrix/array stride or transpose mismatch");
    matrix_data.write(0, row_major, sizeof row_major, 0, 1, false);
    require(!std::memcmp(matrix_data.bytes().data()+16, row_major, 12) &&
            !std::memcmp(matrix_data.bytes().data()+32, row_major+3, 12) && matrix_data.bytes()[28] == 0,
            "column-major std140 padding mismatch");
    rejected([&] { matrix_data.write(0, row_major, sizeof row_major, 2); }, "out-of-range uniform array accepted");
    DeviceOptions options;
    options.allow_software = true; // This test deliberately runs on lavapipe.
    options.require_foveation = false;
    auto device = Device::create(options);
    const auto &h = device->handles;
    require(device->capabilities.multiview, "stereo multiview not available");
    auto limited_caps = device->capabilities;
    limited_caps.image_view_format_swizzle = false;
    auto limited = Device::borrow(h, limited_caps);
    TextureInfo luminance{1, 1};
    luminance.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                            VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE};
    rejected([&] { Texture unsupported(*limited, luminance); },
             "unsupported texture component swizzle accepted");
    render_stereo(*device);
    render_copy(*device);
    if (argc == 2) {
        ShaderPack game_pack(argv[1]);
        size_t fields = 0;
        for (const auto &p : game_pack.programs()) {
            require(game_pack.find(p.vertex_source, p.fragment_source, p.attributes) == &p,
                    "shader archive source/binding selection changed");
            for (const auto &block : p.uniforms) fields += block.fields.size();
            for (const auto *module : {&p.vertex, &p.fragment}) {
                VkShaderModuleCreateInfo create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                create.codeSize = module->size() * 4; create.pCode = module->data();
                VkShaderModule handle;
                check(vkCreateShaderModule(h.device, &create, nullptr, &handle), "archived game SPIR-V module");
                vkDestroyShaderModule(h.device, handle, nullptr);
            }
        }
        std::printf("ok: loaded %zu archived game shader pairs and %zu reflected fields without a runtime GLSL compiler\n",
                    game_pack.programs().size(), fields);
    }
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
