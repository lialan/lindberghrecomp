#pragma once
#include <cstdint>
#include <vector>
namespace lindbergh::vk::guest {
enum PixelFormat : uint32_t {
    Red = 0x1903, Alpha = 0x1906, RGB = 0x1907, RGBA = 0x1908,
    Luminance = 0x1909, LuminanceAlpha = 0x190a,
    RGB8 = 0x8051, RGBA8 = 0x8058, UnsignedByte = 0x1401
};
// An upload's channel count does not define internal texture storage. Expand
// narrow uploads into requested RGB(A) storage once, before native allocation.
// Returns the canonical upload format. Packed/compressed paths stay unchanged.
uint32_t canonical_color_storage(uint32_t internal, uint32_t format, uint32_t type,
                                 uint32_t width, uint32_t height, std::vector<uint8_t> &bytes);
}
