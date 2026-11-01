#version 120
layout(constant_id=2) const int fixed_mask=0;
layout(constant_id=3) const int fixed_modes=0;
layout(constant_id=4) const int fixed_fog_mode=0;
// Reads GL's own varyings: this stage also follows guest vertex programs
// that have no fragment shader (vulkan_guest.cpp embeds this text).
uniform vec4 fixed_env_color[8];
// GL_COMBINE units (mode 5), packed by the runtime (vulkan_guest.cpp):
// x: RGB function | alpha function << 4; y: RGB sources (4 bits each) | alpha
// sources << 12; z: RGB operands (2 bits each) | alpha operands << 6;
// w: RGB scale | alpha scale << 4.
uniform ivec4 fixed_combine[8];
// Per unit: multiplies a rectangle texture's texel coordinates into normalized ones (1 for 2D).
uniform vec4 fixed_rect[8];
uniform sampler2D fixed_texture0;
uniform sampler2D fixed_texture1;
uniform sampler2D fixed_texture2;
uniform sampler2D fixed_texture3;
uniform sampler2D fixed_texture4;
uniform sampler2D fixed_texture5;
uniform sampler2D fixed_texture6;
uniform sampler2D fixed_texture7;
vec4 fixed_tex[8];
// Source: 0 this unit's texture, 1 its constant, 2 primary colour, 3 previous, 8+n unit n's texture.
vec4 combine_source(int s,int unit,vec4 previous) {
 if(s==0)return fixed_tex[unit];
 if(s==1)return fixed_env_color[unit];
 if(s==2)return gl_Color;
 if(s==3)return previous;
 return fixed_tex[s-8];
}
vec3 operand_rgb(vec4 v,int o) {
 if(o==1)return 1.0-v.rgb;
 if(o==2)return vec3(v.a);
 if(o==3)return vec3(1.0-v.a);
 return v.rgb;
}
float operand_alpha(vec4 v,int o) {return o==3?1.0-v.a:v.a;}
vec3 function_rgb(int f,vec3 a,vec3 b,vec3 c) {
 if(f==1)return a*b;
 if(f==2)return a+b;
 if(f==3)return a+b-0.5;
 if(f==4)return a*c+b*(1.0-c);
 if(f==5)return a-b;
 if(f>=6)return vec3(4.0*dot(a-0.5,b-0.5));
 return a;
}
float function_alpha(int f,float a,float b,float c) {
 if(f==1)return a*b;
 if(f==2)return a+b;
 if(f==3)return a+b-0.5;
 if(f==4)return a*c+b*(1.0-c);
 if(f==5)return a-b;
 return a;
}
vec4 combine_unit(vec4 previous,int unit) {
 ivec4 d=fixed_combine[unit];
 vec4 s[3],t[3];
 for(int i=0;i<3;++i){s[i]=combine_source((d.y>>(4*i))&15,unit,previous);t[i]=combine_source((d.y>>(12+4*i))&15,unit,previous);}
 int frgb=d.x&15,falpha=(d.x>>4)&15;
 vec3 rgb=function_rgb(frgb,operand_rgb(s[0],d.z&3),operand_rgb(s[1],(d.z>>2)&3),operand_rgb(s[2],(d.z>>4)&3));
 float alpha=function_alpha(falpha,operand_alpha(t[0],(d.z>>6)&3),operand_alpha(t[1],(d.z>>8)&3),operand_alpha(t[2],(d.z>>10)&3));
 vec4 r=vec4(clamp(rgb*float(d.w&15),0.0,1.0),clamp(alpha*float((d.w>>4)&15),0.0,1.0));
 if(frgb==7)r.a=r.r; // GL_DOT3_RGBA
 return r;
}
vec4 combine(vec4 c,vec4 t,vec4 e,int mode,int unit) {
 if(mode==5)return combine_unit(c,unit);
 if(mode==1)return t;
 if(mode==2)return vec4(c.rgb+t.rgb,c.a*t.a);
 if(mode==3)return vec4(mix(c.rgb,t.rgb,t.a),c.a);
 if(mode==4)return vec4(mix(c.rgb,e.rgb,t.rgb),c.a*t.a);
 return c*t;
}
void main() {
 vec4 c=gl_Color;
 for(int i=0;i<8;++i)fixed_tex[i]=vec4(0.0);
 if((fixed_mask & 1)!=0)fixed_tex[0]=texture2DProj(fixed_texture0,vec4(gl_TexCoord[0].xy*fixed_rect[0].xy,gl_TexCoord[0].zw));
 if((fixed_mask & 2)!=0)fixed_tex[1]=texture2DProj(fixed_texture1,vec4(gl_TexCoord[1].xy*fixed_rect[1].xy,gl_TexCoord[1].zw));
 if((fixed_mask & 4)!=0)fixed_tex[2]=texture2DProj(fixed_texture2,vec4(gl_TexCoord[2].xy*fixed_rect[2].xy,gl_TexCoord[2].zw));
 if((fixed_mask & 8)!=0)fixed_tex[3]=texture2DProj(fixed_texture3,vec4(gl_TexCoord[3].xy*fixed_rect[3].xy,gl_TexCoord[3].zw));
 if((fixed_mask & 16)!=0)fixed_tex[4]=texture2DProj(fixed_texture4,vec4(gl_TexCoord[4].xy*fixed_rect[4].xy,gl_TexCoord[4].zw));
 if((fixed_mask & 32)!=0)fixed_tex[5]=texture2DProj(fixed_texture5,vec4(gl_TexCoord[5].xy*fixed_rect[5].xy,gl_TexCoord[5].zw));
 if((fixed_mask & 64)!=0)fixed_tex[6]=texture2DProj(fixed_texture6,vec4(gl_TexCoord[6].xy*fixed_rect[6].xy,gl_TexCoord[6].zw));
 if((fixed_mask & 128)!=0)fixed_tex[7]=texture2DProj(fixed_texture7,vec4(gl_TexCoord[7].xy*fixed_rect[7].xy,gl_TexCoord[7].zw));
 if((fixed_mask & 1)!=0)c=combine(c,fixed_tex[0],fixed_env_color[0],(fixed_modes>>0)&7,0);
 if((fixed_mask & 2)!=0)c=combine(c,fixed_tex[1],fixed_env_color[1],(fixed_modes>>3)&7,1);
 if((fixed_mask & 4)!=0)c=combine(c,fixed_tex[2],fixed_env_color[2],(fixed_modes>>6)&7,2);
 if((fixed_mask & 8)!=0)c=combine(c,fixed_tex[3],fixed_env_color[3],(fixed_modes>>9)&7,3);
 if((fixed_mask & 16)!=0)c=combine(c,fixed_tex[4],fixed_env_color[4],(fixed_modes>>12)&7,4);
 if((fixed_mask & 32)!=0)c=combine(c,fixed_tex[5],fixed_env_color[5],(fixed_modes>>15)&7,5);
 if((fixed_mask & 64)!=0)c=combine(c,fixed_tex[6],fixed_env_color[6],(fixed_modes>>18)&7,6);
 if((fixed_mask & 128)!=0)c=combine(c,fixed_tex[7],fixed_env_color[7],(fixed_modes>>21)&7,7);
 if(fixed_fog_mode!=0) {
   float d=gl_FogFragCoord,f;
   if(fixed_fog_mode==1)f=(gl_Fog.end-d)*gl_Fog.scale;
   else if(fixed_fog_mode==2)f=exp(-gl_Fog.density*d);
   else f=exp(-gl_Fog.density*gl_Fog.density*d*d);
   c.rgb=mix(gl_Fog.color.rgb,c.rgb,clamp(f,0.0,1.0));
 }
 gl_FragColor=c;
}
