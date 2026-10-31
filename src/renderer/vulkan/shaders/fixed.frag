#version 120
layout(constant_id=2) const int fixed_mask=0;
layout(constant_id=3) const int fixed_modes=0;
layout(constant_id=4) const int fixed_fog_mode=0;
varying vec4 fixed_uv[8];
varying float fixed_fog_distance;
uniform vec4 fixed_env_color[8];
uniform sampler2D fixed_texture0;
uniform sampler2D fixed_texture1;
uniform sampler2D fixed_texture2;
uniform sampler2D fixed_texture3;
uniform sampler2D fixed_texture4;
uniform sampler2D fixed_texture5;
uniform sampler2D fixed_texture6;
uniform sampler2D fixed_texture7;
vec4 combine(vec4 c,vec4 t,vec4 e,int mode) {
 if(mode==1)return t;
 if(mode==2)return vec4(c.rgb+t.rgb,c.a*t.a);
 if(mode==3)return vec4(mix(c.rgb,t.rgb,t.a),c.a);
 if(mode==4)return vec4(mix(c.rgb,e.rgb,t.rgb),c.a*t.a);
 return c*t;
}
void main() {
 vec4 c=gl_Color;
 if((fixed_mask & 1)!=0)c=combine(c,texture2DProj(fixed_texture0,fixed_uv[0]),fixed_env_color[0],(fixed_modes>>0)&7);
 if((fixed_mask & 2)!=0)c=combine(c,texture2DProj(fixed_texture1,fixed_uv[1]),fixed_env_color[1],(fixed_modes>>3)&7);
 if((fixed_mask & 4)!=0)c=combine(c,texture2DProj(fixed_texture2,fixed_uv[2]),fixed_env_color[2],(fixed_modes>>6)&7);
 if((fixed_mask & 8)!=0)c=combine(c,texture2DProj(fixed_texture3,fixed_uv[3]),fixed_env_color[3],(fixed_modes>>9)&7);
 if((fixed_mask & 16)!=0)c=combine(c,texture2DProj(fixed_texture4,fixed_uv[4]),fixed_env_color[4],(fixed_modes>>12)&7);
 if((fixed_mask & 32)!=0)c=combine(c,texture2DProj(fixed_texture5,fixed_uv[5]),fixed_env_color[5],(fixed_modes>>15)&7);
 if((fixed_mask & 64)!=0)c=combine(c,texture2DProj(fixed_texture6,fixed_uv[6]),fixed_env_color[6],(fixed_modes>>18)&7);
 if((fixed_mask & 128)!=0)c=combine(c,texture2DProj(fixed_texture7,fixed_uv[7]),fixed_env_color[7],(fixed_modes>>21)&7);
 if(fixed_fog_mode!=0) {
   float d=fixed_fog_distance,f;
   if(fixed_fog_mode==1)f=(gl_Fog.end-d)*gl_Fog.scale;
   else if(fixed_fog_mode==2)f=exp(-gl_Fog.density*d);
   else f=exp(-gl_Fog.density*gl_Fog.density*d*d);
   c.rgb=mix(gl_Fog.color.rgb,c.rgb,clamp(f,0.0,1.0));
 }
 gl_FragColor=c;
}
