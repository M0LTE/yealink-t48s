// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: a picture onto the screen's own memory, for both backends.
 *
 * A picture the size of the screen is copied, to 32 bits as it is or to 16
 * (RGB565), the right way up or turned by 180 degrees for a panel mounted
 * upside down (the T46S's). A picture of another size is fitted: each screen
 * pixel is the average of the picture's pixels under it. That is what an
 * 800x480 app gets on a smaller screen. See docs/models.md. */
#include <string.h>
#include "shell.h"

static inline uint16_t to565(uint32_t p)
{
    return (uint16_t)((p >> 8 & 0xF800) | (p >> 5 & 0x07E0) | (p >> 3 & 0x001F));
}

/* the same size: one row at a time, no arithmetic but the 16-bit packing */
static void copy(unsigned char *fb, int line, int pw, int ph, int bpp, int turned,
                 const uint32_t *src, int stride, int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++) {
        const uint32_t *sp = src + (size_t)(y + j) * (size_t)stride + x;
        if (!turned) {
            unsigned char *row = fb + (size_t)(y + j) * (size_t)line;
            if (bpp == 32) {
                memcpy(row + (size_t)x * 4, sp, (size_t)w * 4);
            } else {
                uint16_t *dp = (uint16_t *)row + x;
                for (int i = 0; i < w; i++) dp[i] = to565(sp[i]);
            }
        } else {
            /* the last row first, and each row from its right-hand end */
            unsigned char *row = fb + (size_t)(ph - 1 - y - j) * (size_t)line;
            if (bpp == 32) {
                uint32_t *dp = (uint32_t *)row + (pw - 1 - x);
                for (int i = 0; i < w; i++) *dp-- = sp[i];
            } else {
                uint16_t *dp = (uint16_t *)row + (pw - 1 - x);
                for (int i = 0; i < w; i++) *dp-- = to565(sp[i]);
            }
        }
    }
}

void fit_put(void *fbv, int line, int pw, int ph, int bpp, int turned,
             const uint32_t *src, int stride, int sw, int sh, int x, int y, int w, int h)
{
    unsigned char *fb = fbv;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    if (sw == pw && sh == ph) { copy(fb, line, pw, ph, bpp, turned, src, stride, x, y, w, h); return; }

    /* the screen pixels that any of this part of the picture falls on */
    int dx0 = x * pw / sw, dx1 = ((x + w) * pw + sw - 1) / sw;
    int dy0 = y * ph / sh, dy1 = ((y + h) * ph + sh - 1) / sh;
    if (dx1 > pw) dx1 = pw;
    if (dy1 > ph) dy1 = ph;
    for (int dy = dy0; dy < dy1; dy++) {
        int sy0 = dy * sh / ph, sy1 = ((dy + 1) * sh + ph - 1) / ph;
        int ty = turned ? ph - 1 - dy : dy;
        unsigned char *row = fb + (size_t)ty * (size_t)line;
        if (sy1 > sh) sy1 = sh;
        for (int dx = dx0; dx < dx1; dx++) {
            int sx0 = dx * sw / pw, sx1 = ((dx + 1) * sw + pw - 1) / pw;
            unsigned r = 0, g = 0, b = 0, n = 0;
            if (sx1 > sw) sx1 = sw;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint32_t *sp = src + (size_t)sy * (size_t)stride;
                for (int sx = sx0; sx < sx1; sx++, n++) {
                    r += sp[sx] >> 16 & 255; g += sp[sx] >> 8 & 255; b += sp[sx] & 255;
                }
            }
            if (!n) continue;
            r /= n; g /= n; b /= n;
            int tx = turned ? pw - 1 - dx : dx;
            if (bpp == 16) ((uint16_t *)row)[tx] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
            else ((uint32_t *)row)[tx] = r << 16 | g << 8 | b;
        }
    }
}
