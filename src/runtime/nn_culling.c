#include "lindbergh_rt.h"
#include "nn_culling.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void (*original)(CPU *);
static uint32_t projection_x,projection_y;
static float widen;
static int log_calls;
extern void (*dispatch_lifted(uint32_t va))(CPU *);
static void side_planes(CPU *cpu)
{
    const uint32_t x=rd32(projection_x),y=rd32(projection_y);float fx,fy;
    memcpy(&fx,&x,4);memcpy(&fy,&y,4);const float wx=fx/widen,wy=fy/widen;uint32_t bx,by;
    memcpy(&bx,&wx,4);memcpy(&by,&wy,4);wr32(projection_x,bx);wr32(projection_y,by);
    push32(cpu,0xDEADBEEFu);original(cpu);
    wr32(projection_x,x);wr32(projection_y,y);
    if(log_calls>0){--log_calls;fprintf(stderr,"[nn-cull] side-plane input %.6g %.6g -> %.6g %.6g; render projection restored\n",fx,fy,wx,wy);}
}
float nn_culling_default_widen(void)
{
#if defined(LINDBERGH_OPENXR) || defined(LINDBERGH_VULKAN_OFFSCREEN)
    return 4;
#else
    const char *stereo=getenv("LINDBERGH_STEREO"),*fake=getenv("LINDBERGH_VULKAN_FAKE_YAW");
    return (stereo&&*stereo&&*stereo!='0')||(fake&&*fake)?4:1;
#endif
}
int nn_culling_install(const char *label,uint32_t builder,uint32_t x,uint32_t y,float default_widen)
{
    if(original||!label||!*label)return 0;
    const char *setting=getenv("LINDBERGH_CULL_WIDEN");float factor=default_widen;
    if(setting&&*setting){char *end;factor=strtof(setting,&end);if(*end)return 0;}
    if(!isfinite(factor)||factor<=1||!builder||!x||!y||x==y)return 0;
    void (*function)(CPU *)=dispatch_lifted(builder);if(!function)return 0;
    if(!guest_override_va(label,builder,side_planes))return 0;
    original=function;projection_x=x;projection_y=y;widen=factor;
    const char *log=getenv("LINDBERGH_CULL_LOG");log_calls=log&&*log=='1'?3:0;
    fprintf(stderr,"[nn-cull] %s at %#x: side planes %.1fx wider; projection %#x/%#x unchanged\n",label,builder,factor,x,y);
    return 1;
}
