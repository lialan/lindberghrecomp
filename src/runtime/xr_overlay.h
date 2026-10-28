/*
 * xr_overlay.h - the gun and the laser, drawn into each eye (xr_overlay.c).
 */
#ifndef XR_OVERLAY_H
#define XR_OVERLAY_H

/* One headset frame's worth, in metres in the LOCAL space. */
typedef struct {
    int   gun_valid;
    float gun_q[4], gun_p[3];   /* the controller's aim pose: x y z w, position */
    float recoil;               /* metres the gun is pushed back */
    float muzzle[3];            /* where the barrel ends */
    float end[3];               /* where the aim ray meets the game's screen */
    int   beam, dot, flash;     /* what to draw */
} XoFrame;

int  xo_init(void);
/* With the eye's swapchain image bound for drawing (and a depth buffer). */
void xo_draw(const XoFrame *f, const float viewproj[16], const float eye_pos[3], int w, int h);

/* The game's flat picture (a 2D texture, or layer 0 of an array) on the
 * quad -1..1 placed by mvp; clears the rest of the image to black. */
void xo_draw_screen(const float mvp[16], unsigned tex, int array_layer0, int w, int h);

#endif
