#include "pipeline_recipes.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace lindbergh::vk {
namespace {
void word(std::string &s, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        s += char(v >> (i * 8));
}
struct Reader {
    const std::string &s;
    size_t at = 0;
    uint32_t word() {
        if (at + 4 > s.size())
            throw std::runtime_error("Truncated pipeline recipes");
        uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i)
            v |= uint32_t(uint8_t(s[at++])) << (i * 8);
        return v;
    }
    SourceDigest digest() {
        if (at + 32 > s.size())
            throw std::runtime_error("Truncated recipe shader identity");
        SourceDigest v;
        std::memcpy(v.data(), s.data() + at, 32);
        at += 32;
        return v;
    }
};
RasterState raster(RasterKey k) {
    RasterState r;
    const auto a = k.a, b = k.b;
    r.depth_test = a & 1;
    r.depth_write = (a >> 1) & 1;
    r.blend = (a >> 2) & 1;
    r.cull = (a >> 3) & 3;
    r.depth_compare = VkCompareOp((a >> 5) & 7);
    r.topology = VkPrimitiveTopology((a >> 8) & 15);
    r.color_mask = (a >> 12) & 15;
    r.stencil = (a >> 16) & 1;
    r.stencil_compare = VkCompareOp((a >> 17) & 7);
    r.stencil_fail = VkStencilOp((a >> 20) & 7);
    r.stencil_depth_fail = VkStencilOp((a >> 23) & 7);
    r.stencil_pass = VkStencilOp((a >> 26) & 7);
    r.depth_bias = (a >> 29) & 1;
    r.clockwise = (a >> 30) & 1;
    r.src_rgb = VkBlendFactor(b & 31);
    r.dst_rgb = VkBlendFactor((b >> 5) & 31);
    r.src_alpha = VkBlendFactor((b >> 10) & 31);
    r.dst_alpha = VkBlendFactor((b >> 15) & 31);
    r.blend_rgb = VkBlendOp((b >> 20) & 7);
    r.blend_alpha = VkBlendOp((b >> 23) & 7);
    r.alpha_func = (b >> 26) & 15;
    r.fixed_texture = (b >> 30) & 255;
    r.fixed_env = (b >> 38) & 0xffffff;
    r.fog_mode = (b >> 62) & 3;
    if (!(r.key() == k))
        throw std::runtime_error("Invalid noncanonical pipeline raster recipe");
    return r;
}
SourceDigest identity(const ShaderRecord &record) {
    std::string bytes(reinterpret_cast<const char *>(record.key.data()), 32);
    word(bytes, uint32_t(record.vertex.size()));
    word(bytes, uint32_t(record.fragment.size()));
    // Explicit little endian also makes the catalogue independent of host ABI.
    for (auto v : record.vertex)
        word(bytes, v);
    for (auto v : record.fragment)
        word(bytes, v);
    return source_digest(bytes);
}
bool portable_color(uint32_t format) {
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}
bool portable_target(const std::array<uint32_t,5> &source, const std::array<uint32_t,5> &destination) {
    // These are pipeline descriptions, not VkPipeline handles. Rebuild against
    // the destination's own render pass (including color conversion and FDM).
    // Shader identity remains exact; retain view count and origin conventions.
    return source == destination ||
           (portable_color(source[0]) && portable_color(destination[0]) &&
            source[1] == destination[1] && source[3] == destination[3]);
}
} // namespace
std::array<uint32_t, 5> PipelineRecipes::target_class(const Target &target) {
    return {uint32_t(target.info.color_format), target.info.layers, uint32_t(target.info.final_layout),
            uint32_t(target.info.guest_bottom_origin), uint32_t(target.info.density_map != VK_NULL_HANDLE)};
}
std::string PipelineRecipes::key(const Recipe &r, bool with_raster) {
    std::string s(reinterpret_cast<const char *>(r.shader.data()), 32);
    for (auto v : r.target)
        word(s, v);
    word(s, uint32_t(r.bindings.size()));
    word(s, uint32_t(r.attributes.size()));
    for (auto &b : r.bindings) {
        word(s, b.binding);
        word(s, b.stride);
        word(s, b.inputRate);
    }
    for (auto &a : r.attributes) {
        word(s, a.location);
        word(s, a.binding);
        word(s, a.format);
        word(s, a.offset);
    }
    if (with_raster) {
        const auto k = r.raster.key();
        word(s, uint32_t(k.a));
        word(s, uint32_t(k.a >> 32));
        word(s, uint32_t(k.b));
        word(s, uint32_t(k.b >> 32));
    }
    return s;
}
PipelineRecipes::PipelineRecipes(Device &d, PipelineCache &cache, std::vector<const ShaderPack *> packs,
                                 std::string path, std::string seed)
    : device_(d), cache_(cache), path_(std::move(path)) {
    for (auto *pack : packs)
        if (pack)
            for (const auto &r : pack->programs()) {
                const auto id = identity(r);
                shaders_[id] = &r;
                identities_[&r] = id;
            }
    load(seed);
    load(path_);
    std::fprintf(stderr, "[vulkan-recipes] %zu portable pipeline recipes loaded\n", recipes_.size());
}
PipelineRecipes::~PipelineRecipes() { lifetime_.reset(); }
void PipelineRecipes::validate(const Recipe &r) const {
    const auto &limits = device_.capabilities.properties.limits;
    if (r.bindings.size() > limits.maxVertexInputBindings ||
        r.attributes.size() > limits.maxVertexInputAttributes)
        throw std::runtime_error("Pipeline recipe vertex input count exceeds device limits");
    std::set<uint32_t> bindings, locations;
    for (auto &b : r.bindings)
        if (b.binding >= limits.maxVertexInputBindings || b.stride > limits.maxVertexInputBindingStride ||
            b.inputRate > VK_VERTEX_INPUT_RATE_INSTANCE || !bindings.insert(b.binding).second)
            throw std::runtime_error("Invalid pipeline recipe vertex binding");
    for (auto &a : r.attributes) {
        if (a.location >= limits.maxVertexInputAttributes ||
            a.offset > limits.maxVertexInputAttributeOffset || !bindings.count(a.binding) ||
            !locations.insert(a.location).second || a.format < VK_FORMAT_R4G4_UNORM_PACK8 ||
            a.format > VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
            throw std::runtime_error("Invalid pipeline recipe vertex attribute");
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(device_.handles.physical, a.format, &properties);
        if (!(properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
            throw std::runtime_error("Unsupported pipeline recipe vertex format");
    }
    r.raster.key();
}
void PipelineRecipes::load(const std::string &path) {
    if (path.empty())
        return;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return;
    try {
        const auto size = in.tellg();
        if (size < 44 || size > 64 * 1024 * 1024)
            throw std::runtime_error("Invalid pipeline recipe file size");
        std::string data(size_t(size), '\0');
        in.seekg(0);
        if (!in.read(data.data(), size))
            throw std::runtime_error("Cannot read pipeline recipes");
        const auto checksum = source_digest(std::string_view(data.data(), data.size() - 32));
        if (std::memcmp(data.data() + data.size() - 32, checksum.data(), 32))
            throw std::runtime_error("Pipeline recipe checksum mismatch");
        data.resize(data.size() - 32);
        if (data.compare(0, 8, std::string("LVPREC1\0", 8)))
            throw std::runtime_error("Unsupported pipeline recipe version");
        Reader reader{data, 8};
        const auto count = reader.word();
        if (count > 100000)
            throw std::runtime_error("Too many pipeline recipes");
        std::map<std::string, Recipe> pending;
        for (uint32_t i = 0; i < count; ++i) {
            Recipe r;
            r.shader = reader.digest();
            for (auto &v : r.target)
                v = reader.word();
            const auto bindings = reader.word(), attributes = reader.word();
            if (bindings > 32 || attributes > 32 || !r.target[1] || r.target[1] > 2 || r.target[3] > 1 ||
                r.target[4] > 1)
                throw std::runtime_error("Invalid pipeline recipe schema");
            for (uint32_t j = 0; j < bindings; ++j) {
                const auto b = reader.word(), s = reader.word(), rate = reader.word();
                r.bindings.push_back({b, s, VkVertexInputRate(rate)});
            }
            for (uint32_t j = 0; j < attributes; ++j) {
                const auto l = reader.word(), b = reader.word(), f = reader.word(), o = reader.word();
                r.attributes.push_back({l, b, VkFormat(f), o});
            }
            const auto al = reader.word(), ah = reader.word(), bl = reader.word(), bh = reader.word();
            r.raster = raster({uint64_t(al) | (uint64_t(ah) << 32), uint64_t(bl) | (uint64_t(bh) << 32)});
            if (shaders_.count(r.shader)) {
                validate(r);
                pending.emplace(key(r, true), r);
            }
        }
        if (reader.at != data.size())
            throw std::runtime_error("Trailing pipeline recipe data");
        // A corrupt seed/cache never partially populates live pipeline state.
        for (auto &entry : pending)
            recipes_.insert(entry);
        if (path != path_ && !pending.empty())
            dirty_ = true;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[vulkan-recipes] ignored %s: %s\n", path.c_str(), e.what());
    }
}
std::shared_ptr<Renderer>
PipelineRecipes::renderer(Target &target, const ShaderRecord &record,
                          const std::vector<VkVertexInputBindingDescription> &bindings,
                          const std::vector<VkVertexInputAttributeDescription> &attributes) {
    Recipe base{identities_.at(&record), target_class(target), bindings, attributes, {}};
    auto &entry = renderers_[{&target, key(base, false)}];
    if (entry)
        return entry;
    validate(base);
    std::array<VkDescriptorSetLayout, 3> layouts{};
    try {
        for (unsigned set = 0; set < 3; ++set) {
            std::vector<VkDescriptorSetLayoutBinding> descriptions;
            if (set < 2)
                for (auto &u : record.uniforms)
                    if (u.set == set)
                        descriptions.push_back({u.binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                                nullptr});
            if (set == 2)
                for (auto &s : record.samplers) {
                    if (s.count != 1)
                        throw std::runtime_error("Unsupported pipeline recipe sampler array");
                    descriptions.push_back({s.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                            nullptr});
                }
            std::sort(descriptions.begin(), descriptions.end(),
                      [](auto &a, auto &b) { return a.binding < b.binding; });
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = uint32_t(descriptions.size());
            info.pBindings = descriptions.data();
            check(vkCreateDescriptorSetLayout(device_.handles.device, &info, nullptr, &layouts[set]),
                  "recipe descriptor layout");
        }
        ProgramSource source{record.vertex.data(),
                             record.fragment.data(),
                             record.vertex.size() * 4,
                             record.fragment.size() * 4,
                             bindings,
                             attributes,
                             {layouts.begin(), layouts.end()}};
        entry = std::make_shared<Renderer>(device_, target, cache_, source);
        entry->pipeline_created = [this, base,
                                   alive = std::weak_ptr<int>(lifetime_)](RasterState state) mutable {
            if (alive.expired())
                return;
            base.raster = state;
            if (recipes_.emplace(key(base, true), base).second)
                dirty_ = true;
            if (preparing_)
                ++prepared_;
            else
                ++first_uses_;
        };
    } catch (...) {
        for (auto layout : layouts)
            if (layout)
                vkDestroyDescriptorSetLayout(device_.handles.device, layout, nullptr);
        throw;
    }
    // Pipeline layouts retain the definition, not the descriptor-layout handle.
    for (auto layout : layouts)
        vkDestroyDescriptorSetLayout(device_.handles.device, layout, nullptr);
    return entry;
}
void PipelineRecipes::prepare(Target &target) {
    const auto started = std::chrono::steady_clock::now();
    const auto compatible = target_class(target);
    const auto before = prepared_;
    // Preparing a different target class records its concrete descriptions too.
    // Snapshot the candidates so callback insertions cannot extend this traversal.
    std::vector<Recipe> candidates;
    for (const auto &entry : recipes_)
        if (portable_target(entry.second.target, compatible)) candidates.push_back(entry.second);
    preparing_ = true;
    try {
        for (const auto &r : candidates) {
            renderer(target, *shaders_.at(r.shader), r.bindings, r.attributes)->prepare(r.raster);
        }
    } catch (...) {
        preparing_ = false;
        throw;
    }
    preparing_ = false;
    if (prepared_ != before)
        std::fprintf(
            stderr, "[vulkan-recipes] retained %zu pipelines before target use (%ux%u x%u, format %u, density %u, origin %u), %.3f ms\n",
            prepared_ - before, target.info.width, target.info.height, target.info.layers,
            compatible[0], compatible[4], compatible[3],
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}
std::vector<std::shared_ptr<Renderer>> PipelineRecipes::retire(Target &target) {
    std::vector<std::shared_ptr<Renderer>> retired;
    for (auto i = renderers_.begin(); i != renderers_.end();)
        if (i->first.first == &target) {
            retired.push_back(std::move(i->second));
            i = renderers_.erase(i);
        } else
            ++i;
    return retired;
}
void PipelineRecipes::save() {
    if (path_.empty() || !dirty_)
        return;
    std::string data("LVPREC1\0", 8);
    word(data, uint32_t(recipes_.size()));
    for (auto &entry : recipes_)
        data += entry.first;
    const auto checksum = source_digest(data);
    data.append(reinterpret_cast<const char *>(checksum.data()), 32);
    const auto temporary = path_ + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.write(data.data(), std::streamsize(data.size())))
            throw std::runtime_error("Cannot write pipeline recipes");
    }
    if (std::rename(temporary.c_str(), path_.c_str()))
        throw std::runtime_error("Cannot replace pipeline recipes");
    dirty_ = false;
}
} // namespace lindbergh::vk
