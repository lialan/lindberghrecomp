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
void lg_stereo_target_scale(const LgStereoTargetSize *s,int w,int h,int rectangle,float *x,float *y)
{
    float kx=(float)s->eye_w/s->game_w,ky=(float)s->eye_h/s->game_h;
    if(w>0 && h>0 && w==s->world_w && h==s->world_h) {kx=(float)s->eye_w/w;ky=(float)s->eye_h/h;}
    else if(!rectangle && !(w==s->game_w && h==s->game_h) && w>0 && h>0) {
        float k=fminf(fminf(kx,ky),2.f);
        k=fminf(k,1024.f/(w>h?w:h));kx=ky=k<1?1:k;
    }
    *x=kx;*y=ky;
}
int lg_stereo_classify_draw(const LgStereoDrawFacts *d)
{
    int hud=d->perspective ? d->hud_target || d->menu || d->zoom
                          : !(d->texture_view || d->picture_target) || !d->covers_viewport;
    if(d->offscreen || d->flat) return LG_STEREO_PASS;
    if(hud && !d->perspective && d->multiview && d->fills_screen) return LG_STEREO_FILL;
    return hud ? LG_STEREO_HUD : LG_STEREO_WORLD;
}
void lg_stereo_rotate(const float *q,const float *v,float *out)
{
    const float x=q[0],y=q[1],z=q[2],w=q[3];
    const float tx=2*(y*v[2]-z*v[1]),ty=2*(z*v[0]-x*v[2]),tz=2*(x*v[1]-y*v[0]);
    const float r[]={v[0]+w*tx+(y*tz-z*ty),v[1]+w*ty+(z*tx-x*tz),v[2]+w*tz+(x*ty-y*tx)};
    memcpy(out,r,sizeof r);
}
void lg_stereo_pose_view(const float *q,const float *p,float scale,float *out)
{
    const float inverse[]={-q[0],-q[1],-q[2],q[3]};
    const float ex[]={1,0,0},ey[]={0,1,0},ez[]={0,0,1};
    const float translation[]={-p[0]*scale,-p[1]*scale,-p[2]*scale};
    float c0[3],c1[3],c2[3],t[3];
    lg_stereo_rotate(inverse,ex,c0);lg_stereo_rotate(inverse,ey,c1);lg_stereo_rotate(inverse,ez,c2);
    lg_stereo_rotate(inverse,translation,t);
    identity(out);
    for(int i=0;i<3;++i) {out[i]=c0[i];out[4+i]=c1[i];out[8+i]=c2[i];out[12+i]=t[i];}
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
