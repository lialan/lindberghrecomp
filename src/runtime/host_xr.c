#include "display_cadence.h"
#include "xr_settings.h"
#include "xr_controls.h"
#include "xr_actions.h"
#include "xr_panel.h"
#include "stereo_projection.h"
/*
 * host_xr.c - the headset, through OpenXR (Android and Windows PC VR).
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
 *   resolution 2208     each eye's image height, in pixels (default: the
 *                       panel's - Quest 3 2064x2208, Steam Frame 2160x2160)
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
#if defined(LINDBERGH_OPENXR)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL.h>
#ifdef __ANDROID__
#include <dlfcn.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_GL_IMAGE XrSwapchainImageOpenGLESKHR
#define XR_GL_IMAGE_TYPE XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR
#else
#include <windows.h>
#include <GL/glew.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_GL_IMAGE XrSwapchainImageOpenGLKHR
#define XR_GL_IMAGE_TYPE XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "lindbergh_rt.h"
#include "game_profile.h"
#include "stereo.h"
#include "xr_overlay.h"
#include "xr_menu.h"

/* a debug switch, looked up once */
#define ENV_ON(name) ({ static int _on = -1; if (_on < 0) _on = getenv(name) != NULL; _on; })

/* host_sdl.c: the gun and the cabinet's buttons, from the controllers */
void host_vr_input(int gun_x, int gun_y, unsigned held, int coin);
void host_vr_input2(int on, int gun2_x, int gun2_y);

#ifndef GL_TEXTURE_INTERNAL_FORMAT
#define GL_TEXTURE_WIDTH            0x1000
#define GL_TEXTURE_HEIGHT           0x1001
#define GL_TEXTURE_INTERNAL_FORMAT  0x1003
#endif

