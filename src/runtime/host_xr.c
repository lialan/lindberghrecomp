/*
 * host_xr.c - the headset, through OpenXR (Meta Quest, Android XR).
 *
 * host_sdl.c still owns the window, the GL context and the cabinet; this adds
 * what a headset needs around each frame the game draws:
 *
 *   xr_frame_begin()  wait for the headset's next frame, read where the eyes
 *                     and controllers are, hand the eyes to stereo.c and the
 *                     gun to the cabinet;
 *   xr_frame_end()    copy the two eye images into the headset's swapchains
 *                     and submit them.
 *
 * The game's camera is the LOCAL reference space: its origin is where the
 * player's head was when the app started (or was last recentred with the
 * Meta button), so the game's view starts straight ahead at eye height.
 *
 * GL here bypasses gl4es: the eye images are plain GLES textures, and the
 * swapchain images are textures gl4es never saw. Every binding changed is put
 * back, so gl4es' idea of the state stays true.
 *
 * Settings, one per line in vr.cfg next to the game:
 *   scale 1.0           game units per metre
 *   screen 3.0          distance of the HUD's screen, in metres
 *   resolution 1440     the longer side of each eye's image, in pixels
 *   refresh 90          display rate to ask for (72, 80, 90, 120)
 *   hand right          the gun hand (a trigger on the other hand moves it)
 */
#if defined(__ANDROID__)

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <SDL.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "lindbergh_rt.h"
#include "stereo.h"

/* host_sdl.c: the gun and the cabinet's buttons, from the controllers */
void host_vr_input(int gun_x, int gun_y, unsigned held, int coin);

/* ---- raw GLES, past gl4es ---- */
static struct {
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void (*GetIntegerv)(GLenum, GLint *);
    GLboolean (*IsEnabled)(GLenum);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    const GLubyte *(*GetString)(GLenum);
} gl;

static int gl_load(void)
{
    void *lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return 0;
#define L(n) if (!(*(void **)&gl.n = dlsym(lib, "gl" #n))) { fprintf(stderr, "[xr] no gl" #n "\n"); return 0; }
    L(BindFramebuffer) L(GenFramebuffers) L(FramebufferTexture2D) L(CheckFramebufferStatus)
    L(BlitFramebuffer) L(GetIntegerv) L(IsEnabled) L(Enable) L(Disable) L(GetString)
#undef L
    return 1;
}

#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9

/* ---- settings ---- */
static struct {
    float scale, screen;
    int   resolution, refresh, left_handed;
} cfg = { 1.0f, 3.0f, 1440, 90, 0 };

static void cfg_load(void)
{
    FILE *f = fopen("vr.cfg", "r");
    if (!f) return;
    char key[32], val[32];
    while (fscanf(f, "%31s %31s", key, val) == 2) {
        if (!strcmp(key, "scale")) cfg.scale = (float)atof(val);
        else if (!strcmp(key, "screen")) cfg.screen = (float)atof(val);
        else if (!strcmp(key, "resolution")) cfg.resolution = atoi(val);
        else if (!strcmp(key, "refresh")) cfg.refresh = atoi(val);
        else if (!strcmp(key, "hand")) cfg.left_handed = !strcmp(val, "left");
    }
    fclose(f);
    if (cfg.scale <= 0) cfg.scale = 1;
    if (cfg.screen <= 0.2f) cfg.screen = 3;
    if (cfg.resolution < 512 || cfg.resolution > 4096) cfg.resolution = 1440;
}

/* ---- the session ---- */
static XrInstance  instance;
static XrSystemId  sys;
static XrSession   session;
static XrSpace     local;
static XrSessionState state = XR_SESSION_STATE_UNKNOWN;
static int         running;
static XrFrameState frame = { XR_TYPE_FRAME_STATE };
static int         in_frame;           /* xrBeginFrame done, xrEndFrame owed */
static XrView      views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
static XrFovf      fov[2];             /* what was rendered: symmetric */
static int         views_ok;

