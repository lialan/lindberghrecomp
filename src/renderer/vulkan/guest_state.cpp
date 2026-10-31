#include "guest_state.hpp"
#include <algorithm>
#include <cstring>

namespace lindbergh::vk {

Matrix identity()
{
    Matrix out{};
    for (unsigned i = 0; i < 4; i++) out[i * 5] = 1;
    return out;
}

Matrix multiply(const Matrix &left, const Matrix &right)
{
    Matrix out{};
    for (unsigned col = 0; col < 4; col++)
        for (unsigned row = 0; row < 4; row++)
            for (unsigned k = 0; k < 4; k++) out[col * 4 + row] += left[k * 4 + row] * right[col * 4 + k];
    return out;
}

GuestState::Stack::Stack() { matrices[0] = identity(); }
GuestState::GuestState() : mvp_(identity())
{
    batch_.vertices.reserve(256);
    batch_.indices.reserve(384);
}

void GuestState::fail(uint32_t e) { if (!error_) error_ = e; }
uint32_t GuestState::error() { auto e = error_; error_ = 0; return e; }

GuestState::Stack &GuestState::stack()
{
    return mode_ == guest::Modelview ? modelview_ : mode_ == guest::Projection ? projection_ : texture_[texture_unit_];
}
const GuestState::Stack &GuestState::stack() const
{
    return mode_ == guest::Modelview ? modelview_ : mode_ == guest::Projection ? projection_ : texture_[texture_unit_];
}
const Matrix &GuestState::current_matrix() const { return stack().matrices[stack().top]; }

bool GuestState::matrix_allowed()
{
    if (inside_) { fail(guest::InvalidOperation); return false; }
    return true;
}

void GuestState::matrix_mode(uint32_t mode)
{
    if (!matrix_allowed()) return;
    if (mode != guest::Modelview && mode != guest::Projection && mode != guest::Texture) { fail(guest::InvalidEnum); return; }
    mode_ = mode;
}

void GuestState::active_texture(unsigned unit)
{
    if (!matrix_allowed()) return;
    if (unit >= texture_.size()) { fail(guest::InvalidEnum); return; }
    texture_unit_ = unit;
}

void GuestState::push_matrix()
{
    if (!matrix_allowed()) return;
    auto &s = stack();
    if (s.top == s.matrices.size() - 1) { fail(guest::StackOverflow); return; }
    s.matrices[s.top + 1] = s.matrices[s.top]; ++s.top;
}

void GuestState::pop_matrix()
{
    if (!matrix_allowed()) return;
    auto &s = stack();
    if (!s.top) { fail(guest::StackUnderflow); return; }
    --s.top; dirty_ = true;
}

void GuestState::load_identity() { const auto m = identity(); load_matrix(m.data()); }

void GuestState::load_matrix(const float *m)
{
    if (!matrix_allowed()) return;
    auto &s = stack();
    std::memcpy(s.matrices[s.top].data(), m, 16 * sizeof(float));
    dirty_ = true;
}

void GuestState::mult_matrix(const float *m)
{
    if (!matrix_allowed()) return;
    Matrix right;
    std::memcpy(right.data(), m, sizeof right);
    auto &s = stack();
    s.matrices[s.top] = multiply(s.matrices[s.top], right);
    dirty_ = true;
}

void GuestState::translate(float x, float y, float z)
{
    auto m = identity(); m[12] = x; m[13] = y; m[14] = z; mult_matrix(m.data());
}

void GuestState::scale(float x, float y, float z)
{
    auto m = identity(); m[0] = x; m[5] = y; m[10] = z; mult_matrix(m.data());
}

void GuestState::ortho(double left, double right, double bottom, double top, double near_z, double far_z)
{
    if (!matrix_allowed()) return;
    if (left == right || bottom == top || near_z == far_z) { fail(guest::InvalidValue); return; }
    auto m = identity();
    m[0] = float(2 / (right - left)); m[5] = float(2 / (top - bottom)); m[10] = float(-2 / (far_z - near_z));
    m[12] = float(-(right + left) / (right - left));
    m[13] = float(-(top + bottom) / (top - bottom));
    m[14] = float(-(far_z + near_z) / (far_z - near_z));
    mult_matrix(m.data());
}

Matrix GuestState::modelview_projection()
{
    if (dirty_) {
        mvp_ = multiply(projection_.matrices[projection_.top], modelview_.matrices[modelview_.top]);
        dirty_ = false;
    }
    return mvp_;
}

void GuestState::begin(uint32_t mode)
{
    if (inside_) { fail(guest::InvalidOperation); return; }
    if (mode > guest::Polygon) { fail(guest::InvalidEnum); return; }
    primitive_ = mode; inside_ = true;
    batch_.vertices.clear(); batch_.indices.clear();
}

void GuestState::color(float r, float g, float b, float a)
{
    const float color[4] = {r, g, b, a};
    std::copy(color, color + 4, color_);
}

void GuestState::vertex(float x, float y, float z, float w)
{
    if (!inside_) { fail(guest::InvalidOperation); return; }
    Vertex v{{x, y, z, w}, {color_[0], color_[1], color_[2], color_[3]}};
    batch_.vertices.push_back(v);
}

const Geometry *GuestState::end()
{
    if (!inside_) { fail(guest::InvalidOperation); return nullptr; }
    inside_ = false;
    const auto n = static_cast<uint32_t>(batch_.vertices.size());
    auto &indices = batch_.indices;
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) { indices.push_back(a); indices.push_back(b); indices.push_back(c); };
    auto line = [&](uint32_t a, uint32_t b) { indices.push_back(a); indices.push_back(b); };
    batch_.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    switch (primitive_) {
    case guest::Points:
        batch_.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        for (uint32_t i = 0; i < n; i++) indices.push_back(i);
        break;
    case guest::Lines:
        batch_.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        for (uint32_t i = 0; i + 1 < n; i += 2) line(i, i + 1);
        break;
    case guest::LineStrip:
    case guest::LineLoop:
        batch_.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        for (uint32_t i = 1; i < n; i++) line(i - 1, i);
        if (primitive_ == guest::LineLoop && n > 1) line(n - 1, 0);
        break;
    case guest::Triangles:
        for (uint32_t i = 0; i + 2 < n; i += 3) tri(i, i + 1, i + 2);
        break;
    case guest::TriangleStrip:
        for (uint32_t i = 2; i < n; i++) tri(i & 1 ? i - 1 : i - 2, i & 1 ? i - 2 : i - 1, i);
        break;
    case guest::TriangleFan:
    case guest::Polygon:
        for (uint32_t i = 2; i < n; i++) tri(0, i - 1, i);
        break;
    case guest::Quads:
        for (uint32_t i = 0; i + 3 < n; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); }
        break;
    case guest::QuadStrip:
        for (uint32_t i = 0; i + 3 < n; i += 2) { tri(i, i + 1, i + 3); tri(i, i + 3, i + 2); }
        break;
    }
    return &batch_;
}

} // namespace lindbergh::vk