/* ---- raw GLES, past gl4es ---- */
static struct {
    void (APIENTRY *BindFramebuffer)(GLenum, GLuint);
    void (APIENTRY *GenFramebuffers)(GLsizei, GLuint *);
    void (APIENTRY *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (APIENTRY *CheckFramebufferStatus)(GLenum);
    void (APIENTRY *BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void (APIENTRY *GetIntegerv)(GLenum, GLint *);
    GLboolean (APIENTRY *IsEnabled)(GLenum);
    void (APIENTRY *Enable)(GLenum);
    void (APIENTRY *Disable)(GLenum);
    const GLubyte *(APIENTRY *GetString)(GLenum);
    void (APIENTRY *GenRenderbuffers)(GLsizei, GLuint *);
    void (APIENTRY *BindRenderbuffer)(GLenum, GLuint);
    void (APIENTRY *RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (APIENTRY *FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    void (APIENTRY *FramebufferTextureLayer)(GLenum, GLenum, GLuint, GLint, GLint);
    void (APIENTRY *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    GLenum (APIENTRY *GetError)(void);
    void (APIENTRY *BindTexture)(GLenum, GLuint);
    void (APIENTRY *GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
    void (APIENTRY *Finish)(void);
} gl;

static int gl_load(void)
{
#ifdef __ANDROID__
    void *lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return 0;
#define L(n) if (!(*(void **)&gl.n = dlsym(lib, "gl" #n))) { fprintf(stderr, "[xr] no gl" #n "\n"); return 0; }
#else
#define L(n) if (!(*(void **)&gl.n = SDL_GL_GetProcAddress("gl" #n))) { fprintf(stderr, "[xr] no gl" #n "\n"); return 0; }
#endif
    L(BindFramebuffer) L(GenFramebuffers) L(FramebufferTexture2D) L(CheckFramebufferStatus)
    L(BlitFramebuffer) L(GetIntegerv) L(IsEnabled) L(Enable) L(Disable) L(GetString)
    L(GenRenderbuffers) L(BindRenderbuffer) L(RenderbufferStorage) L(FramebufferRenderbuffer)
    L(FramebufferTextureLayer) L(ReadPixels) L(GetError) L(BindTexture) L(GetTexLevelParameteriv) L(Finish)
#undef L
    return 1;
}

#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9

/* ---- settings ---- */
static const char *const LASER_NAME[] = { "laser", "dot", "off" };

static const char *const SIGHT_NAME[] = { "none", "reddot", "holo" };

static LgXrSettings cfg = LG_XR_SETTINGS_DEFAULT;
static void cfg_save(void) { xr_settings_save(&cfg); }
static void cfg_load(void) { xr_settings_load(&cfg); }

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
    XR_GL_IMAGE img[8];
    GLuint      fb[8], depth;
    uint32_t    acquired;
    XrSwapchain dchain;                /* the world's depth, for the compositor */
    uint32_t    dn;
    XR_GL_IMAGE dimg[8];
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
static XoFrame xo, xo2;                /* the gun and laser for this frame; xo2: player 2's (dual wielding) */
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
static LgXrActions actions;
#define aim_space actions.aim_space
#define hand_path actions.hand_path
#define a_trigger actions.action[LG_XR_TRIGGER]
#define a_grip actions.action[LG_XR_GRIP]
#define a_a actions.action[LG_XR_LOWER]
#define a_b actions.action[LG_XR_UPPER]
#define a_stick actions.action[LG_XR_STICK]
#define a_menu actions.action[LG_XR_MENU]
static LgXrControls controls=LG_XR_CONTROLS_DEFAULT;
#define gun_hand controls.gun_hand
static int prev[2][8];                 /* last state of each button, per hand */

static int input_init(void) { XRCHECK(lg_xr_actions_create(&actions,instance,session));return 1; }
static float get_float(XrAction a,int h) {return lg_xr_action_float(&actions,a,h);}
static int get_bool(XrAction a,int h) {return lg_xr_action_bool(&actions,a,h);}
static void rumble(int h,float amp,int ms) {lg_xr_actions_rumble(&actions,h,amp,ms);}

/* Rotate v by quaternion q. */
static void qrot(XrQuaternionf q, const float v[3], float out[3])
{
    const float quaternion[]={q.x,q.y,q.z,q.w};
    lg_stereo_rotate(quaternion,v,out);
}

/* Cabinet bits, as in lindbergh_rt.h */
/* LINDBERGH_XR_FAKE_GUN=1: a stand-in for a controller (fake_pose); =2 two of them */
static int fake_gun(void)
{
    static int fake = -1;
    if (fake < 0) { const char *v = getenv("LINDBERGH_XR_FAKE_GUN"); fake = v && *v && *v != '0' ? (atoi(v) > 1 ? 2 : 1) : 0; }
    return fake;
}

/* ---- the settings panel ----
 * The left controller's menu button opens it to the player's left, turned
 * towards them, the game's screen still in view (the arcade's own Test menu
 * is worked from here too). Point the gun at a row and pull the trigger;
 * the game gets no input meanwhile. */
static XpPanel menu;
static float rates[16];
static int running_resolution, running_width;
/* the gun's ray this frame (game units), what it reached two frames ago */
static float probe_o[2][3], probe_d[2][3], aim_hit_t[2] = { -1, -1 }, aim_reach[2];   /* per player */
static XrPosef head_pose;            /* between the eyes, facing as they do: where a head-locked HUD hangs */
static int head_ok;
static int aim_on_box[2];           /* this frame's aim went to a box on the screen */
static int probe_on[2];
static XrTime reload_t;            /* when the player last reloaded */
static void aim_probe(void);   /* the eye size this run was set up with */
static int nrates;

static void menu_items(XmItem *it)
{
    xp_items(&menu,&cfg,gun_hand,stereo_hud_is_head_locked(),running_resolution,running_width,it);
}

/* The arcade's reset: the game starts again from its boot, as a new process
 * (the activity's restartGame, which needs a game whose activity has it) */
static void reset_game(void)
{
#ifdef __ANDROID__
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
#else
    STARTUPINFOA startup = { .cb = sizeof startup };
    PROCESS_INFORMATION process;
    char *command = _strdup(GetCommandLineA());
    if (command && CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        free(command);
        exit(0);
    }
    free(command);
    fprintf(stderr, "[xr] restart failed: %lu\n", GetLastError());
#endif
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
    XmItem it[XP_COUNT];
    char title[80];
    menu_items(it);
    snprintf(title, sizeof title, "%s - settings", app_title());
    xm_paint(img, title, it, XP_COUNT, menu.hover,
             "Point and pull the trigger. Menu button: close.");
    xo_panel_upload(img, XM_W, XM_H);
    menu.dirty = 0;
}

static void menu_place(void)
{
    float head[3],q[]={views[0].pose.orientation.x,views[0].pose.orientation.y,views[0].pose.orientation.z,views[0].pose.orientation.w};
    for(int i=0;i<3;++i)head[i]=.5f*((&views[0].pose.position.x)[i]+(&views[1].pose.position.x)[i]);
    xp_place(&menu,head,q);
}

/* ---- the frame rate readout (cfg.show_stats) ----
 * Frames shown in each half second, repainted when the number changes; drawn
 * low and to the left of where the head looks, a metre out, in both eyes. */
static struct { float c[3], right[3], up[3], n[3], hw, hh; int shown; } fpsl = { .shown = -1 };
static void fps_update(XrTime now)
{
    static XrTime t0; static int frames;
    frames++;
    if (!t0) t0 = now;
    if (now - t0 >= 500000000) {
        int fps = (int)(frames * 1e9 / (double)(now - t0) + 0.5);
        t0 = now; frames = 0;
        if (fps != fpsl.shown) {
            static unsigned char img[XM_LABEL_W * XM_LABEL_H * 4];
            char s[16];
            snprintf(s, sizeof s, "%d FPS", fps);
            xm_paint_label(img, s);
            xo_label_upload(img, XM_LABEL_W, XM_LABEL_H);
            fpsl.shown = fps;
        }
    }
    float head[3];
    static const float ax[3] = { 1, 0, 0 }, ay[3] = { 0, 1, 0 }, az[3] = { 0, 0, 1 }, at[3] = { -0.30f, -0.26f, -1.0f };
    float off[3];
    for (int i = 0; i < 3; i++) head[i] = 0.5f * ((&views[0].pose.position.x)[i] + (&views[1].pose.position.x)[i]);
    qrot(views[0].pose.orientation, ax, fpsl.right);
    qrot(views[0].pose.orientation, ay, fpsl.up);
    qrot(views[0].pose.orientation, az, fpsl.n);
    qrot(views[0].pose.orientation, at, off);
    for (int i = 0; i < 3; i++) fpsl.c[i] = head[i] + off[i];
    fpsl.hw = 0.07f; fpsl.hh = fpsl.hw * XM_LABEL_H / XM_LABEL_W;
}

/* where the ray meets the panel: 0..1 from its top left; 0 if it misses */
static int menu_hit(const float p[3],const float d[3],float *u,float *v,float hit[3])
{return xp_hit(&menu,p,d,u,v,hit);}

static void menu_choose(int item)
{
    const unsigned effects=xp_choose(&menu,&cfg,&gun_hand,item,rates,nrates);
    if(effects&XP_CROSSHAIR_CHANGED)stereo_show_game_sight(cfg.crosshair && !game_profile()->joystick);
    if(effects&XP_SCREEN_CHANGED)stereo_set_screen_distance(cfg.screen*cfg.scale);
    if(effects&XP_HUD_CHANGED)stereo_set_hud_pitch((float)cfg.hud_pitch);
    if((effects&XP_RATE_CHANGED)&&request_rate)request_rate(session,(float)cfg.refresh);
    if(effects&XP_RECENTER)anchor_pending=1;
    if(effects&XP_SAVE)cfg_save();
    if(effects&XP_RESTART)reset_game();
}

/* the stand-in for a controller (LINDBERGH_XR_FAKE_GUN, for a headset or
 * emulator without them): a gun at the hip sweeping across the screen,
 * firing in half-second bursts, and for one second in six pointed at the
 * floor with the trigger held - the arcade's reload. =2: a second one at the
 * left hip, as player 2, sweeping the other way. */
static void fake_pose(int pl, XrTime t, XrSpaceLocation *loc, int *trig)
{
    double s = (double)t / 1e9 + (pl ? 3.0 : 0.0);
    float yaw = (float)(0.25 * sin(s * 0.7)), pitch = (float)(0.05 + 0.08 * sin(s * 1.1));
    if (pl) yaw = -yaw;
    static int fixed = -1; static float fyaw, fpitch;   /* LINDBERGH_XR_FAKE_AIM=yaw,pitch (radians): held still */
    if (fixed < 0) { const char *v = getenv("LINDBERGH_XR_FAKE_AIM"); fixed = v && sscanf(v, "%f,%f", &fyaw, &fpitch) == 2; }
    if (fixed && !pl) { yaw = fyaw; pitch = fpitch; }
    int reload = fmod(s, 6.0) >= 5.0;
    if (reload) pitch = -0.9f;
    float cy = cosf(yaw / 2), sy = sinf(yaw / 2), cp = cosf(pitch / 2), sp = sinf(pitch / 2);
    loc->pose.orientation = (XrQuaternionf){ sp * cy, cp * sy, -sp * sy, cp * cy };
    loc->pose.position = (XrVector3f){ pl ? -0.16f : 0.16f, -0.22f, -0.28f };
    static int ads = -1;            /* LINDBERGH_XR_FAKE_ADS=1: shouldered, looking through the sight */
    if (ads < 0) { const char *v = getenv("LINDBERGH_XR_FAKE_ADS"); ads = v && *v == '1'; }
    if (ads && !pl) {
        loc->pose.position = (XrVector3f){ 0.0f, -0.080f, -0.16f };
        yaw *= 0.15f; pitch = 0.0f; reload = 0;
        cy = cosf(yaw / 2); sy = sinf(yaw / 2); cp = 1; sp = 0;
        loc->pose.orientation = (XrQuaternionf){ sp * cy, cp * sy, -sp * sy, cp * cy };
    }
    *trig = reload || fmod(s, 1.0) < 0.5;
}

static int fake_menu = -1;          /* LINDBERGH_XR_FAKE_MENU=1 with the stand-in gun: the panel open, no clicks */

/* One gun: hand h's controller (or the stand-in) as player pl. Where the
 * game is to be told it points (gx, gy: off the screen when not), what the
 * overlay draws for it, and - the panel open - which row it points at and
 * its clicks. Returns whether it points at the game's screen. */
static int aim_gun(int h, int pl, XrTime t, int *trig, XoFrame *x, float *gx_, float *gy_)
{
    float gx = -100000, gy = -100000;
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    int aimed = 0;
    x->gun_valid = 0;
    x->p2 = pl;
    int located = XR_SUCCEEDED(xrLocateSpace(aim_space[h], local, t, &loc)) &&
                  (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                  (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    if (!located && fake_gun() > pl) { fake_pose(pl, t, &loc, &trig[h]); located = 1; }
    stereo_set_gun_ray(pl, NULL, NULL, 0);     /* until the gun is found this frame */
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
        float xs_, ys_, ah[3] = { 0, 0, 0 }, at, ax[3];
        int flat = stereo_flat_frame() || stereo_cqc_active();
        if (flat) at = d[2] < -1e-4f ? (-cfg.screen * cfg.scale - o[2]) / d[2] : -1;
        else if (aim_hit_t[pl] > 0) at = aim_hit_t[pl];
        else at = d[2] < -1e-4f ? (-cfg.aim * cfg.scale - o[2]) / d[2] : -1;
        for (int i = 0; i < 3; i++) ax[i] = o[i] + d[i] * at;
        memcpy(probe_o[pl], o, sizeof probe_o[pl]); memcpy(probe_d[pl], d, sizeof probe_d[pl]); probe_on[pl] = 1;
        stereo_set_gun_ray(pl, o, d, !menu.open);
        int gw, gh;
        stereo_game_size(&gw, &gh);
        if (at > 0 && stereo_ray_to_screen(ah, ax, &xs_, &ys_) && xs_ >= 0 && xs_ < gw && ys_ >= 0 && ys_ < gh) {
            gx = xs_; gy = ys_; aimed = 1; aim_reach[pl] = at / cfg.scale;
        }
        /* A box the game hangs in front of its camera - the path choice's
         * views - is aimed at where the laser crosses it, not where it meets
         * the world behind: from the hand those are degrees apart */
        aim_on_box[pl] = 0;
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
            if (!flat && stereo_aim_box(head ? ho : o, head ? hd : d, head, &xs, &ys, &ts)) { gx = xs; gy = ys; aimed = 1; aim_reach[pl] = ts / cfg.scale; aim_on_box[pl] = 1; }
            /* or every shot where the laser crosses the HUD's screen: the game
             * draws its hit there, on its 2D (The House of the Dead 4 Special) */
            else if (!flat && stereo_aims_on_hud()) {
                /* the laser still runs on to what it meets; only the game's
                 * point moves (a game firing from the gun's own ray hits by that) */
                int was = aimed; float reach = aim_reach[pl];
                aimed = 0; gx = gy = -100000;
                if (stereo_ray_to_hud(head ? ho : o, head ? hd : d, head, &xs, &ys, &ts) && xs >= 0 && xs < gw && ys >= 0 && ys < gh) {
                    gx = xs; gy = ys; aimed = 1; aim_reach[pl] = was ? reach : ts / cfg.scale;
                }
            }
        }
        /* the overlay: the gun on the aim pose, the laser to the game's screen */
        x->gun_valid = 1;
        x->gun_q[0] = loc.pose.orientation.x; x->gun_q[1] = loc.pose.orientation.y;
        x->gun_q[2] = loc.pose.orientation.z; x->gun_q[3] = loc.pose.orientation.w;
        memcpy(x->gun_p, p, sizeof p);
        float m[3];
        qrot(loc.pose.orientation, tip, m);
        for (int i = 0; i < 3; i++) x->muzzle[i] = p[i] + m[i];
        float reach = 5.0f;
        if (aimed) reach = aim_reach[pl];
        for (int i = 0; i < 3; i++) x->end[i] = p[i] + d[i] * reach;
        x->beam = cfg.laser == LASER_ON;
        x->dot = aimed && cfg.laser != LASER_OFF;
        if (menu.open) {        /* the laser points at the panel; the game is not aimed at */
            float u, v, hit[3];
            int on = menu_hit(p, d, &u, &v, hit);
            int row = on ? xm_row_at(u, v, XP_COUNT) : -1;
            /* the panel's highlight follows the gun last on it */
            if (on && row != menu.hover) { menu.hover = row; menu.dirty = 1; }
            if (!on && pl == 0 && !cfg.dual && menu.hover != -1) { menu.hover = -1; menu.dirty = 1; }
            if (on) memcpy(x->end, hit, sizeof hit);
            x->beam = 1; x->dot = on;
            if (trig[h] && !prev[h][0])     /* every pull on the panel, for a click that seems lost */
                fprintf(stderr, "[xr] panel trigger: %s hand, %s at %.2f,%.2f, row %d\n", h == HAND_L ? "left" : "right",
                        on ? "on the panel" : "off it", on ? u : -1, on ? v : -1, row);
            if (trig[h] && !prev[h][0] && row >= 0 && !fake_menu) { menu_choose(row); rumble(h, 0.25f, 15); }
            aimed = 0; gx = gy = -100000;
        }
    }
    x->sight = cfg.show_gun ? cfg.sight : XO_SIGHT_NONE;
    x->show_gun = cfg.show_gun;
    x->gun_model = game_profile()->vr.gun_model;
    *gx_ = gx; *gy_ = gy;
    return aimed;
}

/* Firing: a kick, a flash and a pulse ten times a second while held on the
 * screen - the cabinet gun's recoil. Returns whether the gun fires. */
static int gun_fire(int pl, int h, XrTime t, int trig, int aimed, XoFrame *x)
{
    static XrTime fire_start[2];
    static float last_phase[2] = { 1.0f, 1.0f };
    int firing = trig && aimed && !menu.open;
    if (firing && !prev[h][0]) fire_start[pl] = t;
    float phase = firing ? fmodf((float)((t - fire_start[pl]) / 1000000) / 100.0f, 1.0f) : 1.0f;
    x->recoil = firing ? 0.018f * (1.0f - phase) * (1.0f - phase) : 0;
    x->flash = firing && phase < 0.35f;
    if (firing && (phase < last_phase[pl])) rumble(h, 0.7f, 35);
    last_phase[pl] = phase;
    return firing;
}

static void input_frame(XrTime t)
{
    if(XR_FAILED(lg_xr_actions_sync(&actions)))return;

    /* Dual wielding (cfg.dual): each hand its own gun - gun_hand player 1,
     * the other player 2. Otherwise one gun, in the hand last pulled with. */
    int dual = cfg.dual;
    int trig[2], grip[2], btn_a[2], btn_b[2], stick[2];
    for (int h = 0; h < 2; h++) {
        trig[h] = get_float(a_trigger, h) > 0.55f;
        grip[h] = get_float(a_grip, h) > 0.6f;
        btn_a[h] = get_bool(a_a, h); btn_b[h] = get_bool(a_b, h); stick[h] = get_bool(a_stick, h);
    }
    const int lever_cab = game_profile()->joystick;   /* the right hand holds the buttons; nothing is a gun */
    if (lever_cab) gun_hand = HAND_R; else lg_xr_controls_select(&controls,trig,dual);   /* gun_hand is controls.gun_hand */
    int menu_btn = get_bool(a_menu, HAND_L);
    if (fake_menu < 0) { const char *v = getenv("LINDBERGH_XR_FAKE_MENU"); fake_menu = v && *v == '1'; }
    if (fake_menu && !menu.open && views_ok) { menu.open = 1; menu_place(); menu.hover = -1; menu.dirty = 1; }
    if (xp_menu_button(&menu,menu_btn,prev[HAND_L][6])) {
        fprintf(stderr, "[xr] settings panel %s\n", menu.open ? "open" : "closed");
        if (menu.open) { menu_place(); menu.hover = -1; menu.dirty = 1; }
    }
    int g = gun_hand, off = 1 - g;

    float gx = -100000, gy = -100000, g2x = -100000, g2y = -100000;
    int aimed = 0, aimed2 = 0;
    /* a lever cabinet (Virtua Tennis 3) has no gun: no aim ray (and no
     * depth hit-test for it), no gun model, laser, flash or recoil */
    if (lever_cab) { xo.gun_valid = xo.beam = xo.dot = xo.flash = 0; stereo_set_gun_ray(0, NULL, NULL, 0); }
    else aimed = aim_gun(g, 0, t, trig, &xo, &gx, &gy);
    if (dual && !lever_cab) aimed2 = aim_gun(off, 1, t, trig, &xo2, &g2x, &g2y);
    else { xo2.gun_valid = xo2.beam = xo2.dot = xo2.flash = 0; stereo_set_gun_ray(1, NULL, NULL, 0); }
    if (menu.open && menu.dirty) menu_paint();

    int firing=lever_cab?0:gun_fire(0,g,t,trig[g],aimed,&xo);
    int firing2=dual&&gun_fire(1,off,t,trig[off],aimed2,&xo2);

    /* the other hand's upper button (Y for the right-handed), held for a
     * second, opens and closes the settings panel too - not when that hand
     * is player 2's gun, whose buttons they are */
    static XrTime panel_held;
    if(xp_hold_button(&menu,btn_b[off],prev[off][3],dual,t,&panel_held)) {
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

    for (int pl = 0; pl < (dual ? 2 : 1); pl++) {      /* each pull: where the game is told it went */
        int h = pl ? off : g, on = pl ? aimed2 : aimed;
        if (trig[h] && !prev[h][0] && !menu.open)
            fprintf(stderr, "[aim] frame %u: P%d pull at %d,%d%s (reach %.2f m, depth %s, flat %d%s)\n", g_frame, pl + 1,
                    (int)(pl ? g2x : gx), (int)(pl ? g2y : gy), on ? "" : " - off the screen", aim_reach[pl],
                    aim_hit_t[pl] > 0 ? "read" : "assumed", stereo_flat_frame(), aim_on_box[pl] ? ", on a box" : "");
    }

    LgXrControlFrame frame={0};frame.time=t;frame.dual=dual;frame.auto_coins=cfg.auto_coins;frame.blocked=menu.open;
    for(int h=0;h<2;++h) {
        frame.trigger[h]=trig[h];frame.grip[h]=grip[h];frame.lower[h]=btn_a[h];frame.upper[h]=btn_b[h];frame.stick[h]=stick[h];
    }
    frame.aimed[0]=aimed;frame.aimed[1]=aimed2;
    frame.joystick=lever_cab;
    for(int h=0;h<2;++h)lg_xr_action_vec2(&actions,actions.action[LG_XR_THUMB],h,&frame.thumb_x[h],&frame.thumb_y[h]);
    frame.fire_supplied=1;frame.firing[0]=firing;frame.firing[1]=firing2;
    frame.start_override[0]=fake_gun()&&fmod((double)t/1e9,10.0)<.2;
    frame.start_override[1]=dual&&fake_gun()>1&&fmod((double)t/1e9+5.0,10.0)<.2;
    const LgXrCabinet cabinet=lg_xr_controls_update(&controls,&frame);
    unsigned held=cabinet.held;
    if(held&CAB_P1_RELOAD)reload_t=t;
    stereo_game_gun_reloading(reload_t&&t-reload_t<1500000000);
    /* the off hand's lower button saves the eyes (a debugging aid) - not
     * for a lever cabinet, where it is a shot button */
    if(!dual&&!lever_cab&&btn_a[off]&&!prev[off][2]&&!menu.open){stereo_capture();rumble(off,.4f,30);}
    if(menu.test_frames){--menu.test_frames;held|=CAB_TEST;}
    if(menu.service_frames){--menu.service_frames;held|=CAB_SERVICE;}
    int coin=menu.coin+cabinet.coins;menu.coin=0;
    {   /* LINDBERGH_INPUT_LOG=1: each change of the cabinet's buttons, and why
         * (the controllers' raw state), for inputs nobody remembers making */
        static int on=-1;static unsigned last=~0u;
        if(on<0){const char*v=getenv("LINDBERGH_INPUT_LOG");on=v&&*v=='1';}
        if(on&&(held!=last||coin)){
            fprintf(stderr,"[input] frame %u: held %#x coin %d | trig %d,%d grip %d,%d a %d,%d b %d,%d stick %d,%d thumb %.2f,%.2f %.2f,%.2f menu %d\n",
                    g_frame,held,coin,trig[0],trig[1],grip[0],grip[1],btn_a[0],btn_a[1],btn_b[0],btn_b[1],stick[0],stick[1],
                    frame.thumb_x[0],frame.thumb_y[0],frame.thumb_x[1],frame.thumb_y[1],menu.open);
            last=held;
        }
    }
    stereo_set_gun((int)gx, (int)gy);
    stereo_set_gun2((int)g2x, (int)g2y);
    host_vr_input((int)gx, (int)gy, held, coin);
    host_vr_input2(dual, (int)g2x, (int)g2y);

    if (!lever_cab && trig[g] && !aimed && !prev[g][0] && !menu.open) rumble(g, 0.3f, 40);
    if (dual && trig[off] && !aimed2 && !prev[off][0] && !menu.open) rumble(off, 0.3f, 40);
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
    host_display_timing_reset();
    if (!gl_load()) { fprintf(stderr, "[xr] cannot load GLES\n"); return 0; }
    cfg_load();
    stereo_show_game_sight(cfg.crosshair && !game_profile()->joystick);   /* no sight on a lever cabinet */

#ifdef __ANDROID__
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
    uint32_t next = 2;
#else
    const char *ext[6] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    uint32_t next = 1;
#endif
    uint32_t n = 0;
    XRCHECK(xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL));
    XrExtensionProperties *props = calloc(n ? n : 1, sizeof *props);
    if (!props) return 0;
    for (uint32_t i = 0; i < n; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    XrResult enumerated = xrEnumerateInstanceExtensionProperties(NULL, n, &n, props);
    if (XR_FAILED(enumerated)) { free(props); XRCHECK(enumerated); }
    int have_rate = 0, have_perf = 0;
#ifdef __ANDROID__
    int have_thread = 0;
#endif
    int have_graphics = 0;
    have_depth_layer = 0;
    for (uint32_t i = 0; i < n; i++) {
#ifdef __ANDROID__
        if (!strcmp(props[i].extensionName, XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME)) have_thread = 1;
        if (!strcmp(props[i].extensionName, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) have_graphics = 1;
#else
        if (!strcmp(props[i].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) have_graphics = 1;
#endif
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
    if (!have_graphics) { fprintf(stderr, "[xr] active runtime lacks required OpenGL graphics extension\n"); return 0; }
    if (have_rate) ext[next++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    if (have_perf) ext[next++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
#ifdef __ANDROID__
    if (have_thread) ext[next++] = XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME;
#endif
    if (have_depth_layer) ext[next++] = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;

    XrInstanceCreateInfo ic = { XR_TYPE_INSTANCE_CREATE_INFO };
#ifdef __ANDROID__
    XrInstanceCreateInfoAndroidKHR ai = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    ai.applicationVM = vm; ai.applicationActivity = activity;
    ic.next = &ai;
#endif
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

#ifdef __ANDROID__
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
#else
    PFN_xrGetOpenGLGraphicsRequirementsKHR req_fn = NULL;
    XRCHECK(xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req_fn));
    XrGraphicsRequirementsOpenGLKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    XRCHECK(req_fn(instance, sys, &req));
    GLint major = 0, minor = 0;
    gl.GetIntegerv(GL_MAJOR_VERSION, &major); gl.GetIntegerv(GL_MINOR_VERSION, &minor);
    XrVersion version = XR_MAKE_VERSION(major, minor, 0);
    if (version < req.minApiVersionSupported || version > req.maxApiVersionSupported) {
        fprintf(stderr, "[xr] GL %d.%d outside runtime graphics requirements\n", major, minor);
        return 0;
    }
    XrGraphicsBindingOpenGLWin32KHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    gb.hDC = wglGetCurrentDC(); gb.hGLRC = wglGetCurrentContext();
#endif
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
#ifdef __ANDROID__
    if (have_thread) {
        PFN_xrSetAndroidApplicationThreadKHR set_thread = NULL;
        xrGetInstanceProcAddr(instance, "xrSetAndroidApplicationThreadKHR", (PFN_xrVoidFunction *)&set_thread);
        XrResult r = set_thread ? set_thread(session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, (uint32_t)gettid()) : XR_ERROR_FUNCTION_UNSUPPORTED;
        fprintf(stderr, "[xr] game thread %d registered as the renderer: %s\n", (int)gettid(), XR_SUCCEEDED(r) ? "yes" : "refused");
    }

#endif

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
        for (uint32_t j = 0; j < eye[i].n; j++) eye[i].img[j].type = XR_GL_IMAGE_TYPE;
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
                for (uint32_t j = 0; j < eye[i].dn; j++) eye[i].dimg[j].type = XR_GL_IMAGE_TYPE;
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
    stereo_set_aim_hook(aim_probe);
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
    const float q[]={p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w};
    const float position[]={p.position.x,p.position.y,p.position.z};
    lg_stereo_pose_view(q,position,cfg.scale,m);
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

void xr_frame_begin(void)
{
    poll_events();
    if (!running) { views_ok = 0; return; }
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    frame = (XrFrameState){ XR_TYPE_FRAME_STATE };
    if (XR_FAILED(xrWaitFrame(session, &wi, &frame))) return;
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    if (XR_FAILED(xrBeginFrame(session, &bi))) return;
    host_display_timing(frame.predictedDisplayTime, frame.predictedDisplayPeriod);
    in_frame = 1;

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
    if (views_ok && ENV_ON("LINDBERGH_POSE_LOG")) {
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
    else {
        host_vr_input(-100000, -100000, 0, 0); host_vr_input2(0, -100000, -100000);
        xo.gun_valid = xo.beam = xo.dot = xo.flash = 0; xo2.gun_valid = xo2.beam = xo2.dot = xo2.flash = 0;
        stereo_set_gun_ray(0, NULL, NULL, 0); stereo_set_gun_ray(1, NULL, NULL, 0);
    }
    if (cfg.show_stats) fps_update(frame.predictedDisplayTime);
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
    for (int pl = 0; overlay_ok && pl < 2; pl++) {
        if (!probe_on[pl]) continue;
        float px[48][2], z[48], t[48];
        unsigned arr = 0;
        float zn = 0, zf = 0;
        int n = stereo_aim_samples(probe_o[pl], probe_d[pl], 48, px, z, t, &arr, &zn, &zf);
        float h = xo_probe(pl, arr, (const float (*)[2])px, z, t, n, zn, zf);
        if (h != -2) aim_hit_t[pl] = h;
        probe_on[pl] = 0;
        static int log = -1; static unsigned k;
        if (log < 0) { const char *v = getenv("LINDBERGH_AIM_LOG"); log = v && *v == '1'; }
        if (log && ++k % 30 == 0) fprintf(stderr, "[aim] P%d: %d samples, laser reaches %.2f m\n", pl + 1, n, aim_hit_t[pl] / cfg.scale);
    }
}

/* LINDBERGH_XR_PROF=1: where the frame's end waits - the game's GPU work
 * (a finish on entry), the copies into the headset's images, xrEndFrame.
 * Each finish stalls the pipeline: a measurement, not a way to run. */
static int xr_prof = -1;
static double xp_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double xp_t0, xp_game, xp_copy, xp_end; static unsigned xp_n;
static void xp_mark(double *acc) { if (xr_prof > 0) { gl.Finish(); double t = xp_ms(); *acc += t - xp_t0; xp_t0 = t; } }

/* LINDBERGH_GAME_SHOT=<first>,<every>[,full]: the left eye as the game drew
 * it, read from the eyes' array at the frame's end - before the headset's
 * frame is even begun, so a sleeping display or a Guardian prompt does not
 * stop it (XR_SHOT needs the session shown). game_<frame>.ppm, a quarter the
 * size unless full. LINDBERGH_SHOT_BOTH=1 also saves the right eye,
 * game_<frame>_r.ppm (and xrshot_<frame>_r.ppm for XR_SHOT): a picture that
 * differs between the eyes shows only in the pair. */
static int shot_both(void)
{
    static int both = -1;
    if (both < 0) { const char *v = getenv("LINDBERGH_SHOT_BOTH"); both = v && *v == '1'; }
    return both;
}
static void game_shot(void)
{
    static long first = -2, every = 0; static int full;
    if (first == -2) {
        const char *v = getenv("LINDBERGH_GAME_SHOT");
        first = v && *v ? atol(v) : -1;
        const char *c1 = v ? strchr(v, ',') : NULL;
        every = c1 ? atol(c1 + 1) : 0;
        full = c1 && strchr(c1 + 1, ',') && atoi(strchr(c1 + 1, ',') + 1) == 1;
    }
    if (first < 0 || (long)g_frame < first) return;
    if (every > 0 ? ((long)g_frame - first) % every : (long)g_frame != first) return;
    unsigned arr = stereo_eye_array();
    int w = 0, h = 0;
    stereo_eye_dims(&w, &h);
    if (!arr || w <= 0 || h <= 0) return;
    if (!read_fb) gl.GenFramebuffers(1, &read_fb);
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return;
    GLint read = 0;
    GLenum st = 0, err = 0;
    char name[64] = "";
    while (gl.GetError() != GL_NO_ERROR) {}     /* not ours: left by the game's frame */
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    for (int eye = 0; eye <= shot_both(); eye++) {
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
        gl.FramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arr, 0, eye);
        st = gl.CheckFramebufferStatus(GL_READ_FRAMEBUFFER);
        gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        err = gl.GetError();
        snprintf(name, sizeof name, eye ? "game_%05u_r.ppm" : "game_%05u.ppm", g_frame);
        FILE *f = err ? NULL : fopen(name, "wb");
        if (f) {
            int sp = full ? 1 : 4, qw = w / sp, qh = h / sp;
            fprintf(f, "P6\n%d %d\n255\n", qw, qh);
            for (int y = qh - 1; y >= 0; y--)
                for (int x = 0; x < qw; x++) fwrite(px + ((size_t)(y * sp) * w + x * sp) * 4, 1, 3, f);
            fclose(f);
        }
        if (err) break;
    }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    static int told;
    if (!told++ || (err && told < 6)) fprintf(stderr, "[xr] game shot frame %u: %s (error %#x, framebuffer %#x, array %u, %dx%d)\n",
                                             g_frame, err ? "read failed" : name, err, st, arr, w, h);
    free(px);
}

void xr_frame_end(void)
{
    game_shot();
    if (!in_frame) return;
    if (xr_prof < 0) { const char *v = getenv("LINDBERGH_XR_PROF"); xr_prof = v && *v == '1'; }
    if (xr_prof > 0) { xp_t0 = xp_ms(); xp_mark(&xp_game); }
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
                float hw, hh, d, place[16], view[16], proj[16], m[16], n = 0.02f, clip_far = 1000.0f;
                stereo_screen(&hw, &hh, &d);
                memset(place, 0, sizeof place);
                place[0] = hw; place[5] = hh; place[10] = 1; place[14] = -d; place[15] = 1;
                view_matrix(views[i].pose, view);
                float tx = tanf(fov[i].angleRight), ty = tanf(fov[i].angleUp);
                memset(proj, 0, sizeof proj);
                proj[0] = 1 / tx; proj[5] = 1 / ty; proj[10] = -(clip_far + n) / (clip_far - n);
                proj[11] = -1; proj[14] = -2 * clip_far * n / (clip_far - n);
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
                float view[16], proj[16], vp[16], n = 0.02f, clip_far = 100.0f;
                pose_view(views[i].pose, view);
                float tx = tanf(fov[i].angleRight), ty = tanf(fov[i].angleUp);
                memset(proj, 0, sizeof proj);
                proj[0] = 1 / tx; proj[5] = 1 / ty; proj[10] = -(clip_far + n) / (clip_far - n);
                proj[11] = -1; proj[14] = -2 * clip_far * n / (clip_far - n);
                mat4_mul(vp, proj, view);
                float eye_pos[3] = { views[i].pose.position.x, views[i].pose.position.y, views[i].pose.position.z };
                xo_draw(&xo, vp, eye_pos, (int)e->w, (int)e->h);
                if (xo2.gun_valid) xo_draw(&xo2, vp, eye_pos, (int)e->w, (int)e->h);
                if (cfg.show_stats && fpsl.shown >= 0) {
                    float pm[16], m[16];
                    memset(pm, 0, sizeof pm);
                    for (int k = 0; k < 3; k++) {
                        pm[k] = fpsl.right[k] * fpsl.hw; pm[4 + k] = fpsl.up[k] * fpsl.hh;
                        pm[8 + k] = fpsl.n[k]; pm[12 + k] = fpsl.c[k];
                    }
                    pm[15] = 1;
                    mat4_mul(m, vp, pm);
                    xo_draw_label(m, (int)e->w, (int)e->h);
                }
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
                    if (xo2.gun_valid) xo_draw(&xo2, vp, eye_pos, (int)e->w, (int)e->h);
                }
            }
            {   /* LINDBERGH_XR_SHOT=<first>,<every>: the left eye exactly as handed to
                 * the headset, a quarter the size, as xrshot_<frame>.ppm beside the
                 * game - what the player sees, for runs nobody is watching */
                static long first = -2, every = 0;
                if (first == -2) {
                    const char *v = getenv("LINDBERGH_XR_SHOT");
                    first = v && *v ? atol(v) : -1;
                    every = v && strchr(v, ',') ? atol(strchr(v, ',') + 1) : 0;
                }
                if ((i == 0 || shot_both()) && first >= 0 && (long)g_frame >= first &&
                    (every > 0 ? ((long)g_frame - first) % every == 0 : (long)g_frame == first)) {
                    int w = (int)e->w, h = (int)e->h;
                    unsigned char *px = malloc((size_t)w * h * 4);
                    if (px) {
                        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, e->fb[e->acquired]);
                        gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
                        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
                        char name[64]; snprintf(name, sizeof name, i ? "xrshot_%05u_r.ppm" : "xrshot_%05u.ppm", g_frame);
                        FILE *f = fopen(name, "wb");
                        if (f) {
                            static int full = -1;   /* LINDBERGH_XR_SHOT_FULL=1: the whole size */
                            if (full < 0) { const char *fv = getenv("LINDBERGH_XR_SHOT_FULL"); full = fv && *fv == '1'; }
                            int st = full ? 1 : 4, qw = w / st, qh = h / st;
                            fprintf(f, "P6\n%d %d\n255\n", qw, qh);
                            for (int y = qh - 1; y >= 0; y--)        /* GL rows run bottom up */
                                for (int x = 0; x < qw; x++) fwrite(px + ((size_t)(y * st) * w + x * st) * 4, 1, 3, f);
                            fclose(f);
                            fprintf(stderr, "[xr] shot frame %u -> %s\n", g_frame, name);
                        }
                        free(px);
                    }
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
    xp_mark(&xp_copy);
    XrResult er = xrEndFrame(session, &ei);
    if (xr_prof > 0) {
        xp_end += xp_ms() - xp_t0;
        if (++xp_n == 60) {
            fprintf(stderr, "[xrprof] frame %u: game's GPU work %.1f ms, eye copies %.1f ms, xrEndFrame %.1f ms (flat %d)\n",
                    g_frame, xp_game / xp_n, xp_copy / xp_n, xp_end / xp_n, flat);
            xp_game = xp_copy = xp_end = 0; xp_n = 0;
        }
    }
    static unsigned bad;
    if (XR_FAILED(er) && bad++ < 20) fprintf(stderr, "[xr] frame %u: xrEndFrame failed %d\n", g_frame, (int)er);
}

int xr_running(void) { return running; }

#endif /* __ANDROID__ */