static struct Eye {
    XrSwapchain chain;
    uint32_t    n, w, h;
    XrSwapchainImageOpenGLESKHR img[8];
    GLuint      fb[8];
    uint32_t    acquired;
} eye[2];
static GLuint read_fb;
static int64_t color_format;

static PFN_xrRequestDisplayRefreshRateFB request_rate;
static PFN_xrEnumerateDisplayRefreshRatesFB enumerate_rates;

#define XRCHECK(call) do { XrResult r_ = (call); if (XR_FAILED(r_)) { \
    char s_[64] = ""; if (instance) xrResultToString(instance, r_, s_); \
    fprintf(stderr, "[xr] %s failed: %d %s\n", #call, (int)r_, s_); return 0; } } while (0)

/* ---- input ---- */
enum { HAND_L, HAND_R };
static XrActionSet set;
static XrAction a_aim, a_trigger, a_grip, a_a, a_b, a_stick, a_menu, a_haptic;
static XrSpace aim_space[2];
static XrPath hand_path[2];
static int gun_hand = HAND_R;
static int prev[2][8];                 /* last state of each button, per hand */

static int action(XrAction *out, const char *name, const char *label, XrActionType type)
{
    XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
    ai.actionType = type;
    strcpy(ai.actionName, name);
    strcpy(ai.localizedActionName, label);
    ai.countSubactionPaths = 2;
    ai.subactionPaths = hand_path;
    XRCHECK(xrCreateAction(set, &ai, out));
    return 1;
}

static XrPath path(const char *s) { XrPath p = XR_NULL_PATH; xrStringToPath(instance, s, &p); return p; }

static int input_init(void)
{
    hand_path[HAND_L] = path("/user/hand/left");
    hand_path[HAND_R] = path("/user/hand/right");
    XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(si.actionSetName, "gun");
    strcpy(si.localizedActionSetName, "Gun");
    XRCHECK(xrCreateActionSet(instance, &si, &set));
    if (!action(&a_aim, "aim", "Aim", XR_ACTION_TYPE_POSE_INPUT) ||
        !action(&a_trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !action(&a_grip, "grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !action(&a_a, "lower", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !action(&a_b, "upper", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !action(&a_stick, "stick", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !action(&a_menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !action(&a_haptic, "recoil", "Recoil", XR_ACTION_TYPE_VIBRATION_OUTPUT)) return 0;

    static const struct { XrAction *a; const char *p; } B[] = {
        { &a_aim, "/user/hand/left/input/aim/pose" }, { &a_aim, "/user/hand/right/input/aim/pose" },
        { &a_trigger, "/user/hand/left/input/trigger/value" }, { &a_trigger, "/user/hand/right/input/trigger/value" },
        { &a_grip, "/user/hand/left/input/squeeze/value" }, { &a_grip, "/user/hand/right/input/squeeze/value" },
        { &a_a, "/user/hand/left/input/x/click" }, { &a_a, "/user/hand/right/input/a/click" },
        { &a_b, "/user/hand/left/input/y/click" }, { &a_b, "/user/hand/right/input/b/click" },
        { &a_stick, "/user/hand/left/input/thumbstick/click" }, { &a_stick, "/user/hand/right/input/thumbstick/click" },
        { &a_menu, "/user/hand/left/input/menu/click" },
        { &a_haptic, "/user/hand/left/output/haptic" }, { &a_haptic, "/user/hand/right/output/haptic" },
    };
    XrActionSuggestedBinding sb[sizeof B / sizeof B[0]];
    for (unsigned i = 0; i < sizeof B / sizeof B[0]; i++) { sb[i].action = *B[i].a; sb[i].binding = path(B[i].p); }
    XrInteractionProfileSuggestedBinding ps = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    ps.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    ps.countSuggestedBindings = sizeof B / sizeof B[0];
    ps.suggestedBindings = sb;
    XRCHECK(xrSuggestInteractionProfileBindings(instance, &ps));

    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo ac = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        ac.action = a_aim; ac.subactionPath = hand_path[h]; ac.poseInActionSpace.orientation.w = 1;
        XRCHECK(xrCreateActionSpace(session, &ac, &aim_space[h]));
    }
    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = 1; at.actionSets = &set;
    XRCHECK(xrAttachSessionActionSets(session, &at));
    return 1;
}

static float get_float(XrAction a, int h)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a; gi.subactionPath = hand_path[h];
    XrActionStateFloat s = { XR_TYPE_ACTION_STATE_FLOAT };
    return XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &s)) && s.isActive ? s.currentState : 0;
}

