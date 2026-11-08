#include "xr_actions.h"
#include <string.h>
#define CHECK(call) do {result=(call);if(XR_FAILED(result))goto fail;}while(0)
XrResult lg_xr_actions_create(LgXrActions *a,XrInstance instance,XrSession session)
{
    XrResult result=XR_SUCCESS;memset(a,0,sizeof *a);a->instance=instance;a->session=session;
    CHECK(xrStringToPath(instance,"/user/hand/left",&a->hand_path[0]));
    CHECK(xrStringToPath(instance,"/user/hand/right",&a->hand_path[1]));
    XrActionSetCreateInfo set={XR_TYPE_ACTION_SET_CREATE_INFO};strcpy(set.actionSetName,"gun");strcpy(set.localizedActionSetName,"Gun");
    CHECK(xrCreateActionSet(instance,&set,&a->set));
    static const struct {const char *name,*label;XrActionType type;} spec[]={
        {"aim","Aim",XR_ACTION_TYPE_POSE_INPUT},{"trigger","Trigger",XR_ACTION_TYPE_FLOAT_INPUT},
        {"grip","Grip",XR_ACTION_TYPE_FLOAT_INPUT},{"lower","A / X",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"upper","B / Y",XR_ACTION_TYPE_BOOLEAN_INPUT},{"stick","Thumbstick click",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"menu","Menu",XR_ACTION_TYPE_BOOLEAN_INPUT},{"thumb","Thumbstick",XR_ACTION_TYPE_VECTOR2F_INPUT},{"recoil","Recoil",XR_ACTION_TYPE_VIBRATION_OUTPUT}};
    for(unsigned i=0;i<LG_XR_ACTION_COUNT;++i) {
        XrActionCreateInfo create={XR_TYPE_ACTION_CREATE_INFO};create.actionType=spec[i].type;
        strcpy(create.actionName,spec[i].name);strcpy(create.localizedActionName,spec[i].label);
        create.countSubactionPaths=2;create.subactionPaths=a->hand_path;
        CHECK(xrCreateAction(a->set,&create,&a->action[i]));
    }
    static const struct {int action;const char *path;} bindings[]={
        {LG_XR_AIM,"/user/hand/left/input/aim/pose"},{LG_XR_AIM,"/user/hand/right/input/aim/pose"},
        {LG_XR_TRIGGER,"/user/hand/left/input/trigger/value"},{LG_XR_TRIGGER,"/user/hand/right/input/trigger/value"},
        {LG_XR_GRIP,"/user/hand/left/input/squeeze/value"},{LG_XR_GRIP,"/user/hand/right/input/squeeze/value"},
        {LG_XR_LOWER,"/user/hand/left/input/x/click"},{LG_XR_LOWER,"/user/hand/right/input/a/click"},
        {LG_XR_UPPER,"/user/hand/left/input/y/click"},{LG_XR_UPPER,"/user/hand/right/input/b/click"},
        {LG_XR_STICK,"/user/hand/left/input/thumbstick/click"},{LG_XR_STICK,"/user/hand/right/input/thumbstick/click"},
        {LG_XR_MENU,"/user/hand/left/input/menu/click"},
        {LG_XR_THUMB,"/user/hand/left/input/thumbstick"},{LG_XR_THUMB,"/user/hand/right/input/thumbstick"},
        {LG_XR_HAPTIC,"/user/hand/left/output/haptic"},{LG_XR_HAPTIC,"/user/hand/right/output/haptic"}};
    XrActionSuggestedBinding suggested[sizeof bindings/sizeof bindings[0]];
    for(unsigned i=0;i<sizeof bindings/sizeof bindings[0];++i) {
        suggested[i].action=a->action[bindings[i].action];CHECK(xrStringToPath(instance,bindings[i].path,&suggested[i].binding));
    }
    XrInteractionProfileSuggestedBinding profile={XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    CHECK(xrStringToPath(instance,"/interaction_profiles/oculus/touch_controller",&profile.interactionProfile));
    profile.countSuggestedBindings=sizeof bindings/sizeof bindings[0];profile.suggestedBindings=suggested;
    CHECK(xrSuggestInteractionProfileBindings(instance,&profile));
    for(int h=0;h<2;++h) {
        XrActionSpaceCreateInfo space={XR_TYPE_ACTION_SPACE_CREATE_INFO};space.action=a->action[LG_XR_AIM];space.subactionPath=a->hand_path[h];
        space.poseInActionSpace.orientation.w=1;CHECK(xrCreateActionSpace(session,&space,&a->aim_space[h]));
    }
    XrSessionActionSetsAttachInfo attach={XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};attach.countActionSets=1;attach.actionSets=&a->set;
    CHECK(xrAttachSessionActionSets(session,&attach));return XR_SUCCESS;
fail:lg_xr_actions_destroy(a);return result;
}
#undef CHECK
void lg_xr_actions_destroy(LgXrActions *a)
{
    for(int h=0;h<2;++h)if(a->aim_space[h])xrDestroySpace(a->aim_space[h]);
    for(int i=0;i<LG_XR_ACTION_COUNT;++i)if(a->action[i])xrDestroyAction(a->action[i]);
    if(a->set)xrDestroyActionSet(a->set);memset(a,0,sizeof *a);
}
XrResult lg_xr_actions_sync(const LgXrActions *a)
{
    XrActiveActionSet active={a->set,XR_NULL_PATH};XrActionsSyncInfo sync={XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets=1;sync.activeActionSets=&active;return xrSyncActions(a->session,&sync);
}
float lg_xr_action_float(const LgXrActions *a,XrAction action,int hand)
{
    XrActionStateGetInfo get={XR_TYPE_ACTION_STATE_GET_INFO};get.action=action;get.subactionPath=a->hand_path[hand];
    XrActionStateFloat state={XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(a->session,&get,&state))&&state.isActive?state.currentState:0;
}
int lg_xr_action_bool(const LgXrActions *a,XrAction action,int hand)
{
    XrActionStateGetInfo get={XR_TYPE_ACTION_STATE_GET_INFO};get.action=action;get.subactionPath=a->hand_path[hand];
    XrActionStateBoolean state={XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(a->session,&get,&state))&&state.isActive&&state.currentState;
}
int lg_xr_action_vec2(const LgXrActions *a,XrAction action,int hand,float *x,float *y)
{
    XrActionStateGetInfo get={XR_TYPE_ACTION_STATE_GET_INFO};get.action=action;get.subactionPath=a->hand_path[hand];
    XrActionStateVector2f state={XR_TYPE_ACTION_STATE_VECTOR2F};
    const int ok=XR_SUCCEEDED(xrGetActionStateVector2f(a->session,&get,&state))&&state.isActive;
    *x=ok?state.currentState.x:0;*y=ok?state.currentState.y:0;return ok;
}
XrResult lg_xr_actions_sample(const LgXrActions *a,XrSpace space,XrTime time,LgXrActionSample *sample)
{
    memset(sample,0,sizeof *sample);const XrResult synced=lg_xr_actions_sync(a);
    if(synced!=XR_SUCCESS)return synced;
    for(int h=0;h<2;++h) {
        sample->trigger[h]=lg_xr_action_float(a,a->action[LG_XR_TRIGGER],h);
        sample->grip[h]=lg_xr_action_float(a,a->action[LG_XR_GRIP],h);
        sample->lower[h]=lg_xr_action_bool(a,a->action[LG_XR_LOWER],h);
        sample->upper[h]=lg_xr_action_bool(a,a->action[LG_XR_UPPER],h);
        sample->stick[h]=lg_xr_action_bool(a,a->action[LG_XR_STICK],h);
        lg_xr_action_vec2(a,a->action[LG_XR_THUMB],h,&sample->thumb_x[h],&sample->thumb_y[h]);
        XrActionStateGetInfo get={XR_TYPE_ACTION_STATE_GET_INFO};get.action=a->action[LG_XR_AIM];get.subactionPath=a->hand_path[h];
        XrActionStatePose pose={XR_TYPE_ACTION_STATE_POSE};XrSpaceLocation located={XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags valid=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if(XR_SUCCEEDED(xrGetActionStatePose(a->session,&get,&pose))&&pose.isActive&&
           XR_SUCCEEDED(xrLocateSpace(a->aim_space[h],space,time,&located))&&(located.locationFlags&valid)==valid) {
            sample->valid[h]=1;sample->pose[h]=located.pose;
        }
    }
    sample->menu=lg_xr_action_bool(a,a->action[LG_XR_MENU],0);return XR_SUCCESS;
}
XrResult lg_xr_actions_rumble(const LgXrActions *a,int hand,float amplitude,int milliseconds)
{
    XrHapticVibration vibration={XR_TYPE_HAPTIC_VIBRATION};vibration.amplitude=amplitude;
    vibration.duration=(XrDuration)milliseconds*1000000;vibration.frequency=XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info={XR_TYPE_HAPTIC_ACTION_INFO};info.action=a->action[LG_XR_HAPTIC];info.subactionPath=a->hand_path[hand];
    return xrApplyHapticFeedback(a->session,&info,(const XrHapticBaseHeader *)&vibration);
}
