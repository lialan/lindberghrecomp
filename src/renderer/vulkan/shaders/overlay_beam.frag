#version 450
layout(location=0) in vec2 texcoord;
layout(location=0) out vec4 color;
layout(set=0,binding=0,std140) uniform Overlay { mat4 model;vec4 color;vec4 shape; } overlay;
void main(){
    float d=length(texcoord);
    float a=overlay.shape.y>0?clamp(1-abs(d-overlay.shape.x)/overlay.shape.y,0,1):clamp(1-d,0,1);
    color=vec4(overlay.color.rgb,overlay.color.a*a*a);
}
