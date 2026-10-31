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
 *   aim 15              how far along the laser the game's shot meets it, in
 *                       metres (the game fires from the head, the laser from the gun)
 *   resolution 2280     the longer side of each eye's image, in pixels
 *   width 2064          each eye's width; 0 keeps the headset's own proportions
 *   refresh 120         display rate to ask for (72, 80, 90, 120); the game
 *                       draws at 60, so 120 shows every frame twice - evenly
 *                       - where 72 or 90 repeat some and not others (judder)
 *   hand right          the gun hand (a trigger on the other hand moves it)
 *   laser laser         aim aid: laser (beam and dot), dot, or off; holding
 *                       the off hand's upper button for a second cycles it
 *   coins auto          auto: a coin with every Start, so no coin button is
 *                       needed; manual: only the left menu button
 *
 * The laser setting is changed in the headset with the other hand's upper
 * button and saved back to vr.cfg.
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
#include "game_profile.h"
#include "stereo.h"
#include "xr_overlay.h"
#include "xr_menu.h"

/* host_sdl.c: the gun and the cabinet's buttons, from the controllers */
void host_vr_input(int gun_x, int gun_y, unsigned held, int coin);

#ifndef GL_TEXTURE_INTERNAL_FORMAT
#define GL_TEXTURE_WIDTH            0x1000
#define GL_TEXTURE_HEIGHT           0x1001
#define GL_TEXTURE_INTERNAL_FORMAT  0x1003
#endif

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
    void (*GenRenderbuffers)(GLsizei, GLuint *);
    void (*BindRenderbuffer)(GLenum, GLuint);
    void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    void (*FramebufferTextureLayer)(GLenum, GLenum, GLuint, GLint, GLint);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    GLenum (*GetError)(void);
    void (*BindTexture)(GLenum, GLuint);
    void (*GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
} gl;

static int gl_load(void)
{
    void *lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return 0;
#define L(n) if (!(*(void **)&gl.n = dlsym(lib, "gl" #n))) { fprintf(stderr, "[xr] no gl" #n "\n"); return 0; }
    L(BindFramebuffer) L(GenFramebuffers) L(FramebufferTexture2D) L(CheckFramebufferStatus)
    L(BlitFramebuffer) L(GetIntegerv) L(IsEnabled) L(Enable) L(Disable) L(GetString)
    L(GenRenderbuffers) L(BindRenderbuffer) L(RenderbufferStorage) L(FramebufferRenderbuffer)
    L(FramebufferTextureLayer) L(ReadPixels) L(GetError) L(BindTexture) L(GetTexLevelParameteriv)
#undef L
    return 1;
}

#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9

/* ---- settings ---- */
enum { LASER_ON, LASER_DOT, LASER_OFF };
static const char *const LASER_NAME[] = { "laser", "dot", "off" };

static const char *const SIGHT_NAME[] = { "none", "reddot", "holo" };

static struct {
    float scale, screen, aim;            /* aim: where the laser and the game's shot meet, metres */
    int   resolution, width, refresh, left_handed, laser, auto_coins;
    int   sight, crosshair, show_gun;         /* XO_SIGHT_*; the game's own sight; the gun model */
    int   hud_pitch;                          /* a head-locked HUD tilted up (+) or down, degrees */
} cfg = { 1.0f, 3.0f, 15.0f, 2280, 2064, 120, 0, LASER_ON, 1, XO_SIGHT_NONE, 1, 1 };

static void cfg_save(void)
{
    FILE *f = fopen("vr.cfg", "w");
    if (!f) return;
    fprintf(f, "scale %g\nscreen %g\naim %g\nresolution %d\nwidth %d\nrefresh %d\nhand %s\nlaser %s\ncoins %s\n"
               "sight %s\ncrosshair %s\ngun %s\nhud_pitch %d\n",
            cfg.scale, cfg.screen, cfg.aim, cfg.resolution, cfg.width, cfg.refresh, cfg.left_handed ? "left" : "right",
            LASER_NAME[cfg.laser], cfg.auto_coins ? "auto" : "manual",
            SIGHT_NAME[cfg.sight], cfg.crosshair ? "on" : "off", cfg.show_gun ? "on" : "off", cfg.hud_pitch);
    fclose(f);
}

static void cfg_set(const char *key, const char *val);

static void cfg_load(void)
{
    /* the game's own size of a metre, until vr.cfg says otherwise: set by
     * its android_main (Let's Go Jungle's world is in about decimetres) */
    { const char *v = getenv("LINDBERGH_XR_SCALE"); if (v && atof(v) > 0) cfg.scale = (float)atof(v); }
    FILE *f = fopen("vr.cfg", "r");
    if (!f) f = fopen("../vr.cfg", "r");    /* beside the game: where adb can put one */
    char key[32], val[32];
    while (f && fscanf(f, "%31s %31s", key, val) == 2) cfg_set(key, val);
    if (f) fclose(f);
    /* LINDBERGH_XR_SET="key=value,...": for this run only, not saved (testing) */
    const char *e = getenv("LINDBERGH_XR_SET");
    while (e && *e) {
        if (sscanf(e, "%31[^=]=%31[^,]", key, val) == 2) cfg_set(key, val);
        e = strchr(e, ',');
        if (e) e++;
    }
    if (cfg.scale <= 0) cfg.scale = 1;
    if (cfg.screen <= 0.2f) cfg.screen = 3;
    if (cfg.aim < 1.0f || cfg.aim > 200.0f) cfg.aim = 15.0f;
    if (cfg.resolution < 512 || cfg.resolution > 4096) cfg.resolution = 2280;
    if (cfg.width < 0 || cfg.width > 4096) cfg.width = 0;
}

static void cfg_set(const char *key, const char *val)
{
    {
        if (!strcmp(key, "scale")) cfg.scale = (float)atof(val);
        else if (!strcmp(key, "screen")) cfg.screen = (float)atof(val);
        else if (!strcmp(key, "hud_pitch")) cfg.hud_pitch = atoi(val);
        else if (!strcmp(key, "aim")) cfg.aim = (float)atof(val);
        else if (!strcmp(key, "resolution")) cfg.resolution = atoi(val);
        else if (!strcmp(key, "width")) cfg.width = atoi(val);
        else if (!strcmp(key, "refresh")) cfg.refresh = atoi(val);
        else if (!strcmp(key, "hand")) cfg.left_handed = !strcmp(val, "left");
        else if (!strcmp(key, "laser")) cfg.laser = !strcmp(val, "dot") ? LASER_DOT : !strcmp(val, "off") ? LASER_OFF : LASER_ON;
        else if (!strcmp(key, "coins")) cfg.auto_coins = strcmp(val, "manual") != 0;
        else if (!strcmp(key, "sight")) cfg.sight = !strcmp(val, "reddot") ? XO_SIGHT_RED_DOT : !strcmp(val, "holo") ? XO_SIGHT_HOLO : XO_SIGHT_NONE;
        else if (!strcmp(key, "crosshair")) cfg.crosshair = strcmp(val, "off") != 0;
        else if (!strcmp(key, "gun")) cfg.show_gun = strcmp(val, "off") != 0;
    }
}

/* ---- the session ---- */
static XrInstance  instance;
static XrSystemId  sys;
static XrSession   session;
static XrSpace     local;          /* LOCAL, re-anchored on the head (anchor_head) */
static XrSpace     local_base;     /* LOCAL as the headset keeps it */
static int         anchor_pending = 1;
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
    GLuint      fb[8], depth;
    uint32_t    acquired;
    XrSwapchain dchain;                /* the world's depth, for the compositor */
    uint32_t    dn;
    XrSwapchainImageOpenGLESKHR dimg[8];
    GLuint      dfb[8];
} eye[2];
/* XR_KHR_composition_layer_depth: with each eye's depth, the compositor
 * corrects a frame it shows twice (the game draws 60, the display runs 120)
 * for the head's movement as well as its turn. Half the eye's size: the copy
 * costs about 0.5 ms of GPU, where the full size cost 1 ms - a few frames a
 * second on a game already at the GPU's limit. LINDBERGH_XR_DEPTH=0: off,
 * =1: full size. */
static int have_depth_layer, depth_on, depth_div = 2;   /* depth at 1/depth_div of the eye's size */
static int depth_blit;                 /* the world's depth blits straight across (formats match): 1; a shader copies it: 0 */
static unsigned depth_checked;         /* the depth array depth_blit was decided for */
static GLuint depth_read_fb;
static int64_t depth_format;
static XoFrame xo;                     /* the gun and laser for this frame */
static int overlay_ok;
static GLuint read_fb;

