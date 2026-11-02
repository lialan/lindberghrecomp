#include "shader_pack.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace lindbergh::vk {
namespace {
// SHA-256 is used only at guest shader link/load time. The archive stores the
// same digests emitted by Python hashlib; no hashing enters the draw path.
uint32_t rotate(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
void digest_block(std::array<uint32_t, 8> &h, const uint8_t *data)
{
    static constexpr uint32_t k[] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = (uint32_t(data[4*i]) << 24) | (uint32_t(data[4*i+1]) << 16) |
               (uint32_t(data[4*i+2]) << 8) | data[4*i+3];
    for (unsigned i = 16; i < 64; ++i) {
        const auto a = w[i-15], b = w[i-2];
        w[i] = w[i-16] + (rotate(a,7) ^ rotate(a,18) ^ (a >> 3)) + w[i-7] +
               (rotate(b,17) ^ rotate(b,19) ^ (b >> 10));
    }
    auto a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], v=h[7];
    for (unsigned i = 0; i < 64; ++i) {
        const auto t1 = v + (rotate(e,6)^rotate(e,11)^rotate(e,25)) + ((e&f)^(~e&g)) + k[i] + w[i];
        const auto t2 = (rotate(a,2)^rotate(a,13)^rotate(a,22)) + ((a&b)^(a&c)^(b&c));
        v=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=v;
}

class Reader {
public:
    explicit Reader(const std::string &path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("Cannot open Vulkan shader pack: " + path);
        const auto size = input.tellg();
        if (size < 16 || size > (256 << 20)) throw std::runtime_error("Invalid Vulkan shader pack size");
        bytes.resize(static_cast<size_t>(size)); input.seekg(0);
        if (!input.read(reinterpret_cast<char *>(bytes.data()), size)) throw std::runtime_error("Truncated Vulkan shader pack");
    }
    void read(void *to, size_t n) {
        if (n > bytes.size() - at) throw std::runtime_error("Truncated Vulkan shader pack record");
        if (n) std::memcpy(to, bytes.data() + at, n);
        at += n;
    }
    uint32_t u32() {
        uint8_t v[4]; read(v, 4);
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }
    uint32_t count(uint32_t max) {
        const auto n = u32();
        if (n > max) throw std::runtime_error("Invalid Vulkan shader pack record count");
        return n;
    }
    std::string string() {
        const auto n = count(1 << 20);
        std::string s(n, 0); read(s.data(), n); return s;
    }
    std::vector<uint32_t> module() {
        const auto n = count(4 << 20);
        if (n < 5) throw std::runtime_error("Short Vulkan SPIR-V module");
        std::vector<uint32_t> result(n);
        for (auto &word : result) word = u32();
        if (result[0] != 0x07230203) throw std::runtime_error("Invalid Vulkan SPIR-V header");
        return result;
    }
    bool done() const { return at == bytes.size(); }
private:
    std::vector<uint8_t> bytes;
    size_t at = 0;
};

void validate(const UniformLayout &block, const UniformField &f)
{
    if (f.scalar > 2 || !f.columns || f.columns > 4 || !f.rows || f.rows > 4 || !f.count ||
        (f.count > 1 && !f.array_stride) || (f.columns > 1 && !f.matrix_stride))
        throw std::runtime_error("Invalid Vulkan uniform field shape");
    const auto end = uint64_t(f.offset) + uint64_t(f.count - 1) * f.array_stride +
                     uint64_t(f.columns - 1) * f.matrix_stride + f.rows * 4;
    if (end > block.bytes) throw std::runtime_error("Vulkan uniform field exceeds block size");
}
} // namespace

SourceDigest source_digest(std::string_view source)
{
    std::array<uint32_t, 8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const auto bytes = reinterpret_cast<const uint8_t *>(source.data());
    size_t offset = 0;
    while (source.size() - offset >= 64) { digest_block(h, bytes + offset); offset += 64; }
    uint8_t tail[128]{};
    const auto rest = source.size() - offset;
    if (rest) std::memcpy(tail, bytes + offset, rest);
    tail[rest] = 0x80;
    const unsigned end = rest < 56 ? 64 : 128;
    const uint64_t bits = uint64_t(source.size()) * 8;
    for (unsigned i = 0; i < 8; ++i) tail[end-1-i] = static_cast<uint8_t>(bits >> (i*8));
    digest_block(h, tail);
    if (end == 128) digest_block(h, tail + 64);
    SourceDigest result;
    for (unsigned i = 0; i < 32; ++i) result[i] = static_cast<uint8_t>(h[i/4] >> (24 - (i%4)*8));
    return result;
}

SourceDigest shader_source_digest(std::string_view source)
{
    if (source.empty() || (source.back() == '\n' && source.find('\r') == std::string_view::npos))
        return source_digest(source);
    std::string normalized(source);
    std::replace(normalized.begin(), normalized.end(), '\r', ' ');
    if (normalized.back() != '\n') normalized.push_back('\n');
    return source_digest(normalized);
}

ShaderPack::ShaderPack(const std::string &path)
{
    Reader r(path);
    char magic[8]; r.read(magic, 8);
    if (std::memcmp(magic, "LVSPV001", 8) || r.u32() != 1) throw std::runtime_error("Unsupported Vulkan shader pack version");
    const auto count = r.count(65536);
    programs_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ShaderRecord p;
        r.read(p.key.data(), p.key.size());
        r.read(p.vertex_source.data(), p.vertex_source.size()); p.vertex = r.module();
        r.read(p.fragment_source.data(), p.fragment_source.size()); p.fragment = r.module();
        const auto attrs = r.count(32);
        for (uint32_t j = 0; j < attrs; ++j) {
            auto name = r.string(); auto location = r.count(31);
            p.attributes.push_back({std::move(name), location});
        }
        const auto inputs = r.count(32);
        for (uint32_t j = 0; j < inputs; ++j) {
            const auto location = r.count(31); auto type = r.string();
            p.inputs.push_back({location, std::move(type)});
        }
        const auto blocks = r.count(16);
        for (uint32_t j = 0; j < blocks; ++j) {
            UniformLayout b;
            b.set = r.count(31); b.binding = r.u32(); b.bytes = r.count(1 << 20);
            const auto fields = r.count(65536);
            for (uint32_t k = 0; k < fields; ++k) {
                UniformField f;
                f.name=r.string(); f.offset=r.u32(); f.scalar=r.u32(); f.columns=r.u32(); f.rows=r.u32();
                f.count=r.u32(); f.array_stride=r.u32(); f.matrix_stride=r.u32();
                validate(b, f); b.fields.push_back(std::move(f));
            }
            p.uniforms.push_back(std::move(b));
        }
        const auto samplers = r.count(128);
        for (uint32_t j = 0; j < samplers; ++j) {
            ShaderSampler s;
            s.binding=r.u32(); s.name=r.string(); s.type=r.string(); s.count=r.count(128);
            if (!s.count) throw std::runtime_error("Empty Vulkan sampler array");
            p.samplers.push_back(std::move(s));
        }
        programs_.push_back(std::move(p));
    }
    if (!r.done()) throw std::runtime_error("Trailing Vulkan shader pack data");
}

