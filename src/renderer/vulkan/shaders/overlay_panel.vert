#version 450
#extension GL_EXT_multiview : require
layout(location=0) in vec2 position;
layout(location=0) out vec2 uv;
layout(push_constant) uniform Matrices { mat4 view_projection[2]; } matrices;
void main() {
    uv=vec2(.5+.5*position.x,.5-.5*position.y);
    vec4 p=matrices.view_projection[gl_ViewIndex]*vec4(position,0,1);
    gl_Position=vec4(p.x,-p.y,(p.z+p.w)*.5,p.w);
}
