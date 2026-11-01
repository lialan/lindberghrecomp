#include "xr_actions.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HANDLE(type,value) ((type)(uintptr_t)(value))
static int created,spaces,dead_actions,dead_spaces,dead_sets,fail_action,queries,active=1;
static int left_x,right_a;static XrResult sync_result=XR_SUCCESS;
static XrSpaceLocationFlags valid=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
static XrActionType types[LG_XR_ACTION_COUNT];
XrResult xrStringToPath(XrInstance i,const char *s,XrPath *out) {
    assert(i);if(!strcmp(s,"/user/hand/left"))*out=1;else if(!strcmp(s,"/user/hand/right"))*out=2;else *out=3;
    left_x|=!strcmp(s,"/user/hand/left/input/x/click");right_a|=!strcmp(s,"/user/hand/right/input/a/click");return XR_SUCCESS;
}
XrResult xrCreateActionSet(XrInstance i,const XrActionSetCreateInfo *c,XrActionSet *out) {
    assert(i&&!strcmp(c->actionSetName,"gun"));*out=HANDLE(XrActionSet,32);return XR_SUCCESS;
}
XrResult xrCreateAction(XrActionSet s,const XrActionCreateInfo *c,XrAction *out) {
    assert(s==HANDLE(XrActionSet,32)&&c->countSubactionPaths==2&&c->subactionPaths[0]==1&&c->subactionPaths[1]==2);
    if(fail_action&&created+1==fail_action)return XR_ERROR_RUNTIME_FAILURE;
    types[created]=c->actionType;*out=HANDLE(XrAction,10+created++);return XR_SUCCESS;
}
XrResult xrSuggestInteractionProfileBindings(XrInstance i,const XrInteractionProfileSuggestedBinding *b) {
    assert(i&&created==8&&b->countSuggestedBindings==15&&left_x&&right_a);return XR_SUCCESS;
}
XrResult xrCreateActionSpace(XrSession s,const XrActionSpaceCreateInfo *c,XrSpace *out) {
    assert(s&&c->action==HANDLE(XrAction,10)&&c->subactionPath==(XrPath)(spaces+1)&&c->poseInActionSpace.orientation.w==1);
    *out=HANDLE(XrSpace,100+spaces++);return XR_SUCCESS;
}
XrResult xrAttachSessionActionSets(XrSession s,const XrSessionActionSetsAttachInfo *a) {
    assert(s&&spaces==2&&a->countActionSets==1&&a->actionSets[0]==HANDLE(XrActionSet,32));return XR_SUCCESS;
}
XrResult xrDestroySpace(XrSpace s) {assert(s);++dead_spaces;return XR_SUCCESS;}
XrResult xrDestroyAction(XrAction a) {assert(a);++dead_actions;return XR_SUCCESS;}
XrResult xrDestroyActionSet(XrActionSet s) {assert(s);++dead_sets;return XR_SUCCESS;}
XrResult xrSyncActions(XrSession s,const XrActionsSyncInfo *a) {assert(s&&a->countActiveActionSets==1);return sync_result;}
XrResult xrGetActionStateFloat(XrSession s,const XrActionStateGetInfo *g,XrActionStateFloat *out) {
    assert(s&&types[(uintptr_t)g->action-10]==XR_ACTION_TYPE_FLOAT_INPUT);++queries;
    out->isActive=active;out->currentState=g->action==HANDLE(XrAction,11)?.8f:.7f;return XR_SUCCESS;
}
XrResult xrGetActionStateBoolean(XrSession s,const XrActionStateGetInfo *g,XrActionStateBoolean *out) {
    assert(s&&types[(uintptr_t)g->action-10]==XR_ACTION_TYPE_BOOLEAN_INPUT);++queries;
    out->isActive=active;out->currentState=g->subactionPath==1;return XR_SUCCESS;
}
XrResult xrGetActionStatePose(XrSession s,const XrActionStateGetInfo *g,XrActionStatePose *out) {
    assert(s&&types[(uintptr_t)g->action-10]==XR_ACTION_TYPE_POSE_INPUT);++queries;out->isActive=active;return XR_SUCCESS;
}
XrResult xrLocateSpace(XrSpace s,XrSpace base,XrTime time,XrSpaceLocation *out) {
    assert(s&&base&&time==123);out->locationFlags=valid;out->pose.orientation.w=1;out->pose.position.x=s==HANDLE(XrSpace,100)?-.16f:.16f;return XR_SUCCESS;
}
XrResult xrApplyHapticFeedback(XrSession s,const XrHapticActionInfo *a,const XrHapticBaseHeader *h) {
    const XrHapticVibration *v=(const XrHapticVibration *)h;
    assert(s&&a->action==HANDLE(XrAction,17)&&a->subactionPath==2&&v->amplitude==.7f&&v->duration==35000000);return XR_SUCCESS;
}
int main(void) {
    LgXrActions a;assert(lg_xr_actions_create(&a,HANDLE(XrInstance,1),HANDLE(XrSession,2))==XR_SUCCESS);
    LgXrActionSample sample;assert(lg_xr_actions_sample(&a,HANDLE(XrSpace,9),123,&sample)==XR_SUCCESS);
    assert(sample.valid[0]&&sample.valid[1]&&sample.pose[0].position.x!=sample.pose[1].position.x);
    assert(sample.trigger[0]==.8f&&sample.grip[1]==.7f&&sample.lower[0]&&!sample.lower[1]&&sample.menu);
    active=0;assert(lg_xr_actions_sample(&a,HANDLE(XrSpace,9),123,&sample)==XR_SUCCESS);
    assert(!sample.valid[0]&&!sample.valid[1]&&!sample.trigger[0]&&!sample.lower[0]);
    active=1;valid=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    assert(lg_xr_actions_sample(&a,HANDLE(XrSpace,9),123,&sample)==XR_SUCCESS&&!sample.valid[0]&&!sample.valid[1]);
    const int old_queries=queries;sync_result=XR_SESSION_NOT_FOCUSED;memset(&sample,1,sizeof sample);
    assert(lg_xr_actions_sample(&a,HANDLE(XrSpace,9),123,&sample)==XR_SESSION_NOT_FOCUSED&&!sample.valid[0]&&!sample.trigger[0]&&queries==old_queries);
    assert(lg_xr_actions_rumble(&a,1,.7f,35)==XR_SUCCESS);
    lg_xr_actions_destroy(&a);assert(dead_spaces==2&&dead_actions==8&&dead_sets==1&&!a.session);
    lg_xr_actions_destroy(&a);assert(dead_sets==1);
    created=spaces=dead_spaces=dead_actions=dead_sets=0;fail_action=4;
    assert(lg_xr_actions_create(&a,HANDLE(XrInstance,1),HANDLE(XrSession,2))==XR_ERROR_RUNTIME_FAILURE);
    assert(dead_spaces==0&&dead_actions==3&&dead_sets==1&&!a.set&&!a.session);
    puts("ok: shared action bindings, active/valid poses, focus loss, haptics and partial initialization cleanup");return 0;
}