/* LINDBERGH_FLASH_LOG=1: each eye's finished image sampled at 8x8 points every
 * frame, to find flashes - a frame unlike the ones either side of it, while
 * those two agree - and frames where the two eyes disagree far more than
 * usual. Diagnostic only: the read back waits on the GPU. */
#define FL_N (8 * 8 * 4)
static int fl_on = -1;
static GLuint fl_fb, fl_rb;
static unsigned char fl_img[3][2][FL_N];   /* frames f-2, f-1, f; per eye */
static unsigned fl_count, fl_flashes, fl_split;
extern unsigned g_frame;
static unsigned fl_diff(const unsigned char *a, const unsigned char *b)
{
    unsigned d = 0;
    for (int k = 0; k < FL_N; k++) if ((k & 3) != 3) d += (unsigned)abs((int)a[k] - (int)b[k]);
    return d / (FL_N / 4 * 3);              /* mean per channel, 0..255 */
}
static void fl_sample(int eye_i, GLint w, GLint h)
{
    if (!fl_fb) {
        gl.GenRenderbuffers(1, &fl_rb);
        gl.BindRenderbuffer(GL_RENDERBUFFER, fl_rb);
        gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 8, 8);
        gl.GenFramebuffers(1, &fl_fb);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fl_fb);
        gl.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fl_rb);
    }
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fl_fb);
    gl.BlitFramebuffer(0, 0, w, h, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, fl_fb);
    gl.ReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, fl_img[2][eye_i]);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
}
static void fl_frame(void)
{
    if (++fl_count >= 3) {
        for (int i = 0; i < 2; i++) {
            unsigned in = fl_diff(fl_img[0][i], fl_img[1][i]), out = fl_diff(fl_img[1][i], fl_img[2][i]);
            unsigned across = fl_diff(fl_img[0][i], fl_img[2][i]);
            if (in >= 24 && out >= 24 && across * 3 < (in < out ? in : out)) {
                fl_flashes++;
                unsigned m[2][3] = { { 0 } };
                for (int k = 0; k < FL_N; k++) if ((k & 3) != 3) { m[0][k & 3] += fl_img[1][i][k]; m[1][k & 3] += fl_img[0][i][k]; }
                fprintf(stderr, "[flash] frame %u, %s eye: one frame unlike its neighbours (%u in, %u out, %u across); "
                        "mean rgb %u %u %u, before %u %u %u\n", g_frame - 1, i ? "right" : "left", in, out, across,
                        m[0][0] / 64, m[0][1] / 64, m[0][2] / 64, m[1][0] / 64, m[1][1] / 64, m[1][2] / 64);
            }
        }
        unsigned lr = fl_diff(fl_img[2][0], fl_img[2][1]);
        if (lr >= 40) { fl_split++; if (fl_split < 200) fprintf(stderr, "[flash] frame %u: eyes differ by %u\n", g_frame, lr); }
        if (g_frame % 1800 == 0) fprintf(stderr, "[flash] frame %u: %u flashes, %u split frames so far\n", g_frame, fl_flashes, fl_split);
    }
    memcpy(fl_img[0], fl_img[1], sizeof fl_img[0]);
    memcpy(fl_img[1], fl_img[2], sizeof fl_img[1]);
}
static int64_t color_format;

static PFN_xrRequestDisplayRefreshRateFB request_rate;
static PFN_xrPerfSettingsSetPerformanceLevelEXT set_perf_level;
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
/* LINDBERGH_XR_FAKE_GUN=1: a stand-in for a controller (see below) */
static int fake_gun(void)
{
    static int fake = -1;
    if (fake < 0) { const char *v = getenv("LINDBERGH_XR_FAKE_GUN"); fake = v && *v && *v != '0'; }
    return fake;
}

/* ---- the settings panel ----
 * The left controller's menu button opens it to the player's left, turned
 * towards them, the game's screen still in view (the arcade's own Test menu
 * is worked from here too). Point the gun at a row and pull the trigger;
 * the game gets no input meanwhile. */
enum { M_SIGHT, M_LASER, M_CROSSHAIR, M_GUN, M_HAND, M_SCREEN, M_HUD, M_RATE, M_RES, M_COINS, M_COIN, M_RECENTRE,
       M_TEST, M_SERVICE, M_RESET, M_CLOSE, M_COUNT };
static struct {
    int open, hover, dirty;
    float c[3], right[3], up[3], n[3], hw, hh;  /* where it hangs, in LOCAL metres */
    int test_frames, service_frames, coin;
    int reset_armed;            /* "Reset game" chosen once: a second time resets */
} menu;
static float rates[16];
static int running_resolution, running_width;
/* the gun's ray this frame (game units), what it reached two frames ago */
static float probe_o[3], probe_d[3], aim_hit_t = -1, aim_reach;
static XrPosef head_pose;            /* between the eyes, facing as they do: where a head-locked HUD hangs */
static int head_ok;
static int aim_on_box;              /* this frame's aim went to a box on the screen */
static int probe_on;
static XrTime reload_t;            /* when the player last reloaded */
static void aim_probe(void);   /* the eye size this run was set up with */
static int nrates;

static void menu_items(XmItem *it)
{
    static const char *const sight[] = { "Point shooting", "Red dot", "Holographic" };
    static const char *const laser[] = { "Laser and dot", "Dot only", "Off" };
    static const char *const labels[M_COUNT] = {
        "Sight", "Laser", "Game crosshair", "Gun model", "Gun hand", "Screen distance", "HUD position", "Display rate", "Resolution per eye",
        "Coins", "Insert a coin", "Recentre view", "Arcade Test button (select)",
        "Arcade Service button (next)", "Reset game", "Close" };
    for (int i = 0; i < M_COUNT; i++) { it[i].label = labels[i]; it[i].value[0] = 0; }
    snprintf(it[M_SIGHT].value, 40, "%s", sight[cfg.sight]);
    snprintf(it[M_LASER].value, 40, "%s", laser[cfg.laser]);
    snprintf(it[M_CROSSHAIR].value, 40, "%s", cfg.crosshair ? "Shown" : "Hidden");
    snprintf(it[M_GUN].value, 40, "%s", cfg.show_gun ? "Shown" : "Hidden");
    snprintf(it[M_HAND].value, 40, "%s", gun_hand == HAND_L ? "Left" : "Right");
    snprintf(it[M_SCREEN].value, 40, "%.1f m", cfg.screen);
    if (!stereo_hud_is_head_locked()) snprintf(it[M_HUD].value, 40, "With the screen");
    else if (cfg.hud_pitch == 0) snprintf(it[M_HUD].value, 40, "Centre");
    else snprintf(it[M_HUD].value, 40, "%s %d deg", cfg.hud_pitch < 0 ? "Down" : "Up", abs(cfg.hud_pitch));
    snprintf(it[M_RATE].value, 40, "%d Hz", cfg.refresh);
    char res[24];
    if (cfg.width) snprintf(res, sizeof res, "%d x %d", cfg.width, cfg.resolution);
    else snprintf(res, sizeof res, "%d", cfg.resolution);
    if (cfg.resolution == running_resolution && cfg.width == running_width) snprintf(it[M_RES].value, 40, "%s", res);
    else snprintf(it[M_RES].value, 40, "%s (on restart)", res);
    snprintf(it[M_COINS].value, 40, "%s", cfg.auto_coins ? "Automatic" : "By menu");
    if (menu.reset_armed) snprintf(it[M_RESET].value, 40, "Pull again to reset");
}

/* The arcade's reset: the game starts again from its boot, as a new process
 * (the activity's restartGame, which needs a game whose activity has it) */
