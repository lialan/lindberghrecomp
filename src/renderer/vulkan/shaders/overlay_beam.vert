#version 450
#extension GL_EXT_multiview : require
layout(location=0) in vec3 position_left;
layout(location=1) in vec3 position_right;
layout(location=2) in vec2 uv;
layout(location=0) out vec2 texcoord;
layout(push_constant) uniform Matrices { mat4 view_projection[2]; } matrices;
void main(){
    vec4 p=matrices.view_projection[gl_ViewIndex]*vec4(gl_ViewIndex==0?position_left:position_right,1);
    gl_Position=vec4(p.x,-p.y,(p.z+p.w)*.5,p.w);texcoord=uv;
}
