/*
 * xr_menu.h - the headset's settings panel, painted on the CPU (xr_menu.c)
 * and shown by the overlay (xo_draw_panel).
 */
#ifndef XR_MENU_H
#define XR_MENU_H

#define XM_W 768                /* the panel's picture, in pixels */
#define XM_H 752
#define XM_ROWS 18

typedef struct {
    const char *label;
    char value[40];             /* "" for an action */
} XmItem;

/* Paints the panel into rgba (XM_W x XM_H x 4): title, the items with their
 * values, hover highlighted (-1: none), and a hint at the bottom. */
void xm_paint(unsigned char *rgba, const char *title, const XmItem *items, int n, int hover, const char *hint);
/* The row under panel coordinates u, v (0..1 from the top left); -1 if none. */
int  xm_row_at(float u, float v, int n);

#endif
