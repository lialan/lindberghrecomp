#include <algorithm>
#include "guest_arrays.hpp"
#include <limits>
#include <cstring>
#include <stdexcept>

namespace lindbergh::vk {

const void *GuestMemoryView::read(uint32_t pointer, size_t count) const
{
    const auto offset = uint64_t(pointer) - address;
    if (!base || pointer < address || offset > bytes || count > bytes - offset ||
        count > (uint64_t(1) << 32) - pointer)
        throw std::out_of_range("Vulkan client array escapes the guest memory window");
    return static_cast<const unsigned char *>(base) + offset;
}

struct ArrayFormat { VkFormat format; uint32_t component_bytes; };
static ArrayFormat format(uint32_t type, uint32_t components, bool normalized)
{
    if (components < 1 || components > 4) throw std::invalid_argument("Invalid guest array component count");
    const auto n = components - 1;
    static const VkFormat fp[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    static const VkFormat un8[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    static const VkFormat sn8[] = {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM};
    static const VkFormat us8[] = {VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED};
    static const VkFormat ss8[] = {VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED};
    static const VkFormat un16[] = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM};
    static const VkFormat sn16[] = {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM};
    static const VkFormat us16[] = {VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED};
    static const VkFormat ss16[] = {VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED};
    switch (type) {
    case 0x1400: return {(normalized ? sn8 : ss8)[n], 1};  // GL_BYTE
    case 0x1401: return {(normalized ? un8 : us8)[n], 1}; // GL_UNSIGNED_BYTE
    case 0x1402: return {(normalized ? sn16 : ss16)[n], 2};
    case 0x1403: return {(normalized ? un16 : us16)[n], 2};
    case 0x1406: return {fp[n], 4}; // GL_FLOAT: normalization has no effect
    default: throw std::invalid_argument("Guest vertex type requires an explicit conversion path");
    }
}

bool GuestArrays::position(uint32_t index,const GuestMemoryView &memory,const CpuBufferLookup &buffers,
                           std::array<float,4> &out) const
{
    const auto &a=arrays_[0];
    if(!a.enabled || a.type!=0x1406 || a.components<2)return false;
    const auto stride=a.stride?a.stride:a.components*4;
    const uint64_t address=uint64_t(a.address)+uint64_t(index)*stride;
    if(address>UINT32_MAX)throw std::out_of_range("HUD position address overflow");
    const auto source=a.buffer?buffers(a.buffer):memory;
    out={0,0,0,1};std::memcpy(out.data(),source.read(uint32_t(address),a.components*4),a.components*4);
    return true;
}

bool GuestArrays::unit_range(uint32_t location) const
{
    const auto &a=arrays_.at(location);
    if(a.enabled)return a.normalized&&(a.type==0x1401||a.type==0x1403||a.type==0x1405); // GL_UNSIGNED_BYTE, _SHORT, _INT
    return std::all_of(a.value.begin(),a.value.end(),[](float v){return v>=0&&v<=1;});
}

void GuestArrays::pointer(uint32_t location, uint32_t components, uint32_t type,
                          bool normalized, uint32_t stride, uint32_t address)
{
    if (location >= arrays_.size()) throw std::out_of_range("Guest vertex attribute location");
    format(type, components, normalized); // reject unsupported semantics before mutating state
    auto &a = arrays_[location];
    a.components = components; a.type = type; a.normalized = normalized;
    a.stride = stride; a.address = address; a.buffer = bound_array_;
}

void GuestArrays::enable(uint32_t location, bool enabled)
{
    if (location >= arrays_.size()) throw std::out_of_range("Guest vertex attribute location");
    arrays_[location].enabled = enabled;
}
void GuestArrays::current(uint32_t location, const std::array<float, 4> &value)
{
    if (location >= arrays_.size()) throw std::out_of_range("Guest vertex attribute location");
    arrays_[location].value = value;
}
void GuestArrays::inputs(const std::vector<uint32_t> &locations)
{
    uint32_t seen = 0;
    for (const auto location : locations) {
        if (location >= arrays_.size() || (seen & (1u << location)))
            throw std::invalid_argument("Invalid or duplicate SPIR-V vertex input");
        seen |= 1u << location;
    }
    inputs_ = locations;
    plan_.bindings.reserve(locations.size()); plan_.attributes.reserve(locations.size()); plan_.streams.reserve(locations.size());
}

const ArrayPlan &GuestArrays::prepare(Device &device, FrameRing &ring, uint32_t first, uint32_t count,
                                     const GuestMemoryView &memory, const BufferLookup &buffers)
{
    if (!count || uint64_t(first) + count > uint64_t(std::numeric_limits<uint32_t>::max()) + 1)
        throw std::invalid_argument("Invalid guest vertex range");
    const auto &limits = device.capabilities.properties.limits;
    if (inputs_.size() > limits.maxVertexInputBindings)
        throw std::runtime_error("Guest shader exceeds Vulkan vertex binding limit");
    if (format_device_ != device.handles.physical) {
        format_support_.fill(0);
        format_device_ = device.handles.physical;
    }
    plan_.bindings.clear(); plan_.attributes.clear(); plan_.streams.clear();
    for (const auto location : inputs_) {
        const auto binding = static_cast<uint32_t>(plan_.bindings.size());
        const auto &a = arrays_[location];
        if (!a.enabled) {
            plan_.bindings.push_back({binding, 0, VK_VERTEX_INPUT_RATE_VERTEX});
            plan_.attributes.push_back({location, binding, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
            plan_.streams.push_back(ring.upload(a.value.data(), sizeof a.value));
            continue;
        }
        const auto f = format(a.type, a.components, a.normalized);
        const auto element = f.component_bytes * a.components;
        const uint32_t stride = a.stride ? a.stride : element;
        const uint64_t start = uint64_t(a.address) + uint64_t(first) * stride;
        const uint64_t bytes = uint64_t(count - 1) * stride + element;
        if (stride > limits.maxVertexInputBindingStride || start % f.component_bytes || stride % f.component_bytes)
            throw std::runtime_error("Guest vertex array needs an aligned conversion path");
        auto &supported = format_support_.at(static_cast<size_t>(f.format));
        if (!supported) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(device.handles.physical, f.format, &props);
            supported = (props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
        }
        if (supported != 1)
            throw std::runtime_error("GPU cannot directly fetch guest vertex format");
        Upload stream{};
        if (a.buffer) {
            const auto buffer = buffers(a.buffer);
            if (!buffer.buffer || start > buffer.bytes || bytes > buffer.bytes - start)
                throw std::out_of_range("Guest array exceeds bound buffer storage");
            stream = {buffer.buffer, start};
        } else {
            if (start > std::numeric_limits<uint32_t>::max() || bytes > std::numeric_limits<size_t>::max())
                throw std::out_of_range("Guest array address overflow");
            stream = ring.upload(memory.read(static_cast<uint32_t>(start), size_t(bytes)), size_t(bytes));
        }
        plan_.bindings.push_back({binding, stride, VK_VERTEX_INPUT_RATE_VERTEX});
        plan_.attributes.push_back({location, binding, f.format, 0});
        plan_.streams.push_back(stream);
    }
    return plan_;
}

} // namespace lindbergh::vk
