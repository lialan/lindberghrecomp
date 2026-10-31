#version 120
layout(constant_id=2) const int fixed_mask=0;
varying vec4 fixed_uv[8];
varying float fixed_fog_distance;
void main() {
 gl_Position=ftransform(); gl_FrontColor=gl_Color; gl_BackColor=gl_Color;
 fixed_fog_distance=abs((gl_ModelViewMatrix*gl_Vertex).z);
 fixed_uv[0]=(fixed_mask & 1)!=0 ? gl_TextureMatrix[0]*gl_MultiTexCoord0 : vec4(0,0,0,1);
 fixed_uv[1]=(fixed_mask & 2)!=0 ? gl_TextureMatrix[1]*gl_MultiTexCoord1 : vec4(0,0,0,1);
 fixed_uv[2]=(fixed_mask & 4)!=0 ? gl_TextureMatrix[2]*gl_MultiTexCoord2 : vec4(0,0,0,1);
 fixed_uv[3]=(fixed_mask & 8)!=0 ? gl_TextureMatrix[3]*gl_MultiTexCoord3 : vec4(0,0,0,1);
 fixed_uv[4]=(fixed_mask & 16)!=0 ? gl_TextureMatrix[4]*gl_MultiTexCoord4 : vec4(0,0,0,1);
 fixed_uv[5]=(fixed_mask & 32)!=0 ? gl_TextureMatrix[5]*gl_MultiTexCoord5 : vec4(0,0,0,1);
 fixed_uv[6]=(fixed_mask & 64)!=0 ? gl_TextureMatrix[6]*gl_MultiTexCoord6 : vec4(0,0,0,1);
 fixed_uv[7]=(fixed_mask & 128)!=0 ? gl_TextureMatrix[7]*gl_MultiTexCoord7 : vec4(0,0,0,1);
}
