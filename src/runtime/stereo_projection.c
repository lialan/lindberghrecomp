#include "stereo_projection.h"
#include <math.h>
#include <string.h>
static void identity(float *m) {memset(m,0,64);m[0]=m[5]=m[10]=m[15]=1;}
static void multiply(float *out,const float *a,const float *b)
{
    float r[16];
    for(int c=0;c<4;++c) for(int k=0;k<4;++k)
        r[c*4+k]=a[k]*b[c*4]+a[4+k]*b[c*4+1]+a[8+k]*b[c*4+2]+a[12+k]*b[c*4+3];
    memcpy(out,r,64);
}
static void frustum(float *m,double l,double r,double b,double t,double n,double f)
{
    memset(m,0,64);
    m[0]=(float)(2*n/(r-l));m[5]=(float)(2*n/(t-b));
    m[8]=(float)((r+l)/(r-l));m[9]=(float)((t+b)/(t-b));
    m[10]=(float)(-(f+n)/(f-n));m[11]=-1;m[14]=(float)(-2*f*n/(f-n));
}
void lg_stereo_eye_projection(const float *p,int kind,const LgStereoProjection *eye,float *out)
{
    if(kind==LG_STEREO_PASS) {memcpy(out,p,64);return;}
    if(kind==LG_STEREO_FILL) {
        memcpy(out,p,64);
        for(int col=0;col<4;++col) out[col*4+2]=-.999f*out[col*4+3];
        return;
    }
    const float *t=eye->tangents;
    float e[16],ev[16];
    if(lg_stereo_is_perspective(p) && kind==LG_STEREO_WORLD) {
        const float n=p[14]/(p[10]-1),f=p[14]/(p[10]+1);
        if(isfinite(f)) frustum(e,-t[0]*n,t[1]*n,-t[2]*n,t[3]*n,n,f);
        else {
            /* A truly infinite far plane remains usable when float rounding
             * makes p[10] exactly -1. The finite GL path stays unchanged. */
            frustum(e,-t[0]*n,t[1]*n,-t[2]*n,t[3]*n,n,2*n);
            e[10]=p[10];e[14]=p[14];
        }
        multiply(out,e,eye->view);return;
    }
    if(kind==LG_STEREO_WORLD) {memcpy(out,p,64);return;}
    const float d=eye->screen_distance;
    float screen[16];identity(screen);
    screen[0]=d/eye->screen_lens[0];screen[5]=d/eye->screen_lens[1];
    screen[10]=-.02f*d;screen[14]=-d;
    const float zn=.25f*d,zf=4*d;
    frustum(e,-t[0]*zn,t[1]*zn,-t[2]*zn,t[3]*zn,zn,zf);
    multiply(ev,e,eye->hud_view);multiply(ev,ev,screen);multiply(out,ev,p);
}
int lg_stereo_clip_reprojection(const float *p,const float *e,float *out)
{
    double a[4][8];
    for(int r=0;r<4;++r) for(int c=0;c<8;++c) a[r][c]=c<4?p[c*4+r]:(c-4==r);
    for(int c=0;c<4;++c) {
        int pivot=c;
        for(int r=c+1;r<4;++r) if(fabs(a[r][c])>fabs(a[pivot][c])) pivot=r;
        if(!isfinite(a[pivot][c]) || fabs(a[pivot][c])<1e-30) return 0;
        for(int k=0;k<8;++k) {double t=a[c][k];a[c][k]=a[pivot][k];a[pivot][k]=t;}
        const double divisor=a[c][c];for(int k=0;k<8;++k) a[c][k]/=divisor;
        for(int r=0;r<4;++r) if(r!=c) {
            const double f=a[r][c];for(int k=0;k<8;++k) a[r][k]-=f*a[c][k];
        }
    }
    float result[16];
    for(int c=0;c<4;++c) for(int r=0;r<4;++r) {
        double v=0;for(int k=0;k<4;++k) v+=(double)e[k*4+r]*a[k][c+4];
        if(!isfinite(v)) return 0;
        result[c*4+r]=(float)v;
        if(!isfinite(result[c*4+r])) return 0;
    }
    memcpy(out,result,64);return 1;
}
