/*
 * xr_menu.h - the headset's settings panel, painted on the CPU (xr_menu.c)
 * and shown by the overlay (xo_draw_panel).
 */
#ifndef XR_MENU_H
#define XR_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

#define XM_W 768                /* the panel's picture, in pixels */
#define XM_H 860                /* title, XM_ROWS rows of 36 and the hint */
#define XM_ROWS 21

typedef struct {
    const char *label;
    char value[40];             /* "" for an action */
} XmItem;

/* Paints the panel into rgba (XM_W x XM_H x 4): title, the items with their
 * values, hover highlighted (-1: none), and a hint at the bottom. */
void xm_paint(unsigned char *rgba, const char *title, const XmItem *items, int n, int hover, const char *hint);
/* A small label (the frame rate): light text on a dark box, rgba w x h x 4. */
#define XM_LABEL_W 224
#define XM_LABEL_H 48
void xm_paint_label(unsigned char *rgba, const char *s);
/* The stats panel: n left-aligned lines (up to 27 characters each) of light
 * text on a dark box, rgba XM_STATS_W x XM_STATS_H x 4. */
#define XM_STATS_W 480
#define XM_STATS_H 160
void xm_paint_lines(unsigned char *rgba, const char *const *lines, int n);
/* A startup message (XM_W x XM_H x 4): a centred box with a title and a
 * second line, transparent around it. */
#define XM_MESSAGE_H 170
void xm_paint_message(unsigned char *rgba, const char *title, const char *line);
/* The row under panel coordinates u, v (0..1 from the top left); -1 if none. */
int  xm_row_at(float u, float v, int n);

#ifdef __cplusplus
}
#endif
#endif
