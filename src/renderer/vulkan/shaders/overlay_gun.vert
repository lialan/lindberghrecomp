#version 450
#extension GL_EXT_multiview : require
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec3 color;
layout(location=0) out vec3 vertex_color;
layout(push_constant) uniform Matrices { mat4 view_projection[2]; } matrices;
layout(set=0,binding=0,std140) uniform Overlay { mat4 model;vec4 color;vec4 shape; } overlay;
void main() {
    vec3 wn=normalize(mat3(overlay.model)*normal);
    float light=.35+.65*max(dot(wn,normalize(vec3(.3,1,.4))),0);
    float rim=pow(1-abs(wn.z),3)*.08;
    vertex_color=color*light+rim;
    vec4 p=matrices.view_projection[gl_ViewIndex]*vec4(position,1);
    gl_Position=vec4(p.x,-p.y,(p.z+p.w)*.5,p.w);
}
