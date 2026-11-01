// Offline lowering: guest 2D textures are sampled through array views. Static
// textures retain one physical layer. Vulkan array-layer selection clamps the
// eye index to that view's layer count, with no textureSize query or shader
// branch. Render-target copies carry two physical layers and select each eye.
// https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-unnormalized-to-integer
vec4 legacy_texture2D(sampler2DArray s, vec2 p) {
    return texture(s, vec3(p, float(gl_ViewIndex)));
}
vec4 legacy_texture2DLod(sampler2DArray s, vec2 p, float lod) {
    return textureLod(s, vec3(p, float(gl_ViewIndex)), lod);
}
vec4 legacy_texture2DProj(sampler2DArray s, vec3 p) {
    return legacy_texture2D(s, p.xy / p.z);
}
vec4 legacy_texture2DProj(sampler2DArray s, vec4 p) {
    return legacy_texture2D(s, p.xy / p.w);
}
vec4 legacy_texture2DProjLod(sampler2DArray s, vec3 p, float lod) {
    return legacy_texture2DLod(s, p.xy / p.z, lod);
}
vec4 legacy_texture2DProjLod(sampler2DArray s, vec4 p, float lod) {
    return legacy_texture2DLod(s, p.xy / p.w, lod);
}
// Rectangle textures are sampled in texels. ARB program translations scale
// those by u_rect_scale for render targets drawn larger; dividing by the
// image's own size gives the normalized lookup of its array view.
vec4 legacy_texture2DRect(sampler2DArray s, vec2 p) {
    return legacy_texture2D(s, p / vec2(textureSize(s, 0).xy));
}
vec4 legacy_texture2DRectProj(sampler2DArray s, vec3 p) {
    return legacy_texture2DRect(s, p.xy / p.z);
}
vec4 legacy_texture2DRectProj(sampler2DArray s, vec4 p) {
    return legacy_texture2DRect(s, p.xy / p.w);
}
vec4 legacy_shadow2D(sampler2DArrayShadow s, vec3 p) {
    return vec4(texture(s, vec4(p.xy, float(gl_ViewIndex), p.z)));
}
vec4 legacy_shadow2DProj(sampler2DArrayShadow s, vec4 p) {
    return legacy_shadow2D(s, p.xyz / p.w);
}
// FRAGMENT_BIAS_OVERLOADS
vec4 legacy_texture2D(sampler2DArray s, vec2 p, float bias) {
    return texture(s, vec3(p, float(gl_ViewIndex)), bias);
}
vec4 legacy_texture2DProj(sampler2DArray s, vec3 p, float bias) {
    return legacy_texture2D(s, p.xy / p.z, bias);
}
vec4 legacy_texture2DProj(sampler2DArray s, vec4 p, float bias) {
    return legacy_texture2D(s, p.xy / p.w, bias);
}
