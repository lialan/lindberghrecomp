/*
 * xr_overlay.h - the gun and the laser, drawn into each eye (xr_overlay.c).
 */
#ifndef XR_OVERLAY_H
#define XR_OVERLAY_H

enum { XO_SIGHT_NONE, XO_SIGHT_RED_DOT, XO_SIGHT_HOLO };
enum { XO_GUN_SMG, XO_GUN_TP9 };

/* One headset frame's worth, in metres in the LOCAL space. */
typedef struct {
    int   gun_valid;
    float gun_q[4], gun_p[3];   /* the controller's aim pose: x y z w, position */
    float recoil;               /* metres the gun is pushed back */
    float muzzle[3];            /* where the barrel ends */
    float end[3];               /* where the aim ray meets the game's screen */
    int   beam, dot, flash;     /* what to draw */
    int   sight;                /* XO_SIGHT_*: the optic on the rail */
    int   show_gun;             /* 0: the controller shows only the laser */
    int   gun_model;            /* XO_GUN_* */
} XoFrame;

int  xo_init(void);
/* With the eye's swapchain image bound for drawing (and a depth buffer). */
void xo_draw(const XoFrame *f, const float viewproj[16], const float eye_pos[3], int w, int h);

/* The game's flat picture (a 2D texture, or layer 0 of an array) on the
 * quad -1..1 placed by mvp; clears the rest of the image to black. */
void xo_draw_screen(const float mvp[16], unsigned tex, int array_layer0, int w, int h);

/* The settings panel: its picture, then drawn on the quad -1..1 through mvp. */
void xo_panel_upload(const unsigned char *rgba, int w, int h);
void xo_draw_panel(const float mvp[16], int w, int h);

/* What the gun's ray points at: queues a read of the world's depth array at
 * the n samples' pixels, and returns the distance t of the first sample that
 * lay behind a surface two frames ago (zn, zf: the view's near and far) (-1 none, -2 no answer yet). */
float xo_probe(unsigned depth_array, const float (*px)[2], const float *z, const float *t, int n, float zn, float zf);

/* Layer `layer` of the world's depth array into the bound framebuffer's depth
 * (w x h), for the headset's depth layer. */
void xo_copy_depth(unsigned depth_array, int layer, int w, int h);

#endif