void write_shader_pack(const std::string &path, const std::vector<const ShaderRecord *> &records)
{
    std::string data("LVSPV001", 8);
    auto u32 = [&](uint32_t v) { for (unsigned i = 0; i < 4; ++i) data.push_back(char(v >> (8 * i))); };
    auto string = [&](const std::string &s) { u32(uint32_t(s.size())); data += s; };
    auto bytes = [&](const SourceDigest &d) { data.append(reinterpret_cast<const char *>(d.data()), d.size()); };
    auto module = [&](const std::vector<uint32_t> &m) { u32(uint32_t(m.size())); for (auto w : m) u32(w); };
    u32(1); u32(uint32_t(records.size()));
    for (const auto *p : records) {
        bytes(p->key);
        bytes(p->vertex_source); module(p->vertex);
        bytes(p->fragment_source); module(p->fragment);
        u32(uint32_t(p->attributes.size()));
        for (const auto &a : p->attributes) { string(a.name); u32(a.location); }
        u32(uint32_t(p->inputs.size()));
        for (const auto &i : p->inputs) { u32(i.location); string(i.type); }
        u32(uint32_t(p->uniforms.size()));
        for (const auto &b : p->uniforms) {
            u32(b.set); u32(b.binding); u32(b.bytes); u32(uint32_t(b.fields.size()));
            for (const auto &f : b.fields) {
                string(f.name);
                for (auto v : {f.offset, f.scalar, f.columns, f.rows, f.count, f.array_stride, f.matrix_stride}) u32(v);
            }
        }
        u32(uint32_t(p->samplers.size()));
        for (const auto &s : p->samplers) { u32(s.binding); string(s.name); string(s.type); u32(s.count); }
    }
    const auto temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.write(data.data(), std::streamsize(data.size()))) throw std::runtime_error("Cannot write Vulkan shader pack: " + temporary);
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) throw std::runtime_error("Cannot replace Vulkan shader pack: " + path);
}

