#pragma once
#include "renderer.hpp"
#include <functional>

namespace lindbergh::vk {

struct GuestMemoryView {
    const void *base;
    uint64_t bytes;
    uint32_t address = 0;
    const void *read(uint32_t pointer, size_t count) const;
};
struct GuestBufferView { VkBuffer buffer; VkDeviceSize bytes; };
using CpuBufferLookup = std::function<GuestMemoryView(uint32_t)>;
using BufferLookup = std::function<GuestBufferView(uint32_t)>;

struct ArrayPlan {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<Upload> streams;
};

// Shared guest vertex-array state. Legacy semantics use canonical locations
// (vertex=0, normal=2, color=3, texcoord=8+unit), matching offline shader inputs.
// Buffer names are captured by pointer(), not looked up from the current bind
// at draw time. Client addresses always remain 32-bit guest addresses.
class GuestArrays {
public:
    void bind_array_buffer(uint32_t name) { bound_array_ = name; }
    void pointer(uint32_t location, uint32_t components, uint32_t type,
                 bool normalized, uint32_t stride, uint32_t address);
    void enable(uint32_t location, bool enabled);
    void current(uint32_t location, const std::array<float, 4> &value);
    // Configure the active SPIR-V input locations only when the program changes.
    const std::array<float,4> &current_value(uint32_t location) const { return arrays_.at(location).value; }
    void inputs(const std::vector<uint32_t> &locations);
    // Prepare before render-pass recording. Static VBOs bind directly. Dynamic
    // client arrays retain their original formats and need one bounded upload;
    // there is no intermediate CPU vertex expansion or GL driver state.
    // The returned storage is reused and valid until the next prepare().
    // Bounded HUD/viewport classification reads CPU guest shadows only.
    bool position(uint32_t index,const GuestMemoryView &memory,const CpuBufferLookup &buffers,
                  std::array<float,4> &out) const;
    const ArrayPlan &prepare(Device &device, FrameRing &ring, uint32_t first, uint32_t count,
                             const GuestMemoryView &memory, const BufferLookup &buffers);
private:
    struct Attribute {
        bool enabled = false, normalized = false;
        uint32_t components = 4, type = 0x1406, stride = 0, address = 0, buffer = 0;
        std::array<float, 4> value{0, 0, 0, 1};
    };
    std::array<Attribute, 16> arrays_;
    std::vector<uint32_t> inputs_;
    uint32_t bound_array_ = 0;
    ArrayPlan plan_;
    VkPhysicalDevice format_device_ = VK_NULL_HANDLE;
    std::array<uint8_t, 128> format_support_{};
};

} // namespace lindbergh::vk
