/*
 * game_profile.h - what a game tells the runtime about itself.
 *
 * The runtime is shared by every game, and where it has to guess how a game
 * draws or what its numbers mean, a guess tuned for one game moves the
 * others. So each game states its choices here, in one block in its own
 * host.c, and the runtime takes them from there; a field left 0 keeps the
 * runtime's default. The LINDBERGH_* environment variables still override
 * any of it, for a test run.
 *
 * Game-specific code - a function override, a gun curve - stays in the
 * game's own files; this is only for settings.
 */
#ifndef GAME_PROFILE_H
#define GAME_PROFILE_H

/* When the world's depth is read, for the aim and for the headset: the
 * world's target is left for good only after its last draw, and the depth
 * there can be written over later in the frame. */
enum {
    LG_DEPTH_AUTO = 0,          /* on leaving the world's target after its last world draw, as the previous frame had it */
    LG_DEPTH_FIRST_LEAVE,       /* the first time the world's target is left (later passes write over its depth) */
    LG_DEPTH_FRAME_END,         /* at the frame's end */
};

/* The headset's depth for its corrected frames */
enum {
    LG_COMPOSITOR_DEPTH_HALF = 0,   /* half the eye's size (default): half the copy's cost */
    LG_COMPOSITOR_DEPTH_FULL,
    LG_COMPOSITOR_DEPTH_OFF,
};

typedef struct {
    const char *title;          /* the window, the menu bar and the Dock */
    int   width, height;        /* the frame the game draws (0: 640x480) */
    int   pace_fps;             /* hold the game to this rate (0: the display's vsync paces it) */
    float sound_gain;           /* libsegaapi's mix, before its limiter (0: 1) */
    double aim[4];              /* sight = aim[0] * pointer + aim[1] across, aim[2] * pointer + aim[3] down; all 0: 1, 0, 1, 0 */
    void (*gun_map)(int *x, int *y);    /* the sight's position -> the gun reading that puts it there */
    int   desktop_wide;         /* the desktop build draws 16:9 with the HUD at the edges (Ghost Squad) */
    float cull_fov;             /* the angle, degrees, the game culls its models to, when wide (0: the game's) */

    /* in the headset */
    struct {
        const char *title;      /* the settings panel's */
        int   width, height;    /* the frame the game draws there, if not the desktop's */
        float scale;            /* game units per metre (0: 1) */
        int   world_w, world_h; /* the world drawn into a target of its own, not the screen (Let's Go Jungle: 800x600) */
        int   camera_in_constants;  /* the camera is in the vertex programs' constants, not GL's matrices */
        int   depth_read;       /* LG_DEPTH_* */
        int   compositor_depth; /* LG_COMPOSITOR_DEPTH_* */
        int   face_swings;      /* when the game swings its camera hard, its view is put back in front of the
                                 * player's head (needs camera_in_constants; LINDBERGH_XR_FACE_SWINGS=0 turns it off) */
    } vr;
} LgGameProfile;

/* Called once, first thing in the game's main(). */
void game_profile_apply(const LgGameProfile *p);
/* The profile applied (all 0 if none was). */
const LgGameProfile *game_profile(void);

#endif
