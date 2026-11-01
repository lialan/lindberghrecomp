#include "xr_overlay_geometry.h"
#include <math.h>
#include <string.h>
#include "xr_gun_model.h"
#include "xr_gun_tp9.h"
#include "xr_gun_gs.h"

/* a debug switch, looked up once */
#define ENV_ON(name) ({ static int _on = -1; if (_on < 0) _on = getenv(name) != NULL; _on; })

/* the sights' boxes, y measured up from the rail's top */
typedef struct { float c[3], h[3], rgb[3]; } Box;
static const Box RED_DOT[] = {          /* a slim reflex sight, like a Trijicon RMR */
    { { 0, 0.003f, 0.028f }, { 0.0125f, 0.003f, 0.020f }, { 0.12f, 0.12f, 0.13f } },          /* base */
    { { 0, 0.008f, 0.040f }, { 0.0115f, 0.004f, 0.008f }, { 0.15f, 0.15f, 0.16f } },          /* emitter housing */
    { { -0.0110f, 0.0155f, 0.018f }, { 0.0015f, 0.0095f, 0.004f }, { 0.15f, 0.15f, 0.16f } }, /* hood: sides */
    { { 0.0110f, 0.0155f, 0.018f }, { 0.0015f, 0.0095f, 0.004f }, { 0.15f, 0.15f, 0.16f } },
    { { 0, 0.0255f, 0.018f }, { 0.0125f, 0.0015f, 0.004f }, { 0.15f, 0.15f, 0.16f } },        /* hood: top */
};
static const Box HOLO[] = {             /* a window sight, like an EOTech */
    { { 0, 0.008f, 0.032f }, { 0.017f, 0.008f, 0.048f }, { 0.12f, 0.12f, 0.13f } },           /* body */
    { { -0.019f, 0.032f, 0.018f }, { 0.003f, 0.020f, 0.022f }, { 0.15f, 0.15f, 0.16f } },     /* hood: sides */
    { { 0.019f, 0.032f, 0.018f }, { 0.003f, 0.020f, 0.022f }, { 0.15f, 0.15f, 0.16f } },
    { { 0, 0.050f, 0.018f }, { 0.022f, 0.003f, 0.022f }, { 0.15f, 0.15f, 0.16f } },          /* hood: top */
    { { 0.008f, 0.018f, 0.064f }, { 0.007f, 0.004f, 0.010f }, { 0.08f, 0.08f, 0.09f } },      /* buttons */
};
/* each sight's glass: centre z, half width, half height, and the sight line's height above the rail */
static const float GLASS[3][4] = { { 0, 0, 0, 0 }, { 0.018f, 0.0095f, 0.0085f, 0.0145f },
                                   { 0.004f, 0.016f, 0.017f, 0.030f } };

#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define MAX_VERTS MAX2(MAX2(GUN_VERTS, TP9_VERTS), GS_VERTS)
static void add_boxes(float *mesh,float rail_y,int *n,const Box *bx,int count)
{
    static const int face[6][4] = { { 1, 3, 7, 5 }, { 0, 4, 6, 2 }, { 2, 6, 7, 3 },
                                    { 0, 1, 5, 4 }, { 4, 5, 7, 6 }, { 0, 2, 3, 1 } };
    static const float nrm[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int p = 0; p < count; p++) {
        float v[8][3];
        for (int k = 0; k < 8; k++)
            for (int a = 0; a < 3; a++)
                v[k][a] = bx[p].c[a] + (a == 1 ? rail_y : 0) + ((k >> a) & 1 ? bx[p].h[a] : -bx[p].h[a]);
        for (int f = 0; f < 6; f++) {
            static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
            for (int t = 0; t < 6; t++) {
                float *o = &mesh[(*n)++ * 9];
                memcpy(o, v[face[f][tri[t]]], 12);
                memcpy(o + 3, nrm[f], 12);
                memcpy(o + 6, bx[p].rgb, 12);
            }
        }
    }
}