static void reset_game(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID m = (*env)->GetMethodID(env, cls, "restartGame", "()V");
    if (m) {
        fprintf(stderr, "[xr] reset: the game starts again\n");
        fflush(stderr);
        (*env)->CallVoidMethod(env, activity, m);
    } else {
        (*env)->ExceptionClear(env);
        fprintf(stderr, "[xr] reset: this app cannot restart itself\n");
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

/* the panel's title: LINDBERGH_XR_TITLE, which the game's profile sets (game_profile.h) */
static const char *app_title(void)
{
    const char *t = getenv("LINDBERGH_XR_TITLE");
    return t && *t ? t : "Lindbergh VR";
}

static void menu_paint(void)
{
    static unsigned char img[XM_W * XM_H * 4];
    XmItem it[M_COUNT];
    char title[80];
    menu_items(it);
    snprintf(title, sizeof title, "%s - settings", app_title());
    xm_paint(img, title, it, M_COUNT, menu.hover,
             "Point and pull the trigger. Menu button: close.");
    xo_panel_upload(img, XM_W, XM_H);
    menu.dirty = 0;
}

static void menu_place(void)
{
    float head[3], f[3];
    const float back[3] = { 0, 0, -1 };
    for (int i = 0; i < 3; i++) head[i] = 0.5f * ((&views[0].pose.position.x)[i] + (&views[1].pose.position.x)[i]);
    qrot(views[0].pose.orientation, back, f);
    float yaw = atan2f(-f[0], -f[2]) + 0.55f;   /* about 30 degrees to the left */
    float fwd[3] = { -sinf(yaw), 0, -cosf(yaw) };
    for (int i = 0; i < 3; i++) menu.c[i] = head[i] + fwd[i] * 0.95f;
    menu.c[1] = head[1] - 0.12f;
    for (int i = 0; i < 3; i++) menu.n[i] = -fwd[i];
    menu.up[0] = 0; menu.up[1] = 1; menu.up[2] = 0;
    menu.right[0] = menu.up[1] * menu.n[2] - menu.up[2] * menu.n[1];
    menu.right[1] = menu.up[2] * menu.n[0] - menu.up[0] * menu.n[2];
    menu.right[2] = menu.up[0] * menu.n[1] - menu.up[1] * menu.n[0];
    menu.hw = 0.33f; menu.hh = menu.hw * XM_H / XM_W;
}

/* where the ray meets the panel: 0..1 from its top left; 0 if it misses */
static int menu_hit(const float p[3], const float d[3], float *u, float *v, float hit[3])
{
    float dn = d[0] * menu.n[0] + d[1] * menu.n[1] + d[2] * menu.n[2];
    if (dn > -1e-4f) return 0;
    float t = ((menu.c[0] - p[0]) * menu.n[0] + (menu.c[1] - p[1]) * menu.n[1] + (menu.c[2] - p[2]) * menu.n[2]) / dn;
    if (t <= 0) return 0;
    float r[3];
    for (int i = 0; i < 3; i++) { hit[i] = p[i] + d[i] * t; r[i] = hit[i] - menu.c[i]; }
    float x = (r[0] * menu.right[0] + r[1] * menu.right[1] + r[2] * menu.right[2]) / menu.hw;
    float y = (r[0] * menu.up[0] + r[1] * menu.up[1] + r[2] * menu.up[2]) / menu.hh;
    *u = (x + 1) * 0.5f; *v = (1 - y) * 0.5f;
    return x >= -1 && x <= 1 && y >= -1 && y <= 1;
}

static void menu_choose(int item)
{
    static const float screens[] = { 2.0f, 2.5f, 3.0f, 4.0f, 5.0f };
    int armed = menu.reset_armed;
    menu.reset_armed = 0;           /* anything else chosen in between: no reset */
    switch (item) {
    case M_SIGHT: cfg.sight = (cfg.sight + 1) % 3; break;
    case M_LASER: cfg.laser = (cfg.laser + 1) % 3; break;
    case M_CROSSHAIR: cfg.crosshair = !cfg.crosshair; stereo_show_game_sight(cfg.crosshair); break;
    case M_GUN: cfg.show_gun = !cfg.show_gun; break;
    case M_HAND: gun_hand = 1 - gun_hand; cfg.left_handed = gun_hand == HAND_L; break;
    case M_SCREEN: {
        int k = 0;
        while (k < 5 && screens[k] <= cfg.screen + 0.01f) k++;
        cfg.screen = screens[k % 5];
        stereo_set_screen_distance(cfg.screen * cfg.scale);
        break;
    }
    case M_HUD: {       /* a head-locked HUD tilted, for where the eye rests */
        static const int pitches[] = { 0, -10, -20, -30, 10 };
        int k = 0;
        while (k < 5 && pitches[k] != cfg.hud_pitch) k++;
        cfg.hud_pitch = pitches[(k + 1) % 5];
        stereo_set_hud_pitch((float)cfg.hud_pitch);
        break;
    }
    case M_RATE:
        if (nrates) {
            int k = 0;
            while (k < nrates && rates[k] <= cfg.refresh + 0.5f) k++;
            cfg.refresh = (int)(rates[k % nrates] + 0.5f);
            if (request_rate) request_rate(session, (float)cfg.refresh);
        }
        break;
    case M_RES: {
        /* the eye images are set up once, so this takes effect next launch */
        static const int sizes[][2] = { { 0, 1216 }, { 0, 1440 }, { 0, 1680 }, { 0, 1920 }, { 0, 2208 }, { 2064, 2280 } };
        int k = 0;
        while (k < 6 && (sizes[k][1] < cfg.resolution || (sizes[k][1] == cfg.resolution && sizes[k][0] <= cfg.width))) k++;
        cfg.width = sizes[k % 6][0];
        cfg.resolution = sizes[k % 6][1];
        break;
    }
    case M_COINS: cfg.auto_coins = !cfg.auto_coins; break;
    case M_COIN: menu.coin = 1; break;
    case M_RECENTRE: anchor_pending = 1; break;
    case M_TEST: menu.test_frames = 12; break;
    case M_SERVICE: menu.service_frames = 12; break;
    case M_RESET:
        if (!armed) { menu.reset_armed = 1; break; }
        cfg_save();
        reset_game();
        break;
    case M_CLOSE: menu.open = 0; break;
    }
    cfg_save();
    menu.dirty = 1;
}

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
    int menu_btn = get_bool(a_menu, HAND_L);
    static int fake_menu = -1;          /* LINDBERGH_XR_FAKE_MENU=1 with the stand-in gun: the panel open, no clicks */
    if (fake_menu < 0) { const char *v = getenv("LINDBERGH_XR_FAKE_MENU"); fake_menu = v && *v == '1'; }
    if (fake_menu && !menu.open && views_ok) { menu.open = 1; menu_place(); menu.hover = -1; menu.dirty = 1; }
    if (menu_btn && !prev[HAND_L][6]) {
        menu.open = !menu.open; menu.reset_armed = 0;
        fprintf(stderr, "[xr] settings panel %s\n", menu.open ? "open" : "closed");
        if (menu.open) { menu_place(); menu.hover = -1; menu.dirty = 1; }
    }
    int g = gun_hand;

    float gx = -100000, gy = -100000;
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    int aimed = 0;
    xo.gun_valid = 0;
    int located = XR_SUCCEEDED(xrLocateSpace(aim_space[g], local, t, &loc)) &&
                  (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                  (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    if (!located && fake_gun()) {
        /* LINDBERGH_XR_FAKE_GUN=1, for a headset or emulator without
         * controllers: a gun at the right hip sweeping across the screen,
         * firing in half-second bursts, and for one second in six pointed at
         * the floor with the trigger held - the arcade's reload */
        double s = (double)t / 1e9;
        float yaw = (float)(0.25 * sin(s * 0.7)), pitch = (float)(0.05 + 0.08 * sin(s * 1.1));
        static int fixed = -1; static float fyaw, fpitch;   /* LINDBERGH_XR_FAKE_AIM=yaw,pitch (radians): held still */
        if (fixed < 0) { const char *v = getenv("LINDBERGH_XR_FAKE_AIM"); fixed = v && sscanf(v, "%f,%f", &fyaw, &fpitch) == 2; }
        if (fixed) { yaw = fyaw; pitch = fpitch; }
        int reload = fmod(s, 6.0) >= 5.0;
        if (reload) pitch = -0.9f;
        float cy = cosf(yaw / 2), sy = sinf(yaw / 2), cp = cosf(pitch / 2), sp = sinf(pitch / 2);
        loc.pose.orientation = (XrQuaternionf){ sp * cy, cp * sy, -sp * sy, cp * cy };
        loc.pose.position = (XrVector3f){ 0.16f, -0.22f, -0.28f };
        static int ads = -1;            /* LINDBERGH_XR_FAKE_ADS=1: shouldered, looking through the sight */
        if (ads < 0) { const char *v = getenv("LINDBERGH_XR_FAKE_ADS"); ads = v && *v == '1'; }
        if (ads) {
            loc.pose.position = (XrVector3f){ 0.0f, -0.080f, -0.16f };
            yaw *= 0.15f; pitch = 0.0f; reload = 0;
            cy = cosf(yaw / 2); sy = sinf(yaw / 2); cp = 1; sp = 0;
            loc.pose.orientation = (XrQuaternionf){ sp * cy, cp * sy, -sp * sy, cp * cy };
        }
        trig[g] = reload || fmod(s, 1.0) < 0.5;
        located = 1;
    }
    if (located) {
        static const float fwd[3] = { 0, 0, -1 }, tip[3] = { 0, -0.006f, -0.26f };
        float d[3], p[3] = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
        float o[3] = { p[0] * cfg.scale, p[1] * cfg.scale, p[2] * cfg.scale };
        qrot(loc.pose.orientation, fwd, d);
        /* The game fires from its camera - the head, where it was anchored -
         * through a point on its screen. The laser leaves the gun, a hand's
         * length away, so the two lines meet at one distance only: the
         * point cfg.aim metres along the laser, which the game is aimed
         * through. At the screen's own 5 m a soldier 12 m off was missed by
         * half a metre with the gun at the hip. */
        /* Better still, the point the laser actually reaches: the world's
         * depth along it, read back from the GPU (aim_hit_t). A flat frame -
         * a scope, a menu - is a picture on the screen: aimed at where the
         * laser meets it. So is CQC, whose markers are 2D on the screen while
         * the enemy stands a metre away. */
        float x, y, ah[3] = { 0, 0, 0 }, at, ax[3];
        int flat = stereo_flat_frame() || stereo_cqc_active();
        if (flat) at = d[2] < -1e-4f ? (-cfg.screen * cfg.scale - o[2]) / d[2] : -1;
        else if (aim_hit_t > 0) at = aim_hit_t;
        else at = d[2] < -1e-4f ? (-cfg.aim * cfg.scale - o[2]) / d[2] : -1;
        for (int i = 0; i < 3; i++) ax[i] = o[i] + d[i] * at;
        memcpy(probe_o, o, sizeof probe_o); memcpy(probe_d, d, sizeof probe_d); probe_on = 1;
        int gw, gh;
        stereo_game_size(&gw, &gh);
        if (at > 0 && stereo_ray_to_screen(ah, ax, &x, &y) && x >= 0 && x < gw && y >= 0 && y < gh) {
            gx = x; gy = y; aimed = 1; aim_reach = at / cfg.scale;
        }
        /* A box the game hangs in front of its camera - the path choice's
         * views - is aimed at where the laser crosses it, not where it meets
         * the world behind: from the hand those are degrees apart */
        aim_on_box = 0;
        {
            float xs, ys, ts;
            /* a head-locked HUD's boxes hang in front of the face: the laser in the head's frame */
            float ho[3], hd[3];
            int head = stereo_hud_is_head_locked() && head_ok;
            if (head) {
                XrQuaternionf iq = { -head_pose.orientation.x, -head_pose.orientation.y, -head_pose.orientation.z, head_pose.orientation.w };
                float r[3] = { o[0] - head_pose.position.x * cfg.scale, o[1] - head_pose.position.y * cfg.scale,
                               o[2] - head_pose.position.z * cfg.scale };
                qrot(iq, r, ho); qrot(iq, d, hd);
            }
            if (!flat && stereo_aim_box(head ? ho : o, head ? hd : d, head, &xs, &ys, &ts)) { gx = xs; gy = ys; aimed = 1; aim_reach = ts / cfg.scale; aim_on_box = 1; }
        }
        /* the overlay: the gun on the aim pose, the laser to the game's screen */
        xo.gun_valid = 1;
        xo.gun_q[0] = loc.pose.orientation.x; xo.gun_q[1] = loc.pose.orientation.y;
        xo.gun_q[2] = loc.pose.orientation.z; xo.gun_q[3] = loc.pose.orientation.w;
        memcpy(xo.gun_p, p, sizeof p);
        float m[3];
        qrot(loc.pose.orientation, tip, m);
        for (int i = 0; i < 3; i++) xo.muzzle[i] = p[i] + m[i];
        float reach = 5.0f;
        if (aimed) reach = aim_reach;
        for (int i = 0; i < 3; i++) xo.end[i] = p[i] + d[i] * reach;
        xo.beam = cfg.laser == LASER_ON;
        xo.dot = aimed && cfg.laser != LASER_OFF;
        if (menu.open) {        /* the laser points at the panel; the game is not aimed at */
            float u, v, hit[3];
            int on = menu_hit(p, d, &u, &v, hit);
            int row = on ? xm_row_at(u, v, M_COUNT) : -1;
            if (row != menu.hover) { menu.hover = row; menu.dirty = 1; }
            if (on) memcpy(xo.end, hit, sizeof hit);
            xo.beam = 1; xo.dot = on;
            if (trig[g] && !prev[g][0])     /* every pull on the panel, for a click that seems lost */
                fprintf(stderr, "[xr] panel trigger: %s hand, %s at %.2f,%.2f, row %d\n", g == HAND_L ? "left" : "right",
                        on ? "on the panel" : "off it", on ? u : -1, on ? v : -1, row);
            if (trig[g] && !prev[g][0] && row >= 0 && !fake_menu) { menu_choose(row); rumble(g, 0.25f, 15); }
            aimed = 0; gx = gy = -100000;
        }
    }
    xo.sight = cfg.show_gun ? cfg.sight : XO_SIGHT_NONE;
    xo.show_gun = cfg.show_gun;
    if (menu.open && menu.dirty) menu_paint();

    /* firing: a kick, a flash and a pulse ten times a second while held on
     * the screen - the cabinet gun's recoil */
    static XrTime fire_start;
    int firing = trig[g] && aimed && !menu.open;
    if (firing && !prev[g][0]) fire_start = t;
    float phase = firing ? fmodf((float)((t - fire_start) / 1000000) / 100.0f, 1.0f) : 1.0f;
    static float last_phase = 1.0f;
    xo.recoil = firing ? 0.018f * (1.0f - phase) * (1.0f - phase) : 0;
    xo.flash = firing && phase < 0.35f;
    if (firing && (phase < last_phase)) rumble(g, 0.7f, 35);
    last_phase = phase;

    /* the other hand's upper button (Y for the right-handed), held for a
     * second, opens and closes the settings panel too */
    int off = 1 - g;
    static XrTime panel_held;
    if (btn_b[off] && !prev[off][3]) panel_held = t;
    if (btn_b[off] && panel_held && t - panel_held >= 1000000000) {
        panel_held = 0;
        menu.open = !menu.open; menu.reset_armed = 0;
        if (menu.open) { menu_place(); menu.hover = -1; menu.dirty = 1; }
        rumble(off, 0.3f, 20);
        fprintf(stderr, "[xr] settings panel %s (held button)\n", menu.open ? "open" : "closed");
    }

    /* every button change, and the controllers the runtime reports: for
     * finding a button that does not come through */
    {
        static int last[2][6] = { { -1 } };
        int now[2][6];
        for (int h = 0; h < 2; h++) {
            now[h][0] = trig[h]; now[h][1] = grip[h]; now[h][2] = btn_a[h]; now[h][3] = btn_b[h];
            now[h][4] = stick[h]; now[h][5] = h == HAND_L ? menu_btn : 0;
        }
        if (memcmp(now, last, sizeof now)) {
            fprintf(stderr, "[xr] buttons L t%d g%d x%d y%d s%d m%d  R t%d g%d a%d b%d s%d\n", now[0][0], now[0][1],
                    now[0][2], now[0][3], now[0][4], now[0][5], now[1][0], now[1][1], now[1][2], now[1][3], now[1][4]);
            memcpy(last, now, sizeof now);
        }
        static int told;
        if (!told) {
            told = 1;
            for (int h = 0; h < 2; h++) {
                XrInteractionProfileState ip = { XR_TYPE_INTERACTION_PROFILE_STATE };
                char name[XR_MAX_PATH_LENGTH] = "?"; uint32_t len = 0;
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session, hand_path[h], &ip)) && ip.interactionProfile)
                    xrPathToString(instance, ip.interactionProfile, sizeof name, &len, name);
                fprintf(stderr, "[xr] %s hand: %s\n", h == HAND_L ? "left" : "right", name);
            }
        }
    }

    if (trig[g] && !prev[g][0] && !menu.open)       /* each pull: where the game is told it went */
        fprintf(stderr, "[aim] frame %u: pull at %d,%d%s (reach %.2f m, depth %s, flat %d%s)\n", g_frame, (int)gx, (int)gy,
                aimed ? "" : " - off the screen", aim_reach, aim_hit_t > 0 ? "read" : "assumed", stereo_flat_frame(),
                aim_on_box ? ", on a box" : "");

    unsigned held = 0;
    if (firing) held |= CAB_P1_TRIGGER;
    if (trig[g] && !aimed && !menu.open) held |= CAB_P1_RELOAD;  /* the arcade's reload: fire off the screen */
    if (grip[g] && !menu.open) held |= CAB_P1_RELOAD;
    if (held & CAB_P1_RELOAD) reload_t = t;
    stereo_game_gun_reloading(reload_t && t - reload_t < 1500000000);
    if (btn_a[g] && !menu.open) held |= CAB_P1_ACTION;
    if (btn_b[g] && !menu.open) held |= CAB_P1_FIREMODE;
    if (menu.test_frames) { menu.test_frames--; held |= CAB_TEST; }
    if (menu.service_frames) { menu.service_frames--; held |= CAB_SERVICE; }
    int start = (stick[0] || stick[1]) && !menu.open;
    /* the stand-in gun presses Start for a moment every ten seconds too */
    if (fake_gun() && fmod((double)t / 1e9, 10.0) < 0.2) start = 1;
    if (start) held |= CAB_P1_START;
    /* Coins: the left menu button, and with automatic coins one at start-up
     * and one with every press of Start - Start alone plays and continues. */
    static unsigned frames;
    int coin = menu.coin;
    menu.coin = 0;
    if (cfg.auto_coins && ((++frames == 240) || (start && !prev[HAND_L][7]))) coin = 1;
    prev[HAND_L][7] = start;
    stereo_set_gun((int)gx, (int)gy);
    host_vr_input((int)gx, (int)gy, held, coin);

    if (trig[g] && !aimed && !prev[g][0] && !menu.open) rumble(g, 0.3f, 40);
    for (int h = 0; h < 2; h++) {
        prev[h][0] = trig[h]; prev[h][1] = grip[h]; prev[h][2] = btn_a[h];
        prev[h][3] = btn_b[h]; prev[h][4] = stick[h];
    }
    prev[HAND_L][6] = menu_btn;
}