static int get_bool(XrAction a, int h)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a; gi.subactionPath = hand_path[h];
    XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &s)) && s.isActive && s.currentState;
}

static void rumble(int h, float amp, int ms)
{
    XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION };
    v.amplitude = amp; v.duration = (XrDuration)ms * 1000000; v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
    hi.action = a_haptic; hi.subactionPath = hand_path[h];
    xrApplyHapticFeedback(session, &hi, (XrHapticBaseHeader *)&v);
}

/* Rotate v by quaternion q. */
static void qrot(XrQuaternionf q, const float v[3], float out[3])
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* Cabinet bits, as in lindbergh_rt.h */
static void input_frame(XrTime t)
{
    XrActiveActionSet as = { set, XR_NULL_PATH };
    XrActionsSyncInfo sy = { XR_TYPE_ACTIONS_SYNC_INFO };
    sy.countActiveActionSets = 1; sy.activeActionSets = &as;
    if (XR_FAILED(xrSyncActions(session, &sy))) return;

    int trig[2], grip[2], btn_a[2], btn_b[2], stick[2];
    for (int h = 0; h < 2; h++) {
        trig[h] = get_float(a_trigger, h) > 0.55f;
        grip[h] = get_float(a_grip, h) > 0.6f;
        btn_a[h] = get_bool(a_a, h); btn_b[h] = get_bool(a_b, h); stick[h] = get_bool(a_stick, h);
        /* pulling the trigger on the other hand moves the gun there */
        if (trig[h] && !prev[h][0] && h != gun_hand) gun_hand = h;
    }
    int menu = get_bool(a_menu, HAND_L);
    int g = gun_hand;

    float gx = -100000, gy = -100000;
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    int aimed = 0;
    if (XR_SUCCEEDED(xrLocateSpace(aim_space[g], local, t, &loc)) &&
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
        (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        static const float fwd[3] = { 0, 0, -1 };
        float d[3], o[3] = { loc.pose.position.x * cfg.scale, loc.pose.position.y * cfg.scale, loc.pose.position.z * cfg.scale };
        qrot(loc.pose.orientation, fwd, d);
        float x, y;
        if (stereo_ray_to_screen(o, d, &x, &y) && x >= 0 && x < 640 && y >= 0 && y < 480) {
            gx = x; gy = y; aimed = 1;
        }
    }

    unsigned held = 0;
    if (trig[g] && aimed) held |= CAB_P1_TRIGGER;
    if (trig[g] && !aimed) held |= CAB_P1_RELOAD;  /* the arcade's reload: fire off the screen */
    if (grip[g]) held |= CAB_P1_RELOAD;
    if (btn_a[g]) held |= CAB_P1_ACTION;
    if (btn_b[g]) held |= CAB_P1_FIREMODE;
    if (stick[0] || stick[1]) held |= CAB_P1_START;
    int coin = menu && !prev[HAND_L][6];
    host_vr_input((int)gx, (int)gy, held, coin);

    if (trig[g] && !prev[g][0]) rumble(g, aimed ? 0.6f : 0.3f, 40);
    for (int h = 0; h < 2; h++) {
        prev[h][0] = trig[h]; prev[h][1] = grip[h]; prev[h][2] = btn_a[h];
        prev[h][3] = btn_b[h]; prev[h][4] = stick[h];
    }
    prev[HAND_L][6] = menu;
}

/* ---- events ---- */
static int poll_events(void)
{
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            state = ((XrEventDataSessionStateChanged *)&ev)->state;
            fprintf(stderr, "[xr] session state %d\n", (int)state);
            if (state == XR_SESSION_STATE_READY && !running) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(session, &bi))) {
                    running = 1;
                    if (request_rate && cfg.refresh) {
                        XrResult r = request_rate(session, (float)cfg.refresh);
                        fprintf(stderr, "[xr] asked for %d Hz: %d\n", cfg.refresh, (int)r);
                    }
                }
            } else if (state == XR_SESSION_STATE_STOPPING && running) {
                xrEndSession(session);
                running = 0;
            } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                fprintf(stderr, "[xr] session over - exiting\n");
                exit(0);
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            exit(0);
        }
        ev = (XrEventDataBuffer){ XR_TYPE_EVENT_DATA_BUFFER };
    }
    return 1;
}

