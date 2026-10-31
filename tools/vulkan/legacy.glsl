// Build-time compatibility declarations, consumed by the direct SPIR-V
// compiler. These are shader data structures, not calls to a host GL driver.
struct LegacyMaterial {
    vec4 emission;
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
    float shininess;
};
struct LegacyLight {
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
    vec4 position;
    vec4 halfVector;
    vec3 spotDirection;
    float spotExponent;
    float spotCutoff;
    float spotCosCutoff;
    float constantAttenuation;
    float linearAttenuation;
    float quadraticAttenuation;
};
struct LegacyFog {
    vec4 color;
    float density;
    float start;
    float end;
    float scale;
};
struct LegacyLightModelProduct { vec4 sceneColor; };
struct LegacyDepthRange { float near; float far; float diff; };
layout(std140, set=0, binding=0) uniform LegacyState {
    mat4 modelview;
    mat4 projection;
    mat4 texture_matrix[8];
    mat3 normal_matrix;
    float normal_scale;
    LegacyMaterial front_material;
    LegacyLight light_source[8];
    LegacyFog fog;
    LegacyLightModelProduct front_light_model;
    LegacyDepthRange depth_range;
    float viewport_height;
} legacy;

vec4 legacy_shadow2D(sampler2DShadow s, vec3 p) { return vec4(texture(s, p)); }
vec4 legacy_shadow2DProj(sampler2DShadow s, vec4 p) { return vec4(textureProj(s, p)); }
vec4 legacy_frag_coord() {
    return vec4(gl_FragCoord.x, legacy.viewport_height - gl_FragCoord.y,
                gl_FragCoord.z, gl_FragCoord.w);
}
