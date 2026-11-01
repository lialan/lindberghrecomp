#include "guest_pixels.hpp"
#include <stdexcept>
#include <utility>
namespace lindbergh::vk::guest {
uint32_t canonical_color_storage(uint32_t internal, uint32_t format, uint32_t type,
                                 uint32_t width, uint32_t height, std::vector<uint8_t> &bytes)
{
    const bool alpha_storage = internal == 4 || internal == RGBA || internal == RGBA8;
    const bool color = alpha_storage || internal == 3 || internal == RGB || internal == RGB8;
    if (!color || type != UnsignedByte ||
        (format != Luminance && format != LuminanceAlpha && format != Alpha && format != Red)) return format;
    const bool alpha_only = format == Alpha, red_only = format == Red, has_alpha = format == LuminanceAlpha;
    const uint64_t pixels = uint64_t(width) * height;
    if (pixels > bytes.max_size()/4 || pixels*(has_alpha?2:1) != bytes.size())
        throw std::invalid_argument("Invalid guest color upload size");
    const auto input = std::move(bytes);
    bytes.resize(size_t(pixels)*4);
    for (size_t i=0;i<pixels;++i) {
        const auto value=input[i*(has_alpha?2:1)];
        bytes[4*i]=alpha_only?0:value;
        bytes[4*i+1]=bytes[4*i+2]=(alpha_only||red_only)?0:value;
        bytes[4*i+3]=!alpha_storage?255:alpha_only?value:has_alpha?input[2*i+1]:255;
    }
    return RGBA;
}
}
