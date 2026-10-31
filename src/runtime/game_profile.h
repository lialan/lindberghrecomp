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
    unsigned char reload_switch[2]; /* the player's switch byte and bit the reload input sets (0: byte 0, bit 0) */
    int   sysdata_region;       /* the game reads its region (and language) from the EEPROM's system data */
    const char *ride_board;     /* the serial device a large attraction cabinet's ride board is on (hle_rideboard.c) */
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
        int   level_horizon;    /* the world kept level: the game camera's pitch and roll go into where its view
                                 * sits, not into the world (needs camera_in_constants; LINDBERGH_XR_LEVEL=0 off) */
        int   gl_thread;        /* the GL on a thread of its own (gl_thread.c; LINDBERGH_GL_THREAD=0 off) */
        int   hud_head_locked;  /* the 2D HUD stays in front of the face, not in the world (LINDBERGH_XR_HUD_LOCK=0 off) */
        int   aim_on_hud;       /* each shot goes where the laser crosses the HUD's screen (LINDBERGH_XR_AIM_HUD=0 off) */
        unsigned drop_sprite_tex[8]; /* world sprites with these GL textures, left out (LINDBERGH_XR_DROP_TEX=0 keeps them) */
        float hud_lens;         /* the game draws its 2D through a perspective of its own: one with this x scale
                                 * (projection[0]) is screen content, not world (LINDBERGH_STEREO_2D_LENS) */
    } vr;
} LgGameProfile;

/* Called once, first thing in the game's main(). */
void game_profile_apply(const LgGameProfile *p);
/* The profile applied (all 0 if none was). */
const LgGameProfile *game_profile(void);

#endif