/* ---- events ---- */
static int poll_events(void)
{
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            state = ((XrEventDataSessionStateChanged *)&ev)->state;
            fprintf(stderr, "[xr] session state %d\n", (int)state);
            /* the headset put on, or the game back in front: anchor the game
             * camera on the head where it is now */
            if (state == XR_SESSION_STATE_FOCUSED) anchor_pending = 1;
            if (state == XR_SESSION_STATE_READY && !running) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(session, &bi))) {
                    running = 1;
                    if (enumerate_rates) {
                        uint32_t n = 0;
                        if (XR_SUCCEEDED(enumerate_rates(session, 16, &n, rates))) {
                            nrates = (int)n;
                            fprintf(stderr, "[xr] display rates:");
                            for (uint32_t k = 0; k < n; k++) fprintf(stderr, " %.0f", rates[k]);
                            fprintf(stderr, "\n");
                        }
                    }
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
                host_quit();
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            anchor_pending = 1;         /* recentred: the game camera follows */
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            host_quit();
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
    stereo_show_game_sight(cfg.crosshair);

    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    JavaVM *vm = NULL;
    (*env)->GetJavaVM(env, &vm);
    jobject activity = (jobject)SDL_AndroidGetActivity();

    PFN_xrInitializeLoaderKHR init = NULL;
    XRCHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init));
    XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
    li.applicationVM = vm; li.applicationContext = activity;
    XRCHECK(init((XrLoaderInitInfoBaseHeaderKHR *)&li));

    const char *ext[6] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
    uint32_t next = 2, n = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    XrExtensionProperties *props = calloc(n ? n : 1, sizeof *props);
    for (uint32_t i = 0; i < n; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, n, &n, props);
    int have_rate = 0, have_perf = 0, have_thread = 0;
    have_depth_layer = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(props[i].extensionName, XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME)) have_thread = 1;
        if (!strcmp(props[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) have_rate = 1;
        if (!strcmp(props[i].extensionName, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME)) have_perf = 1;
        if (!strcmp(props[i].extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME)) have_depth_layer = 1;
    }
    {   /* the game's choice (game_profile.h), unless LINDBERGH_XR_DEPTH=0 (off), 1 (full size) or 2 (half) */
        const char *v = getenv("LINDBERGH_XR_DEPTH");
        int mode = game_profile()->vr.compositor_depth;
        if (v && *v == '0') mode = LG_COMPOSITOR_DEPTH_OFF;
        if (v && *v == '1') mode = LG_COMPOSITOR_DEPTH_FULL;
        if (v && *v == '2') mode = LG_COMPOSITOR_DEPTH_HALF;
        if (mode == LG_COMPOSITOR_DEPTH_OFF) have_depth_layer = 0;
        depth_div = mode == LG_COMPOSITOR_DEPTH_FULL ? 1 : 2;
    }
    free(props);
    if (have_rate) ext[next++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    if (have_perf) ext[next++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
    if (have_thread) ext[next++] = XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME;
    if (have_depth_layer) ext[next++] = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;

    XrInstanceCreateInfoAndroidKHR ai = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    ai.applicationVM = vm; ai.applicationActivity = activity;
    XrInstanceCreateInfo ic = { XR_TYPE_INSTANCE_CREATE_INFO };
    ic.next = &ai;
    ic.enabledExtensionCount = next; ic.enabledExtensionNames = ext;
    snprintf(ic.applicationInfo.applicationName, sizeof ic.applicationInfo.applicationName, "%s", app_title());
    strcpy(ic.applicationInfo.engineName, "lindberghrecomp");
    ic.applicationInfo.applicationVersion = 1;
    ic.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    XRCHECK(xrCreateInstance(&ic, &instance));
    if (have_rate) {
        xrGetInstanceProcAddr(instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&request_rate);
        xrGetInstanceProcAddr(instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&enumerate_rates);
    }
    if (have_perf)
        xrGetInstanceProcAddr(instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *)&set_perf_level);

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
    /* The game runs on one CPU thread and every GL call is translated on it:
     * it is the CPU that limits the frame rate, not the GPU. Ask for the
     * CPU's sustained high clocks rather than the headset's power-saving
     * default. */
    if (set_perf_level) {
        const char *bv = getenv("LINDBERGH_CPU_BOOST");     /* test: the top clocks the runtime allows */
        int boost = bv && *bv == '1';
        XrResult r = set_perf_level(session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
                                    boost ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
        fprintf(stderr, "[xr] CPU at %s clocks: %s\n", boost ? "boost" : "sustained high", XR_SUCCEEDED(r) ? "yes" : "refused");
    }
    /* That thread is also the one that renders: registered as the
     * renderer, the runtime schedules it ahead of other work - the hitches
     * left are a frame now and then losing its core for a few ms. */
    if (have_thread) {
        PFN_xrSetAndroidApplicationThreadKHR set_thread = NULL;
        xrGetInstanceProcAddr(instance, "xrSetAndroidApplicationThreadKHR", (PFN_xrVoidFunction *)&set_thread);
        XrResult r = set_thread ? set_thread(session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, (uint32_t)gettid()) : XR_ERROR_FUNCTION_UNSUPPORTED;
        fprintf(stderr, "[xr] game thread %d registered as the renderer: %s\n", (int)gettid(), XR_SUCCEEDED(r) ? "yes" : "refused");
    }

    XrReferenceSpaceCreateInfo rc = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rc.poseInReferenceSpace.orientation.w = 1;
    XRCHECK(xrCreateReferenceSpace(session, &rc, &local_base));
    XRCHECK(xrCreateReferenceSpace(session, &rc, &local));
    if (!input_init()) return 0;

    /* Colour: an sRGB swapchain written raw, as the game's colours already
     * are sRGB; a plain RGBA8 one only if that is all there is. */
    uint32_t nf = 0;
    XRCHECK(xrEnumerateSwapchainFormats(session, 0, &nf, NULL));
    int64_t *fmts = calloc(nf ? nf : 1, sizeof *fmts);
    xrEnumerateSwapchainFormats(session, nf, &nf, fmts);
    const char *want = getenv("LINDBERGH_XR_SWAPCHAIN");     /* "rgba8": the plain one, for comparing */
    if (!(want && !strcmp(want, "rgba8")))
        for (uint32_t i = 0; i < nf; i++) if (fmts[i] == GL_SRGB8_ALPHA8) color_format = GL_SRGB8_ALPHA8;
    if (!color_format) for (uint32_t i = 0; i < nf; i++) if (fmts[i] == GL_RGBA8) color_format = GL_RGBA8;
    if (have_depth_layer) {
        static const int64_t pref[] = { GL_DEPTH24_STENCIL8, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT16 };   /* the first is what gl4es' eye layers hold: a plain blit */
        for (unsigned k = 0; k < sizeof pref / sizeof *pref && !depth_format; k++)
            for (uint32_t i = 0; i < nf; i++) if (fmts[i] == pref[k]) depth_format = pref[k];
    }
    free(fmts);
    if (!color_format) { fprintf(stderr, "[xr] no 8-bit colour swapchain\n"); return 0; }

    XrViewConfigurationView vc[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t nv = 0;
    XRCHECK(xrEnumerateViewConfigurationViews(instance, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vc));
    uint32_t w = vc[0].recommendedImageRectWidth, h = vc[0].recommendedImageRectHeight;
    running_resolution = cfg.resolution; running_width = cfg.width;
    if (cfg.width) { w = (uint32_t)cfg.width; h = (uint32_t)cfg.resolution; }   /* exactly as set */
    else {
        float f = (float)cfg.resolution / (float)(w > h ? w : h);
        w = (uint32_t)(w * f) & ~3u; h = (uint32_t)(h * f) & ~3u;
    }
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
        GLint was = 0, was_rb = 0; gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &was);
        gl.GetIntegerv(GL_RENDERBUFFER_BINDING, &was_rb);
        gl.GenRenderbuffers(1, &eye[i].depth);         /* for the gun model */
        gl.BindRenderbuffer(GL_RENDERBUFFER, eye[i].depth);
        gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)w, (GLsizei)h);
        gl.BindRenderbuffer(GL_RENDERBUFFER, (GLuint)was_rb);
        for (uint32_t j = 0; j < eye[i].n; j++) {
            gl.BindFramebuffer(GL_FRAMEBUFFER, eye[i].fb[j]);
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye[i].img[j].image, 0);
            gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, eye[i].depth);
            if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                fprintf(stderr, "[xr] swapchain framebuffer %d/%u incomplete\n", i, j);
        }
        if (depth_format) {
            XrSwapchainCreateInfo dc = ci;
            dc.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            dc.format = depth_format;
            dc.width = w / (uint32_t)depth_div; dc.height = h / (uint32_t)depth_div;
            if (XR_SUCCEEDED(xrCreateSwapchain(session, &dc, &eye[i].dchain))) {
                xrEnumerateSwapchainImages(eye[i].dchain, 0, &eye[i].dn, NULL);
                if (eye[i].dn > 8) eye[i].dn = 8;
                for (uint32_t j = 0; j < eye[i].dn; j++) eye[i].dimg[j].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
                xrEnumerateSwapchainImages(eye[i].dchain, eye[i].dn, &eye[i].dn, (XrSwapchainImageBaseHeader *)eye[i].dimg);
                gl.GenFramebuffers((GLsizei)eye[i].dn, eye[i].dfb);
                for (uint32_t j = 0; j < eye[i].dn; j++) {
                    gl.BindFramebuffer(GL_FRAMEBUFFER, eye[i].dfb[j]);
                    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, eye[i].dimg[j].image, 0);
                    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                        fprintf(stderr, "[xr] depth framebuffer %d/%u incomplete\n", i, j);
                        depth_format = 0;
                    }
                }
            } else depth_format = 0;
        }
        gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)was);
    }
    depth_on = depth_format != 0;
    fprintf(stderr, "[xr] depth to the compositor: %s\n", depth_on ? "yes" : have_depth_layer ? "no (no usable format)" : "no");
    gl.GenFramebuffers(1, &read_fb);

    overlay_ok = xo_init();
    { const char *v = getenv("LINDBERGH_AIM_PROBE"); if (!v || *v != '0') stereo_set_aim_hook(aim_probe); }   /* =0: aim at cfg.aim */
    stereo_eye_size((int)w, (int)h);
    stereo_set_screen_distance(cfg.screen * cfg.scale);
    stereo_set_hud_pitch((float)cfg.hud_pitch);
    stereo_start();
    gun_hand = cfg.left_handed ? HAND_L : HAND_R;
    fprintf(stderr, "[xr] ready: %ux%u per eye, %s swapchain, %.2f game units/m, HUD screen at %.1f m, on %s\n",
            w, h, color_format == GL_SRGB8_ALPHA8 ? "sRGB" : "RGBA8", cfg.scale, cfg.screen,
            (const char *)gl.GetString(GL_RENDERER));
    fprintf(stderr, "[xr] aim aid %s, coins %s\n", LASER_NAME[cfg.laser], cfg.auto_coins ? "automatic" : "by button");

    /* the session usually becomes ready within a few polls */
    for (int i = 0; i < 200 && !running; i++) { poll_events(); if (!running) usleep(10000); }
    return 1;
}

