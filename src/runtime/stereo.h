#include <stdint.h>
/*
 * stereo.h - drawing each frame once per eye (stereo.c), and what a VR host
 * tells it about the eyes.
 */
#ifndef STEREO_H
#define STEREO_H

void stereo_init(void);                 /* hle_register_gl */
void stereo_start(void);                /* turn it on (LINDBERGH_STEREO does so too) */
int  stereo_enabled(void);
void stereo_frame_begin(void);          /* before the game draws a frame */
void stereo_frame_end(void);            /* at the swap: draws the second eye */

void stereo_eye_size(int w, int h);
/* The host's own framebuffer for an eye (an XR swapchain image). */
void stereo_set_eye_target(int eye, unsigned fbo);
/* view: game camera space -> eye space, column-major, in game units.
 * The tangents of the eye's field of view: left, right, down, up. */
void stereo_set_eye(int eye, const float view[16], float tan_l, float tan_r, float tan_d, float tan_u);
/* The HUD is drawn on a screen this far in front of the game camera. */
void stereo_set_screen_distance(float d);
/* A ray in game camera space -> the point it aims at on that screen, in the
 * game's 640x480 pixels. Returns 0 if it points away. */
int  stereo_ray_to_screen(const float o[3], const float d[3], float *x, float *y);
/* The game's own camera, as tangents of half its field of view. */
void stereo_game_camera(float *tan_x, float *tan_y);
unsigned stereo_eye_texture(int eye);
/* multiview: both eyes' image as one two-layer array, as the driver knows it
 * (0 when the eyes are drawn one at a time) */
unsigned stereo_eye_array(void);
int stereo_multiview(void);
void stereo_reproject_view(int view, float out[16]);
unsigned stereo_eye_framebuffer(int eye);
/* Rectangle-texture coordinates are scaled by this (1 when not in stereo). */
void stereo_rect_scale(float *x, float *y);
/* a texture the game renders into, reallocated larger for the eye */
int stereo_is_render_target(unsigned tex);
/* For vertex programs with the game's projection in their own constants. */
void stereo_reproject(float out[16]);
void stereo_present_desktop(int win_w, int win_h);
/* The game's menu screens, by the state function the game runs each frame
 * while one is up: its 3D goes on the screen with the menu's 2D. dispatch()
 * passes on every guest call inside [stereo_state_lo, stereo_state_hi]. */
extern uint32_t stereo_state_lo, stereo_state_hi;
/* The frame just drawn is a menu, drawn flat as on the arcade: the host shows
 * the eye image on the screen, whose half size and distance in game units
 * from the game camera stereo_screen gives. */
int  stereo_flat_frame(void);
void stereo_screen(float *half_w, float *half_h, float *dist);
void stereo_guest_state(uint32_t va);

/* hle_gl_posix.c: a live glMapBuffer's guest shadow, for the recording */
int gl_map_shadow(unsigned target, void **shadow, int *size);
unsigned gl_bound_buffer(unsigned target);

#endif
