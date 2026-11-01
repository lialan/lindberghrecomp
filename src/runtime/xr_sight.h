#pragma once
/* Shared GL/native crosshair recognition. Learn a small textured sprite only
 * after it follows a moving primary gun across 30 distinct game frames.
 * Never hide an unrecognized HUD sprite. Coordinates are guest NDC. */
typedef struct {
    unsigned primary,secondary,candidate,candidate2,hits,hits2,frame,frame2;
    float from[2];
} LgXrSight;
static inline int lg_xr_sight_draw(LgXrSight *s,unsigned frame,unsigned texture,int textured,
                                  const float box[4],const float gun[2],const float gun2[2],int show)
{
    if(gun[0]<-2||!textured||!texture||box[2]-box[0]>.4f||box[3]-box[1]>.5f)return 0;
    const float cx=(box[0]+box[2])*.5f,cy=(box[1]+box[3])*.5f;
    float x=cx-gun[0],y=cy-gun[1];
    if(x*x*100+y*y*56>.02f) {
        x=cx-gun2[0];y=cy-gun2[1];
        if(gun2[0]<-2||x*x*100+y*y*56>.02f)return 0;
        if(texture!=s->primary&&!s->secondary&&s->primary) {
            if(texture!=s->candidate2){s->candidate2=texture;s->hits2=0;}
            if(frame!=s->frame2){s->frame2=frame;++s->hits2;}
            if(s->hits2>=30)s->secondary=s->candidate2;
        }
        return !show&&s->primary&&(texture==s->primary||texture==s->secondary);
    }
    if(!s->primary) {
        if(texture!=s->candidate){s->candidate=texture;s->hits=0;s->from[0]=gun[0];s->from[1]=gun[1];}
        if(frame!=s->frame){s->frame=frame;++s->hits;}
        x=gun[0]-s->from[0];y=gun[1]-s->from[1];
        if(s->hits>=30&&x*x+y*y>.01f)s->primary=s->candidate;
        return 0;
    }
    return !show&&(texture==s->primary||(s->secondary&&texture==s->secondary));
}