/* ---- set-up ---- */
static float symmetric(float a, float b) { a = fabsf(tanf(a)); b = fabsf(tanf(b)); return a > b ? a : b; }

int xr_init(void)
{
    if (!gl_load()) { fprintf(stderr, "[xr] cannot load GLES\n"); return 0; }
    cfg_load();

    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    JavaVM *vm = NULL;
    (*env)->GetJavaVM(env, &vm);
    jobject activity = (jobject)SDL_AndroidGetActivity();

    PFN_xrInitializeLoaderKHR init = NULL;
    XRCHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init));
    XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
    li.applicationVM = vm; li.applicationContext = activity;
    XRCHECK(init((XrLoaderInitInfoBaseHeaderKHR *)&li));

    const char *ext[4] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
    uint32_t next = 2, n = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    XrExtensionProperties *props = calloc(n ? n : 1, sizeof *props);
    for (uint32_t i = 0; i < n; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, n, &n, props);
    int have_rate = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(props[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) have_rate = 1;
    free(props);
    if (have_rate) ext[next++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;

    XrInstanceCreateInfoAndroidKHR ai = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    ai.applicationVM = vm; ai.applicationActivity = activity;
    XrInstanceCreateInfo ic = { XR_TYPE_INSTANCE_CREATE_INFO };
    ic.next = &ai;
    ic.enabledExtensionCount = next; ic.enabledExtensionNames = ext;
    strcpy(ic.applicationInfo.applicationName, "Ghost Squad Evolution VR");
    strcpy(ic.applicationInfo.engineName, "lindberghrecomp");
    ic.applicationInfo.applicationVersion = 1;
    ic.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    XRCHECK(xrCreateInstance(&ic, &instance));
    if (have_rate) {
        xrGetInstanceProcAddr(instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&request_rate);
        xrGetInstanceProcAddr(instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&enumerate_rates);
    }

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHECK(xrGetSystem(instance, &sgi, &sys));

    PFN_xrGetOpenGLESGraphicsRequirementsKHR req_fn = NULL;
    XRCHECK(xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req_fn));
    XrGraphicsRequirementsOpenGLESKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    XRCHECK(req_fn(instance, sys, &req));

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();
    EGLint cfg_id = 0, count = 0;
    eglQueryContext(dpy, ctx, EGL_CONFIG_ID, &cfg_id);
    EGLint attrs[] = { EGL_CONFIG_ID, cfg_id, EGL_NONE };
    EGLConfig config = NULL;
    eglChooseConfig(dpy, attrs, &config, 1, &count);
    XrGraphicsBindingOpenGLESAndroidKHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    gb.display = dpy; gb.config = config; gb.context = ctx;
    XrSessionCreateInfo sc = { XR_TYPE_SESSION_CREATE_INFO };
    sc.next = &gb; sc.systemId = sys;
    XRCHECK(xrCreateSession(instance, &sc, &session));

    XrReferenceSpaceCreateInfo rc = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rc.poseInReferenceSpace.orientation.w = 1;
    XRCHECK(xrCreateReferenceSpace(session, &rc, &local));
    if (!input_init()) return 0;

    /* Colour: an sRGB swapchain written raw, as the game's colours already
     * are sRGB; a plain RGBA8 one only if that is all there is. */
    uint32_t nf = 0;
    XRCHECK(xrEnumerateSwapchainFormats(session, 0, &nf, NULL));
    int64_t *fmts = calloc(nf ? nf : 1, sizeof *fmts);
    xrEnumerateSwapchainFormats(session, nf, &nf, fmts);
    for (uint32_t i = 0; i < nf; i++) if (fmts[i] == GL_SRGB8_ALPHA8) color_format = GL_SRGB8_ALPHA8;
    if (!color_format) for (uint32_t i = 0; i < nf; i++) if (fmts[i] == GL_RGBA8) color_format = GL_RGBA8;
    free(fmts);
    if (!color_format) { fprintf(stderr, "[xr] no 8-bit colour swapchain\n"); return 0; }

    XrViewConfigurationView vc[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t nv = 0;
    XRCHECK(xrEnumerateViewConfigurationViews(instance, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vc));
    uint32_t w = vc[0].recommendedImageRectWidth, h = vc[0].recommendedImageRectHeight;
    float f = (float)cfg.resolution / (float)(w > h ? w : h);
    w = (uint32_t)(w * f) & ~3u; h = (uint32_t)(h * f) & ~3u;
    if (w > vc[0].maxImageRectWidth) w = vc[0].maxImageRectWidth;
    if (h > vc[0].maxImageRectHeight) h = vc[0].maxImageRectHeight;

    for (int i = 0; i < 2; i++) {
        XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = color_format; ci.sampleCount = 1; ci.width = w; ci.height = h;
        ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        XRCHECK(xrCreateSwapchain(session, &ci, &eye[i].chain));
        eye[i].w = w; eye[i].h = h;
        XRCHECK(xrEnumerateSwapchainImages(eye[i].chain, 0, &eye[i].n, NULL));
        if (eye[i].n > 8) eye[i].n = 8;
        for (uint32_t j = 0; j < eye[i].n; j++) eye[i].img[j].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        XRCHECK(xrEnumerateSwapchainImages(eye[i].chain, eye[i].n, &eye[i].n, (XrSwapchainImageBaseHeader *)eye[i].img));
        gl.GenFramebuffers((GLsizei)eye[i].n, eye[i].fb);
        GLint was = 0; gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &was);
        for (uint32_t j = 0; j < eye[i].n; j++) {
            gl.BindFramebuffer(GL_FRAMEBUFFER, eye[i].fb[j]);
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye[i].img[j].image, 0);
            if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                fprintf(stderr, "[xr] swapchain framebuffer %d/%u incomplete\n", i, j);
        }
        gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)was);
    }
    gl.GenFramebuffers(1, &read_fb);

    stereo_eye_size((int)w, (int)h);
    stereo_set_screen_distance(cfg.screen * cfg.scale);
    stereo_start();
    gun_hand = cfg.left_handed ? HAND_L : HAND_R;
    fprintf(stderr, "[xr] ready: %ux%u per eye, %s swapchain, %.2f game units/m, HUD screen at %.1f m, on %s\n",
            w, h, color_format == GL_SRGB8_ALPHA8 ? "sRGB" : "RGBA8", cfg.scale, cfg.screen,
            (const char *)gl.GetString(GL_RENDERER));

    /* the session usually becomes ready within a few polls */
    for (int i = 0; i < 200 && !running; i++) { poll_events(); if (!running) usleep(10000); }
    return 1;
}

