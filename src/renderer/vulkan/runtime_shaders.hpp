#pragma once
#include "shader_pack.hpp"
#include <string>

namespace lindbergh::vk {

// A guest program the game's offline pack lacks, compiled on the device: the
// lowering of tools/vulkan/compile_shaders.py, glslang with its options (no
// SPIR-V optimizer), and the record tools/vulkan/pack_shaders.py would write.
// The game builds its shaders at run time (Ghost Squad Evolution pairs ~730
// texture-blend fragment variants with vertex programs assembled from 111
// pieces), so no harvest covers every pair; without this, a pair the pack
// lacks has its draws skipped.
//
// vertex/fragment: the guest's sources as linked; bindings: its explicit
// attribute locations. False, with error set, when the pair cannot be built.
bool compile_guest_program(const std::string &vertex, const std::string &fragment,
                           const std::vector<AttributeBinding> &bindings,
                           ShaderRecord &out, std::string &error);

// Names this lowering (its GLSL prelude and converter version): programs
// compiled and saved by another one are not reused.
std::string runtime_shader_generation();

// Reflection of one SPIR-V module, as pack_shaders.py reads spirv-cross's:
// what the record needs, for statically used resources only (the offline
// pack's modules are optimized, so unused ones are gone from them).
struct SpirvReflection {
    struct Input { uint32_t location; std::string name, type; };
    struct Block { uint32_t set, binding, bytes; std::vector<UniformField> fields; };
    struct Sampler { uint32_t set, binding, count; std::string name, type; };
    std::vector<Input> inputs;
    std::vector<Block> blocks;
    std::vector<Sampler> samplers;
};
bool reflect_spirv(const std::vector<uint32_t> &module, SpirvReflection &out, std::string &error);

} // namespace lindbergh::vk