/* ---- per frame ---- */

static void mat4_mul(float *o, const float *a, const float *b)
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, 64);
}

static void view_matrix(XrPosef p, float m[16]);

/* LOCAL space (metres) -> eye space, for the overlay */
static void pose_view(XrPosef p, float m[16])
{
    float keep = cfg.scale;
    cfg.scale = 1;
    view_matrix(p, m);
    cfg.scale = keep;
}

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

/* LOCAL's origin is wherever the headset was last recentred, not where the
 * player's head is; the game camera sat at that origin, so a player a step
 * away from it saw the game from inside a wall. The space the game is drawn
 * in is anchored on the head instead - its place, and its facing turned
 * level - when play starts and whenever the player recentres. The screen
 * and the gun live in the same space, so the aim follows. */
static void anchor_head(XrViewLocateInfo *vl)
{
    XrView v[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    uint32_t n = 0;
    vl->space = local_base;
    if (XR_FAILED(xrLocateViews(session, vl, &vs, 2, &n, v)) ||
        !(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return;
    XrQuaternionf q = v[0].pose.orientation;
    const float back[3] = { 0, 0, -1 };
    float f[3];
    qrot(q, back, f);
    float yaw = atan2f(-f[0], -f[2]);
    XrReferenceSpaceCreateInfo rc = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rc.poseInReferenceSpace.orientation = (XrQuaternionf){ 0, sinf(yaw / 2), 0, cosf(yaw / 2) };
    rc.poseInReferenceSpace.position.x = (v[0].pose.position.x + v[1].pose.position.x) / 2;
    rc.poseInReferenceSpace.position.y = (v[0].pose.position.y + v[1].pose.position.y) / 2;
    rc.poseInReferenceSpace.position.z = (v[0].pose.position.z + v[1].pose.position.z) / 2;
    XrSpace s;
    if (XR_FAILED(xrCreateReferenceSpace(session, &rc, &s))) return;
    xrDestroySpace(local);
    local = s;
    anchor_pending = 0;
    fprintf(stderr, "[xr] game camera anchored on the head at %.2f %.2f %.2f m, facing %.0f deg\n",
            rc.poseInReferenceSpace.position.x, rc.poseInReferenceSpace.position.y,
            rc.poseInReferenceSpace.position.z, yaw * 57.2958f);
}

/* The display's frame period, and what a game frame lasts on it: as many of
 * them as make up 1/60 s (two at 120 Hz) - the game's own pacing aims at it */
static XrDuration display_period_ns;
static int phase_kick;      /* one game frame longer by half a display frame, to move off its edge */
float host_frame_period_ms(void)
{
    if (display_period_ns <= 0) return 0;
    double per = 1e9 / 60.0 / (double)display_period_ns;
    long k = (long)(per + 0.5);
    if (k < 1 || fabs(per - k) >= 0.05) return 0;
    double ms = k * display_period_ns / 1e6;
    if (phase_kick) { phase_kick = 0; ms += display_period_ns / 2e6; }
    return (float)ms;
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
    if (frame.predictedDisplayPeriod > 0) display_period_ns = frame.predictedDisplayPeriod;
    in_frame = 1;
    {   /* pacing as the display sees it: display frames per game frame */
        static XrTime prev; static unsigned hist[5], n;
        if (prev && frame.predictedDisplayPeriod > 0) {
            long k = (long)((frame.predictedDisplayTime - prev + frame.predictedDisplayPeriod / 2) / frame.predictedDisplayPeriod);
            hist[k < 1 ? 0 : k > 4 ? 4 : k]++;
            /* The game's frames begin on the wall clock at the display's own
             * rate: if they sit on the edge of a display frame, jitter shows
             * one once and the next three times, and goes on doing so. Two
             * such within half a second: one longer frame moves them to the
             * middle, where they stay. */
            static unsigned last_odd, f;
            f++;
            long want = (long)(1e9 / 60.0 / frame.predictedDisplayPeriod + 0.5);
            if (k != want) {
                if (last_odd && f - last_odd < 30) { phase_kick = 1; last_odd = 0; }
                else last_odd = f;
            }
            if (++n == 300) {
                fprintf(stderr, "[pace] display frames per game frame: 1: %u, 2: %u, 3: %u, 4+: %u\n", hist[1] + hist[0], hist[2], hist[3], hist[4]);
                memset(hist, 0, sizeof hist); n = 0;
            }
        }
        prev = frame.predictedDisplayTime;
    }

    XrViewLocateInfo vl = { XR_TYPE_VIEW_LOCATE_INFO };
    vl.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vl.displayTime = frame.predictedDisplayTime;
    if (anchor_pending) anchor_head(&vl);
    vl.space = local;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    uint32_t n = 0;
    views_ok = XR_SUCCEEDED(xrLocateViews(session, &vl, &vs, 2, &n, views)) &&
               (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    {   /* LINDBERGH_XR_FAKE_YAW=degrees (tests): the head turned left by that much, for
         * telling what follows the head from what stays in the world with
         * the headset on a desk */
        static float yaw = -1000; static unsigned from;    /* =degrees[,from frame] */
        if (yaw == -1000) { const char *v = getenv("LINDBERGH_XR_FAKE_YAW"); yaw = v ? (float)atof(v) * 0.0174533f : 0;
                            from = v && strchr(v, ',') ? (unsigned)atol(strchr(v, ',') + 1) : 0; }
        if (views_ok && yaw != 0 && g_frame >= from) {
            XrQuaternionf r = { 0, sinf(yaw / 2), 0, cosf(yaw / 2) };
            for (int i = 0; i < 2; i++) {
                XrQuaternionf q = views[i].pose.orientation;
                views[i].pose.orientation = (XrQuaternionf){
                    r.w * q.x + r.x * q.w + r.y * q.z - r.z * q.y,
                    r.w * q.y - r.x * q.z + r.y * q.w + r.z * q.x,
                    r.w * q.z + r.x * q.y - r.y * q.x + r.z * q.w,
                    r.w * q.w - r.x * q.x - r.y * q.y - r.z * q.z };
                XrVector3f p = views[i].pose.position;
                float c = cosf(yaw), sn = sinf(yaw);
                views[i].pose.position = (XrVector3f){ c * p.x + sn * p.z, p.y, -sn * p.x + c * p.z };
            }
        }
    }
    {   /* The game swung its camera round (a spider on the tailgate): its new
         * view goes where the player is looking, not where the game was
         * anchored - looking off to one side, they would face the jeep's
         * seats instead. Only when the head is turned enough to miss it. */
        static int on = -1;
        if (on < 0) { const char *v = getenv("LINDBERGH_XR_FACE_SWINGS"); on = v && *v && *v != '0'; }
        if (on && stereo_camera_swung() && views_ok) {
            const float back[3] = { 0, 0, -1 };
            float f[3];
            qrot(views[0].pose.orientation, back, f);
            float yaw = atan2f(-f[0], -f[2]) * 57.2958f;
            if (fabsf(yaw) > 15.0f) {
                anchor_pending = 1;
                fprintf(stderr, "[xr] the game swung its camera with the head %.0f deg off it: re-anchoring\n", yaw);
            }
        }
    }
    if (views_ok && getenv("LINDBERGH_POSE_LOG")) {
        XrQuaternionf q = views[0].pose.orientation;
        fprintf(stderr, "[pose] frame %u: q %.5f %.5f %.5f %.5f pos %.4f %.4f %.4f t %lld\n", g_frame, q.x, q.y, q.z, q.w,
                views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
                (long long)frame.predictedDisplayTime);
    }
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
            /* the same eye seen from the head (between the eyes, facing as
             * they do): a HUD that follows the head is placed through it */
            XrQuaternionf q = views[0].pose.orientation, iq = { -q.x, -q.y, -q.z, q.w };
            float off[3] = { views[i].pose.position.x - 0.5f * (views[0].pose.position.x + views[1].pose.position.x),
                             views[i].pose.position.y - 0.5f * (views[0].pose.position.y + views[1].pose.position.y),
                             views[i].pose.position.z - 0.5f * (views[0].pose.position.z + views[1].pose.position.z) }, rel[3];
            qrot(iq, off, rel);
            XrPosef hp = { { 0, 0, 0, 1 }, { rel[0], rel[1], rel[2] } };
            view_matrix(hp, m);
            stereo_set_eye_head(i, m);
        }
        head_pose.orientation = views[0].pose.orientation;
        head_pose.position.x = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
        head_pose.position.y = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
        head_pose.position.z = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);
        head_ok = 1;
    }
    if (state == XR_SESSION_STATE_FOCUSED) input_frame(frame.predictedDisplayTime);
    else { host_vr_input(-100000, -100000, 0, 0); xo.gun_valid = xo.beam = xo.dot = xo.flash = 0; }
}

/* This frame's world depth, copied into each eye's depth swapchain image -
 * as soon as the world is drawn (later full-screen passes into its target
 * can write over its depth), or at the frame's end if that moment never came. */
static int      depth_got;             /* the images hold this frame's depth, acquired, to be released */
static float    depth_zn, depth_zf;    /* its planes, game units */
static void depth_capture(void)
{
    if (depth_got || !depth_on || !overlay_ok || !in_frame) return;
    float zn = 0, zf = 0;
    unsigned world_depth = stereo_world_depth(&zn, &zf);
    if (!world_depth) return;
    GLint draw = 0, read = 0;
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    if (scissor) gl.Disable(GL_SCISSOR_TEST);
    int got = 0;
    for (int i = 0; i < 2; i++) {
        struct Eye *e = &eye[i];
        uint32_t k = 0;
        XrSwapchainImageAcquireInfo dai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_FAILED(xrAcquireSwapchainImage(e->dchain, &dai, &k))) break;
        got++;
        XrSwapchainImageWaitInfo dwi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        dwi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(e->dchain, &dwi);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, e->dfb[k]);
        if (world_depth != depth_checked) {
            /* a blit is a plain copy, but only between equal formats */
            depth_checked = world_depth;
            GLint was = 0, ifmt = 0, tw = 0, th = 0;
            gl.GetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &was);
            gl.BindTexture(GL_TEXTURE_2D_ARRAY, world_depth);
            gl.GetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
            gl.GetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &tw);
            gl.GetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &th);
            gl.BindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)was);
            while (gl.GetError() != GL_NO_ERROR) {}
            depth_blit = ifmt == (GLint)depth_format && tw == (GLint)e->w && th == (GLint)e->h;
            fprintf(stderr, "[xr] world depth 0x%x %dx%d, swapchain depth 0x%x: %s\n", ifmt, tw, th, (unsigned)depth_format,
                    depth_blit ? "blitted" : "copied by a shader");
            if (depth_blit && !depth_read_fb) gl.GenFramebuffers(1, &depth_read_fb);
        }
        if (depth_blit) {
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, depth_read_fb);
            gl.FramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, world_depth, 0, i);
            gl.BlitFramebuffer(0, 0, (GLint)e->w, (GLint)e->h, 0, 0, (GLint)e->w / depth_div, (GLint)e->h / depth_div,
                               GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        } else
            xo_copy_depth(world_depth, i, (int)e->w / depth_div, (int)e->h / depth_div);
    }
    if (scissor) gl.Enable(GL_SCISSOR_TEST);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    if (got == 2) { depth_got = 1; depth_zn = zn; depth_zf = zf; }
    else if (got == 1) { XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO }; xrReleaseSwapchainImage(eye[0].dchain, &ri); }
    static int log = -1;
    if (log < 0) { const char *v = getenv("LINDBERGH_DEPTH_LOG"); log = v && *v == '1'; }
    if (log && g_frame % 120 == 0) fprintf(stderr, "[xr] frame %u: depth taken %s\n", g_frame, in_frame == 2 ? "at the frame's end" : "with the world");
}