unsigned xo_gun_capacity(void) {return MAX_VERTS+8*36;}
int xo_gun_glass(int sight,float glass[4])
{
    if(sight<XO_SIGHT_NONE||sight>XO_SIGHT_HOLO)return 0;
    memcpy(glass,GLASS[sight],16);return 1;
}
int xo_gun_build(int model,int sight,float *mesh,unsigned capacity,float *rail)
{
    if(!mesh||!rail||sight<XO_SIGHT_NONE||sight>XO_SIGHT_HOLO)return 0;
    const int nbase=model==XO_GUN_TP9?TP9_VERTS:model==XO_GUN_GHOST_SQUAD?GS_VERTS:GUN_VERTS;
    if(capacity<(unsigned)(nbase+(sight==XO_SIGHT_NONE?0:180)))return 0;
    int n;float rail_y;
    if(model==XO_GUN_TP9){memcpy(mesh,TP9_MESH,sizeof TP9_MESH);n=TP9_VERTS;rail_y=TP9_RAIL_Y;}
    else if(model==XO_GUN_GHOST_SQUAD){memcpy(mesh,GS_MESH,sizeof GS_MESH);n=GS_VERTS;rail_y=GS_RAIL_Y;}
    else{memcpy(mesh,GUN_MESH,sizeof GUN_MESH);n=GUN_VERTS;rail_y=GUN_RAIL_Y;}
    if(sight==XO_SIGHT_RED_DOT)add_boxes(mesh,rail_y,&n,RED_DOT,(int)(sizeof RED_DOT/sizeof *RED_DOT));
    if(sight==XO_SIGHT_HOLO)add_boxes(mesh,rail_y,&n,HOLO,(int)(sizeof HOLO/sizeof *HOLO));
    *rail=rail_y;return n;
}
void xo_pose_matrix(const float q[4], const float p[3], float *m)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);     m[3] = 0;
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);     m[7] = 0;
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y); m[11] = 0;
    m[12] = p[0]; m[13] = p[1]; m[14] = p[2]; m[15] = 1;
}

int xo_beam_vertices(float q[6][5],const float eye[3],const float a[3],const float b[3],float w)
{
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e[3] = { eye[0] - a[0], eye[1] - a[1], eye[2] - a[2] };
    float s[3] = { d[1] * e[2] - d[2] * e[1], d[2] * e[0] - d[0] * e[2], d[0] * e[1] - d[1] * e[0] };
    float l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (l < 1e-6f) return 0;
    for (int i = 0; i < 3; i++) s[i] *= w / l;
    const float vertices[6][5] = {
        { a[0] - s[0], a[1] - s[1], a[2] - s[2], -1, 0 }, { a[0] + s[0], a[1] + s[1], a[2] + s[2], 1, 0 },
        { b[0] + s[0], b[1] + s[1], b[2] + s[2], 1, 0 }, { a[0] - s[0], a[1] - s[1], a[2] - s[2], -1, 0 },
        { b[0] + s[0], b[1] + s[1], b[2] + s[2], 1, 0 }, { b[0] - s[0], b[1] - s[1], b[2] - s[2], -1, 0 },
    };
    memcpy(q,vertices,sizeof vertices);return 6;
}
int xo_spot_vertices(float q[6][5],const float eye[3],const float p[3],float rad)
{
    float f[3] = { eye[0] - p[0], eye[1] - p[1], eye[2] - p[2] };
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (l < 1e-6f) return 0;
    for (int i = 0; i < 3; i++) f[i] /= l;
    float up[3] = { 0, 1, 0 }, x[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
    float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (xl < 1e-6f) { x[0] = 1; x[1] = x[2] = 0; xl = 1; }
    for (int i = 0; i < 3; i++) x[i] *= rad / xl;
    float y[3] = { f[1] * x[2] - f[2] * x[1], f[2] * x[0] - f[0] * x[2], f[0] * x[1] - f[1] * x[0] };
    static const float c[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < 3; i++) q[k][i] = p[i] + c[k][0] * x[i] + c[k][1] * y[i];
        q[k][3] = c[k][0]; q[k][4] = c[k][1];
    }
    return 6;
}
int xo_glass_vertices(float q[6][5],const float model[16],int sight,float rail_y)
{
    float gz[4];if(!xo_gun_glass(sight,gz))return 0;
    static const float c[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int k = 0; k < 6; k++) {
        float lp[3] = { c[k][0] * gz[1], rail_y + gz[3] + c[k][1] * gz[2], gz[0] };
        for (int i = 0; i < 3; i++)
            q[k][i] = model[i] * lp[0] + model[4 + i] * lp[1] + model[8 + i] * lp[2] + model[12 + i];
        q[k][3] = 0; q[k][4] = 0;
    }
    return 6;
}
