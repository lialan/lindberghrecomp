#version 120
layout(constant_id=2) const int fixed_mask=0;
// GL_LIGHTING (vulkan_guest.cpp): x enabled lights (0 lighting off); y colour
// material 0 none, 1 ambient, 2 diffuse, 3 ambient and diffuse, 4 specular,
// 5 emission; z local viewer | two-sided << 1; w GL_NORMALIZE/GL_RESCALE_NORMAL.
// Two-sided lighting lights back faces with the flipped normal and the front
// material (the legacy block holds one), GL's result for GL_FRONT_AND_BACK.
uniform ivec4 fixed_lighting;
uniform vec4 fixed_light_model_ambient;
// GL's per-vertex lighting of the front face (OpenGL 2.1 section 2.14.1).
vec4 lit(vec3 V,vec3 N) {
 int cm=fixed_lighting.y;
 vec4 Ecm=cm==5?gl_Color:gl_FrontMaterial.emission;
 vec4 Acm=(cm==1||cm==3)?gl_Color:gl_FrontMaterial.ambient;
 vec4 Dcm=(cm==2||cm==3)?gl_Color:gl_FrontMaterial.diffuse;
 vec4 Scm=cm==4?gl_Color:gl_FrontMaterial.specular;
 vec3 c=Ecm.rgb+Acm.rgb*fixed_light_model_ambient.rgb;
 for(int i=0;i<8;++i) {
  if(((fixed_lighting.x>>i)&1)==0)continue;
  vec4 P=gl_LightSource[i].position;
  vec3 L=P.w==0.0?P.xyz:P.xyz-V;
  float d=length(L),k=1.0;L=d>0.0?L/d:L;
  if(P.w!=0.0)k=1.0/(gl_LightSource[i].constantAttenuation+gl_LightSource[i].linearAttenuation*d+gl_LightSource[i].quadraticAttenuation*d*d);
  if(gl_LightSource[i].spotCutoff!=180.0) {
   float s=dot(-L,normalize(gl_LightSource[i].spotDirection));
   k*=s<gl_LightSource[i].spotCosCutoff?0.0:pow(max(s,0.0),gl_LightSource[i].spotExponent);
  }
  float nl=max(dot(N,L),0.0);
  vec3 term=Acm.rgb*gl_LightSource[i].ambient.rgb+nl*Dcm.rgb*gl_LightSource[i].diffuse.rgb;
  if(nl>0.0) {
   vec3 h=normalize(L+((fixed_lighting.z&1)!=0?normalize(-V):vec3(0,0,1)));
   float nh=max(dot(N,h),0.0);
   term+=(gl_FrontMaterial.shininess>0.0?pow(nh,gl_FrontMaterial.shininess):1.0)*Scm.rgb*gl_LightSource[i].specular.rgb;
  }
  c+=k*term;
 }
 return vec4(clamp(c,0.0,1.0),clamp(Dcm.a,0.0,1.0));
}
void main() {
 gl_Position=ftransform(); gl_FrontColor=gl_Color; gl_BackColor=gl_Color;
 vec4 eye=gl_ModelViewMatrix*gl_Vertex;
 gl_FogFragCoord=abs(eye.z);
 if(fixed_lighting.x!=0) {
  vec3 N=gl_NormalMatrix*gl_Normal;
  if(fixed_lighting.w!=0)N=normalize(N);
  gl_FrontColor=lit(eye.xyz/eye.w,N);
  gl_BackColor=(fixed_lighting.z&2)!=0?lit(eye.xyz/eye.w,-N):gl_FrontColor;
 }
 gl_TexCoord[0]=(fixed_mask & 1)!=0 ? gl_TextureMatrix[0]*gl_MultiTexCoord0 : vec4(0,0,0,1);
 gl_TexCoord[1]=(fixed_mask & 2)!=0 ? gl_TextureMatrix[1]*gl_MultiTexCoord1 : vec4(0,0,0,1);
 gl_TexCoord[2]=(fixed_mask & 4)!=0 ? gl_TextureMatrix[2]*gl_MultiTexCoord2 : vec4(0,0,0,1);
 gl_TexCoord[3]=(fixed_mask & 8)!=0 ? gl_TextureMatrix[3]*gl_MultiTexCoord3 : vec4(0,0,0,1);
 gl_TexCoord[4]=(fixed_mask & 16)!=0 ? gl_TextureMatrix[4]*gl_MultiTexCoord4 : vec4(0,0,0,1);
 gl_TexCoord[5]=(fixed_mask & 32)!=0 ? gl_TextureMatrix[5]*gl_MultiTexCoord5 : vec4(0,0,0,1);
 gl_TexCoord[6]=(fixed_mask & 64)!=0 ? gl_TextureMatrix[6]*gl_MultiTexCoord6 : vec4(0,0,0,1);
 gl_TexCoord[7]=(fixed_mask & 128)!=0 ? gl_TextureMatrix[7]*gl_MultiTexCoord7 : vec4(0,0,0,1);
}
