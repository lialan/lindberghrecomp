/*
 * xr_menu.c - the settings panel's picture: text in rows, painted into an
 * RGBA image the overlay shows on a quad in front of the player. Painted only
 * when something on it changes.
 */
#include <string.h>

#include "xr_font.h"
#include "xr_menu.h"

#define MARGIN 28
#define TITLE_H 64
#define ROW_H 36

static void fill(unsigned char *img, int x0, int y0, int x1, int y1, const unsigned char c[4])
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) memcpy(img + ((size_t)y * XM_W + x) * 4, c, 4);
}

static void text(unsigned char *img, int x, int y, const char *s, const unsigned char c[3])
{
    for (; *s && x + FONT_W <= XM_W; s++, x += FONT_W - 3) {
        int ch = (unsigned char)*s - FONT_FIRST;
        if (ch < 0 || ch >= FONT_COUNT) continue;
        for (int j = 0; j < FONT_H && y + j < XM_H; j++)
            for (int i = 0; i < FONT_W; i++) {
                unsigned a = FONT[ch][j * FONT_W + i];
                if (!a) continue;
                unsigned char *p = img + ((size_t)(y + j) * XM_W + x + i) * 4;
                for (int k = 0; k < 3; k++) p[k] = (unsigned char)((p[k] * (255 - a) + c[k] * a) / 255);
            }
    }
}

static int text_w(const char *s) { return (int)strlen(s) * (FONT_W - 3); }

void xm_paint(unsigned char *img, const char *title, const XmItem *items, int n, int hover, const char *hint)
{
    static const unsigned char bg[4] = { 18, 22, 28, 235 }, bar[4] = { 40, 70, 52, 255 },
                               hi[4] = { 60, 110, 80, 255 }, line[4] = { 70, 80, 90, 255 };
    static const unsigned char white[3] = { 235, 238, 240 }, green[3] = { 120, 230, 140 },
                               grey[3] = { 150, 158, 166 }, gold[3] = { 250, 200, 90 };
    fill(img, 0, 0, XM_W, XM_H, bg);
    fill(img, 0, 0, XM_W, TITLE_H, bar);
    text(img, MARGIN, (TITLE_H - FONT_H) / 2, title, white);
    for (int i = 0; i < n && i < XM_ROWS; i++) {
        int y = TITLE_H + 8 + i * ROW_H;
        if (i == hover) fill(img, 8, y, XM_W - 8, y + ROW_H - 2, hi);
        else fill(img, MARGIN, y + ROW_H - 2, XM_W - MARGIN, y + ROW_H - 1, line);
        int action = !items[i].value[0];
        text(img, MARGIN, y + 1, items[i].label, action ? gold : white);
        if (!action) text(img, XM_W - MARGIN - text_w(items[i].value), y + 1, items[i].value, green);
    }
    if (hint) text(img, MARGIN, XM_H - FONT_H - 10, hint, grey);
}

int xm_row_at(float u, float v, int n)
{
    if (u < 0 || u > 1 || v < 0 || v > 1) return -1;
    int y = (int)(v * XM_H) - TITLE_H - 8;
    if (y < 0) return -1;
    int r = y / ROW_H;
    return r < n && r < XM_ROWS ? r : -1;
}
