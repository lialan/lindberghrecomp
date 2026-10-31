#version 120
uniform vec4 guestTint;
uniform sampler2D guestTexture;
varying vec2 sampleUV;
void main() { gl_FragColor = gl_Color * guestTint * texture2D(guestTexture, sampleUV); }
