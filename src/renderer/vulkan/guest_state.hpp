#pragma once
#include "renderer.hpp"
#include <vector>

namespace lindbergh::vk {

// Guest GL enum values only. These are decoded by the HLE frontend; no host GL
// headers, function pointers or contexts enter the Vulkan renderer.
namespace guest {
enum : uint32_t {
    NoError = 0, InvalidEnum = 0x0500, InvalidValue = 0x0501,
    InvalidOperation = 0x0502, StackOverflow = 0x0503, StackUnderflow = 0x0504,
    Points = 0, Lines = 1, LineLoop = 2, LineStrip = 3, Triangles = 4,
    TriangleStrip = 5, TriangleFan = 6, Quads = 7, QuadStrip = 8, Polygon = 9,
    Modelview = 0x1700, Projection = 0x1701, Texture = 0x1702,
    Matrix0ARB = 0x88C0 // ARB program matrices GL_MATRIX0_ARB..GL_MATRIX7_ARB
};
}

using Matrix = std::array<float, 16>;
Matrix identity();
Matrix multiply(const Matrix &left, const Matrix &right);

struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
};

// CPU state mirrors the guest API once, immediately adjacent to the HLE ABI.
// Storage is reused between batches, and matrices are composed only when dirty.
class GuestState {
public:
    GuestState();
    void matrix_mode(uint32_t mode);
    void active_texture(unsigned unit);
    void push_matrix();
    void pop_matrix();
    void load_identity();
    void load_matrix(const float *column_major);
    void mult_matrix(const float *column_major);
    void translate(float x, float y, float z);
    void scale(float x, float y, float z);
    void ortho(double left, double right, double bottom, double top, double near_z, double far_z);
    Matrix modelview_projection();
    const Matrix &current_matrix() const;
    const Matrix &matrix(uint32_t mode, unsigned unit=0) const;
    void begin(uint32_t mode);
    void color(float r, float g, float b, float a);
    void vertex(float x, float y, float z = 0, float w = 1);
    // Returned batch storage remains valid until the next begin().
    const Geometry *end();
    uint32_t error();

private:
    struct Stack {
        std::array<Matrix, 32> matrices;
        unsigned top = 0;
        Stack();
    };
    Stack modelview_, projection_;
    std::array<Stack, 8> texture_, program_;
    uint32_t mode_ = guest::Modelview, error_ = 0, primitive_ = 0;
    unsigned texture_unit_ = 0;
    bool inside_ = false, dirty_ = true;
    Matrix mvp_;
    float color_[4] = {1, 1, 1, 1};
    Geometry batch_;
    Stack &stack();
    const Stack &stack() const;
    bool matrix_allowed();
    void fail(uint32_t error);
};

} // namespace lindbergh::vk
