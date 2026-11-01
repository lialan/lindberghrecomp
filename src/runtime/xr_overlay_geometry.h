#pragma once
#include "xr_overlay.h"
#ifdef __cplusplus
extern "C" {
#endif
unsigned xo_gun_capacity(void); /* vertices, each position/normal/RGB: 9 floats */
int xo_gun_build(int model,int sight,float *mesh,unsigned capacity,float *rail_y);
int xo_gun_glass(int sight,float glass[4]);
void xo_pose_matrix(const float quaternion[4],const float position[3],float matrix[16]);
int xo_beam_vertices(float vertices[6][5],const float eye[3],const float a[3],const float b[3],float width);
int xo_spot_vertices(float vertices[6][5],const float eye[3],const float position[3],float radius);
int xo_glass_vertices(float vertices[6][5],const float model[16],int sight,float rail_y);
#ifdef __cplusplus
}
#endif