const ShaderRecord *ShaderPack::find(const SourceDigest &vertex, const SourceDigest &fragment,
                                     const std::vector<AttributeBinding> &bindings) const
{
    for (const auto &p : programs_) {
        if (p.vertex_source != vertex || p.fragment_source != fragment) continue;
        bool matches = true;
        for (const auto &a : p.attributes) {
            const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto &b) { return a.name == b.name; });
            if (found == bindings.end() || found->location != a.location) { matches = false; break; }
        }
        if (matches) return &p;
    }
    return nullptr;
}

UniformData::UniformData(const UniformLayout &layout) : layout_(layout), data_(layout.bytes)
{
    for (const auto &field : layout.fields) validate(layout, field);
}
int UniformData::find(std::string_view name) const
{
    for (size_t i = 0; i < layout_.fields.size(); ++i) if (layout_.fields[i].name == name) return static_cast<int>(i);
    return -1;
}
void UniformData::write(uint32_t field, const void *values, size_t bytes, uint32_t first, uint32_t count, bool transpose)
{
    if (field >= layout_.fields.size()) throw std::out_of_range("Unknown Vulkan uniform field");
    const auto &f = layout_.fields[field];
    if (!values || !count || first > f.count || count > f.count - first ||
        bytes != uint64_t(count) * f.columns * f.rows * 4)
        throw std::invalid_argument("Vulkan uniform write type/array size mismatch");
    const auto source_bytes = static_cast<const uint8_t *>(values);
    const size_t column_bytes = f.rows * 4;
    const auto base = size_t(f.offset) + size_t(first) * f.array_stride;
    const bool packed_columns = f.columns == 1 || f.matrix_stride == column_bytes;
    const bool packed_elements = count == 1 || f.array_stride == f.columns * column_bytes;
    if (!transpose && packed_columns && packed_elements) {
        if (std::memcmp(data_.data() + base, source_bytes, bytes)) {
            std::memcpy(data_.data() + base, source_bytes, bytes); ++version_;
        }
        return;
    }
    bool changed = false;
    for (uint32_t element = 0; element < count; ++element)
        for (uint32_t col = 0; col < f.columns; ++col) {
            const auto target = base + size_t(element) * f.array_stride + col * f.matrix_stride;
            if (!transpose) {
                const auto source = (size_t(element) * f.columns + col) * column_bytes;
                if (std::memcmp(data_.data() + target, source_bytes + source, column_bytes)) {
                    std::memcpy(data_.data() + target, source_bytes + source, column_bytes); changed = true;
                }
            } else for (uint32_t row = 0; row < f.rows; ++row) {
                const auto source = (size_t(element) * f.rows * f.columns + row * f.columns + col) * 4;
                if (std::memcmp(data_.data() + target + row * 4, source_bytes + source, 4)) {
                    std::memcpy(data_.data() + target + row * 4, source_bytes + source, 4); changed = true;
                }
            }
        }
    if (changed) ++version_;
}

} // namespace lindbergh::vk
