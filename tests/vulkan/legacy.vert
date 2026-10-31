#version 120
varying vec2 sampleUV;
void main() {
    gl_Position = gl_Vertex;
    gl_FrontColor = gl_Color;
    sampleUV = gl_Vertex.xy * 0.5 + 0.5;
}