/* ---- per frame ---- */

/* game camera space -> eye space, for a pose in LOCAL space (metres) */
static void view_matrix(XrPosef p, float m[16])
{
    XrQuaternionf q = p.orientation, qi = { -q.x, -q.y, -q.z, q.w };
    const float ex[3] = { 1, 0, 0 }, ey[3] = { 0, 1, 0 }, ez[3] = { 0, 0, 1 };
    float c0[3], c1[3], c2[3];
    qrot(qi, ex, c0); qrot(qi, ey, c1); qrot(qi, ez, c2);
    float t[3] = { -p.position.x * cfg.scale, -p.position.y * cfg.scale, -p.position.z * cfg.scale }, rt[3];
    qrot(qi, t, rt);
    memset(m, 0, 64);
    m[0] = c0[0]; m[1] = c0[1]; m[2] = c0[2];
    m[4] = c1[0]; m[5] = c1[1]; m[6] = c1[2];
    m[8] = c2[0]; m[9] = c2[1]; m[10] = c2[2];
    m[12] = rt[0]; m[13] = rt[1]; m[14] = rt[2]; m[15] = 1;
}

void xr_frame_begin(void)
{
    poll_events();
    if (!running) { views_ok = 0; return; }
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    frame = (XrFrameState){ XR_TYPE_FRAME_STATE };
    if (XR_FAILED(xrWaitFrame(session, &wi, &frame))) return;
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    if (XR_FAILED(xrBeginFrame(session, &bi))) return;
    in_frame = 1;

    XrViewLocateInfo vl = { XR_TYPE_VIEW_LOCATE_INFO };
    vl.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vl.displayTime = frame.predictedDisplayTime; vl.space = local;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    uint32_t n = 0;
    views_ok = XR_SUCCEEDED(xrLocateViews(session, &vl, &vs, 2, &n, views)) &&
               (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    if (views_ok) {
        for (int i = 0; i < 2; i++) {
            /* A symmetric frustum covering the eye's own: the same pixel then
             * looks the same way in both eyes, as the game's 2D assumes. */
            float tx = symmetric(views[i].fov.angleLeft, views[i].fov.angleRight);
            float ty = symmetric(views[i].fov.angleDown, views[i].fov.angleUp);
            fov[i].angleLeft = -atanf(tx); fov[i].angleRight = atanf(tx);
            fov[i].angleDown = -atanf(ty); fov[i].angleUp = atanf(ty);
            float m[16];
            view_matrix(views[i].pose, m);
            stereo_set_eye(i, m, tx, tx, ty, ty);
        }
    }
    if (state == XR_SESSION_STATE_FOCUSED) input_frame(frame.predictedDisplayTime);
    else host_vr_input(-100000, -100000, 0, 0);
}

void xr_frame_end(void)
{
    if (!in_frame) return;
    in_frame = 0;
    XrCompositionLayerProjectionView pv[2];
    int ok = views_ok && frame.shouldRender;
    if (ok) {
        GLint draw = 0, read = 0;
        gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST);
        if (scissor) gl.Disable(GL_SCISSOR_TEST);
        /* write the game's sRGB values as they are */
        if (color_format == GL_SRGB8_ALPHA8) gl.Disable(GL_FRAMEBUFFER_SRGB_EXT);
        for (int i = 0; i < 2; i++) {
            struct Eye *e = &eye[i];
            XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (XR_FAILED(xrAcquireSwapchainImage(e->chain, &ai, &e->acquired))) { ok = 0; break; }
            XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(e->chain, &wi);
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
            gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stereo_eye_texture(i), 0);
            gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, e->fb[e->acquired]);
            gl.BlitFramebuffer(0, 0, (GLint)e->w, (GLint)e->h, 0, 0, (GLint)e->w, (GLint)e->h,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(e->chain, &ri);

            pv[i] = (XrCompositionLayerProjectionView){ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            pv[i].pose = views[i].pose;
            pv[i].fov = fov[i];
            pv[i].subImage.swapchain = e->chain;
            pv[i].subImage.imageRect.extent.width = (int32_t)e->w;
            pv[i].subImage.imageRect.extent.height = (int32_t)e->h;
        }
        if (color_format == GL_SRGB8_ALPHA8) gl.Enable(GL_FRAMEBUFFER_SRGB_EXT);
        if (scissor) gl.Enable(GL_SCISSOR_TEST);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    }
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = local; layer.viewCount = 2; layer.views = pv;
    const XrCompositionLayerBaseHeader *layers[1] = { (XrCompositionLayerBaseHeader *)&layer };
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime = frame.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = ok ? 1 : 0; ei.layers = layers;
    xrEndFrame(session, &ei);
}

int xr_running(void) { return running; }

#endif /* __ANDROID__ */
