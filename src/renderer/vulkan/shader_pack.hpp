#pragma once
#include "renderer.hpp"
#include <string_view>

namespace lindbergh::vk {

using SourceDigest = std::array<uint8_t, 32>;
SourceDigest source_digest(std::string_view source);
struct AttributeBinding { std::string name; uint32_t location; };
struct ShaderInput { uint32_t location; std::string type; };
struct UniformField {
    std::string name;
    uint32_t offset, scalar, columns, rows, count, array_stride, matrix_stride;
};
struct UniformLayout {
    uint32_t set, binding, bytes;
    std::vector<UniformField> fields;
};
struct ShaderSampler { uint32_t binding; std::string name, type; uint32_t count; };
struct ShaderRecord {
    SourceDigest key, vertex_source, fragment_source;
    std::vector<uint32_t> vertex, fragment;
    std::vector<AttributeBinding> attributes;
    std::vector<ShaderInput> inputs;
    std::vector<UniformLayout> uniforms;
    std::vector<ShaderSampler> samplers;
};

// Read once during loading. This binary archive contains validated SPIR-V and
// build-time reflection; neither a GLSL compiler nor a JSON parser is linked.
class ShaderPack {
public:
    explicit ShaderPack(const std::string &path);
    const std::vector<ShaderRecord> &programs() const { return programs_; }
    // Called on guest program link, never per draw. Unknown sources/bindings
    // return null, allowing HLE to report a missing offline shader variant.
    const ShaderRecord *find(const SourceDigest &vertex, const SourceDigest &fragment,
                             const std::vector<AttributeBinding> &bindings) const;
private:
    std::vector<ShaderRecord> programs_;
};

// std140 backing storage for one reflected UBO. Locations are resolved once
// by HLE; writes preserve padding, array/matrix strides and transpose semantics.
class UniformData {
public:
    explicit UniformData(const UniformLayout &layout);
    int find(std::string_view name) const;
    void write(uint32_t field, const void *values, size_t bytes,
               uint32_t first = 0, uint32_t count = 1, bool transpose = false);
    const std::vector<uint8_t> &bytes() const { return data_; }
    uint64_t version() const { return version_; }
private:
    const UniformLayout &layout_;
    std::vector<uint8_t> data_;
    uint64_t version_ = 0;
};

} // namespace lindbergh::vk
