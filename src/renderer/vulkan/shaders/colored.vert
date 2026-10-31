#version 450
#extension GL_EXT_multiview : require
layout(location=0) in vec4 position;
layout(location=1) in vec4 color;
layout(location=0) out vec4 vertex_color;
layout(push_constant) uniform Matrices { mat4 view_projection[2]; } matrices;
void main() {
    vec4 p = matrices.view_projection[gl_ViewIndex] * position;
    // Guest clip space is GL's: Y up and Z in [-W,W]. Vulkan's framebuffer
    // uses the opposite Y direction and Z in [0,W]. Do this exactly once.
    gl_Position = vec4(p.x, -p.y, (p.z + p.w) * 0.5, p.w);
    gl_PointSize = 1.0;
    vertex_color = color;
}