/* stereo's hook, once the world's depth is drawn: what the gun's ray reaches */
static void aim_probe(void)
{
    depth_capture();
    if (overlay_ok && probe_on) {
        float px[48][2], z[48], t[48];
        unsigned arr = 0;
        float zn = 0, zf = 0;
        int n = stereo_aim_samples(probe_o, probe_d, 48, px, z, t, &arr, &zn, &zf);
        float h = xo_probe(arr, (const float (*)[2])px, z, t, n, zn, zf);
        if (h != -2) aim_hit_t = h;
        probe_on = 0;
        static int log = -1; static unsigned k;
        if (log < 0) { const char *v = getenv("LINDBERGH_AIM_LOG"); log = v && *v == '1'; }
        if (log && ++k % 30 == 0) fprintf(stderr, "[aim] %d samples, laser reaches %.2f m\n", n, aim_hit_t / cfg.scale);
    }
}

void xr_frame_end(void)
{
    if (!in_frame) return;
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerDepthInfoKHR di[2];
    int ok = views_ok && frame.shouldRender;
    int flat = stereo_flat_frame();
    if (ok && !flat) { in_frame = 2; aim_probe(); depth_capture(); }     /* the world's moment never came */
    in_frame = 0;
    float zn = depth_zn, zf = depth_zf;
    int world_depth = depth_got, depth_released[2] = { 0, 0 };
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
            unsigned layers = stereo_eye_array();     /* multiview: both eyes in one texture */
            if (layers) gl.FramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, layers, 0, i);
            else gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stereo_eye_texture(i), 0);
            gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, e->fb[e->acquired]);
            if (flat && overlay_ok) {
                /* a menu: the game's flat picture, on the screen in front of the player */
                float hw, hh, d, place[16], view[16], proj[16], m[16], n = 0.02f, far = 1000.0f;
                stereo_screen(&hw, &hh, &d);
                memset(place, 0, sizeof place);
                place[0] = hw; place[5] = hh; place[10] = 1; place[14] = -d; place[15] = 1;
                view_matrix(views[i].pose, view);
                float tx = tanf(fov[i].angleRight), ty = tanf(fov[i].angleUp);
                memset(proj, 0, sizeof proj);
                proj[0] = 1 / tx; proj[5] = 1 / ty; proj[10] = -(far + n) / (far - n);
                proj[11] = -1; proj[14] = -2 * far * n / (far - n);
                mat4_mul(m, view, place);
                mat4_mul(m, proj, m);
                xo_draw_screen(m, layers ? layers : stereo_eye_texture(0), layers != 0, (int)e->w, (int)e->h);
            } else
                gl.BlitFramebuffer(0, 0, (GLint)e->w, (GLint)e->h, 0, 0, (GLint)e->w, (GLint)e->h,
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
            static int colour_check = -1;   /* LINDBERGH_COLOUR_CHECK=1: reading back stalls the GPU, a hitch each time */
            if (colour_check < 0) { const char *v = getenv("LINDBERGH_COLOUR_CHECK"); colour_check = v && *v == '1'; }
            if (colour_check && i == 0 && g_frame % 600 == 300) {
                /* the game's centre pixel, and what the copy stored in the
                 * swapchain: equal unless the copy encoded it as sRGB again */
                unsigned char a[4] = { 0 }, b[4] = { 0 };
                GLint cx = (GLint)e->w / 2, cy = (GLint)e->h / 2;
                gl.ReadPixels(cx, cy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);
                gl.BindFramebuffer(GL_READ_FRAMEBUFFER, e->fb[e->acquired]);
                gl.ReadPixels(cx, cy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
                gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
                static int told;
                if (!told++) {
                    const char *x = (const char *)gl.GetString(GL_EXTENSIONS);
                    fprintf(stderr, "[xr] colour: swapchain %s, sRGB write control %s\n",
                            color_format == GL_SRGB8_ALPHA8 ? "sRGB" : "RGBA8",
                            x && strstr(x, "GL_EXT_sRGB_write_control") ? "yes" : "no");
                }
                fprintf(stderr, "[xr] colour check frame %u: game %u %u %u, stored %u %u %u\n",
                        g_frame, a[0], a[1], a[2], b[0], b[1], b[2]);
            }
            if (fl_on < 0) { const char *v = getenv("LINDBERGH_FLASH_LOG"); fl_on = v && *v && *v != '0'; }
            if (fl_on) { fl_sample(i, (GLint)e->w, (GLint)e->h); gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, e->fb[e->acquired]); }
            if (overlay_ok) {
                /* the gun and laser, seen from this eye */
                float view[16], proj[16], vp[16], n = 0.02f, far = 100.0f;
                pose_view(views[i].pose, view);
                float tx = tanf(fov[i].angleRight), ty = tanf(fov[i].angleUp);
                memset(proj, 0, sizeof proj);
                proj[0] = 1 / tx; proj[5] = 1 / ty; proj[10] = -(far + n) / (far - n);
                proj[11] = -1; proj[14] = -2 * far * n / (far - n);
                mat4_mul(vp, proj, view);
                float eye_pos[3] = { views[i].pose.position.x, views[i].pose.position.y, views[i].pose.position.z };
                xo_draw(&xo, vp, eye_pos, (int)e->w, (int)e->h);
                if (menu.open) {
                    float pm[16], m[16];
                    memset(pm, 0, sizeof pm);
                    for (int k = 0; k < 3; k++) {
                        pm[k] = menu.right[k] * menu.hw; pm[4 + k] = menu.up[k] * menu.hh;
                        pm[8 + k] = menu.n[k]; pm[12 + k] = menu.c[k];
                    }
                    pm[15] = 1;
                    mat4_mul(m, vp, pm);
                    xo_draw_panel(m, (int)e->w, (int)e->h);
                    xo_draw(&xo, vp, eye_pos, (int)e->w, (int)e->h);   /* the gun and laser over the panel */
                }
            }
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(e->chain, &ri);

            pv[i] = (XrCompositionLayerProjectionView){ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            pv[i].pose = views[i].pose;
            pv[i].fov = fov[i];
            pv[i].subImage.swapchain = e->chain;
            pv[i].subImage.imageRect.extent.width = (int32_t)e->w;
            pv[i].subImage.imageRect.extent.height = (int32_t)e->h;
            if (world_depth) {
                /* the world's depth for this eye, and where its planes lie in metres */
                XrSwapchainImageReleaseInfo dri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(e->dchain, &dri);
                depth_released[i] = 1;
                di[i] = (XrCompositionLayerDepthInfoKHR){ XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR };
                di[i].subImage.swapchain = e->dchain;
                di[i].subImage.imageRect.extent.width = (int32_t)e->w / depth_div;
                di[i].subImage.imageRect.extent.height = (int32_t)e->h / depth_div;
                di[i].minDepth = 0; di[i].maxDepth = 1;
                di[i].nearZ = zn / cfg.scale; di[i].farZ = zf / cfg.scale;
                pv[i].next = &di[i];
            }
        }
        if (!world_depth) pv[0].next = pv[1].next = NULL;      /* both eyes or neither */
        {   static int told = -1;
            if (ok && told != (world_depth != 0)) {
                told = world_depth != 0;
                fprintf(stderr, "[xr] frame %u: depth layer %s (near %.3f m, far %.1f m)\n", g_frame,
                        told ? "on" : "off", zn / cfg.scale, zf / cfg.scale);
            }
        }
        if (fl_on > 0 && ok) fl_frame();
        if (color_format == GL_SRGB8_ALPHA8) gl.Enable(GL_FRAMEBUFFER_SRGB_EXT);
        if (scissor) gl.Enable(GL_SCISSOR_TEST);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    }
    for (int i = 0; i < 2 && depth_got; i++)      /* taken, but the frame shows nothing (or not this eye) */
        if (!depth_released[i]) { XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO }; xrReleaseSwapchainImage(eye[i].dchain, &ri); }
    if (!(depth_released[0] && depth_released[1])) pv[0].next = pv[1].next = NULL;
    depth_got = 0;
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = local; layer.viewCount = 2; layer.views = pv;
    const XrCompositionLayerBaseHeader *layers[1] = { (XrCompositionLayerBaseHeader *)&layer };
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime = frame.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = ok ? 1 : 0; ei.layers = layers;
    static int last_ok = -1;
    if (ok != last_ok)
        fprintf(stderr, "[xr] frame %u: %s (views %d, should render %d, state %d)\n", g_frame,
                ok ? "showing the game" : "nothing to show", views_ok, (int)frame.shouldRender, (int)state);
    last_ok = ok;
    XrResult er = xrEndFrame(session, &ei);
    static unsigned bad;
    if (XR_FAILED(er) && bad++ < 20) fprintf(stderr, "[xr] frame %u: xrEndFrame failed %d\n", g_frame, (int)er);
}

int xr_running(void) { return running; }

#endif /* __ANDROID__ */
