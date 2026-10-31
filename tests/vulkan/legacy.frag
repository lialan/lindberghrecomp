#version 120
uniform vec4 guestTint;
void main() { gl_FragColor = gl_Color * guestTint; }
