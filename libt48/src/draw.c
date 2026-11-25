// SPDX-License-Identifier: AGPL-3.0-or-later
/* libt48 drawing: fills, boxes, copies and text. Lifted out of native/waterfall.c. */
#include <stdio.h>
#include <string.h>
#include "t48.h"

/* clip a rectangle to the surface; 0 if nothing is left */
static int clip(const t48_surface *s, int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > s->w) *w = s->w - *x;
    if (*y + *h > s->h) *h = s->h - *y;
    return *w > 0 && *h > 0;
}

uint32_t t48_mix(uint32_t d, uint32_t c, unsigned a)
{
    a += a >> 7;  /* 0..256 */
    uint32_t rb = ((c & 0xFF00FF) * a + (d & 0xFF00FF) * (256 - a)) >> 8;
    uint32_t g = ((c & 0x00FF00) * a + (d & 0x00FF00) * (256 - a)) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

uint32_t t48_dim(uint32_t c, int num, int den)
{
    return T48_RGB(((c >> 16) & 255) * num / den, ((c >> 8) & 255) * num / den, (c & 255) * num / den);
}

void t48_fill(t48_surface *s, int x, int y, int w, int h, uint32_t c)
{
    if (!clip(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *p = s->px + (y + j) * s->stride + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

void t48_fill_alpha(t48_surface *s, int x, int y, int w, int h, uint32_t c, unsigned a)
{
    if (!clip(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *p = s->px + (y + j) * s->stride + x;
        for (int i = 0; i < w; i++) p[i] = t48_mix(p[i], c, a);
    }
}

void t48_rect(t48_surface *s, int x, int y, int w, int h, uint32_t c)
{
    t48_fill(s, x, y, w, 1, c);
    t48_fill(s, x, y + h - 1, w, 1, c);
    t48_fill(s, x, y, 1, h, c);
    t48_fill(s, x + w - 1, y, 1, h, c);
}

/* one corner of a rounded box: the r x r square at x, y, with the circle's
 * centre at the square's corner picked by cx, cy (0 or 1) */
static void corner(t48_surface *s, int x, int y, int r, int cx, int cy, uint32_t c)
{
    int r4 = r * 4;
    for (int j = 0; j < r; j++) {
        int py = y + j;
        if (py < 0 || py >= s->h) continue;
        for (int i = 0; i < r; i++) {
            int px = x + i, in = 0;
            if (px < 0 || px >= s->w) continue;
            /* 4x4 samples per pixel, in quarter pixels from the centre */
            for (int sj = 0; sj < 4; sj++) {
                int dy = cy ? r4 - (j * 4 + sj) - 1 : j * 4 + sj;
                for (int si = 0; si < 4; si++) {
                    int dx = cx ? r4 - (i * 4 + si) - 1 : i * 4 + si;
                    if (dx * dx + dy * dy < r4 * r4) in++;
                }
            }
            if (!in) continue;
            uint32_t *d = s->px + py * s->stride + px;
            *d = in == 16 ? c : t48_mix(*d, c, (unsigned)in * 255 / 16);
        }
    }
}

void t48_round_fill(t48_surface *s, int x, int y, int w, int h, int r, uint32_t c)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 1) { t48_fill(s, x, y, w, h, c); return; }
    t48_fill(s, x + r, y, w - 2 * r, r, c);
    t48_fill(s, x, y + r, w, h - 2 * r, c);
    t48_fill(s, x + r, y + h - r, w - 2 * r, r, c);
    corner(s, x, y, r, 1, 1, c);
    corner(s, x + w - r, y, r, 0, 1, c);
    corner(s, x, y + h - r, r, 1, 0, c);
    corner(s, x + w - r, y + h - r, r, 0, 0, c);
}

void t48_blit(t48_surface *s, int x, int y, const uint32_t *src, int sw, int sh, int sstride)
{
    int cx = x, cy = y, cw = sw, ch = sh;
    if (!clip(s, &cx, &cy, &cw, &ch)) return;
    for (int j = 0; j < ch; j++)
        memcpy(s->px + (cy + j) * s->stride + cx, src + (cy - y + j) * sstride + (cx - x), (size_t)cw * 4);
}

/* Each source row is made wide once, in a small buffer that stays in the
 * processor's cache, then copied to its n rows of the surface: the surface is
 * only ever written, and in order. */
void t48_blit_scaled(t48_surface *s, int x, int y, const uint32_t *src, int sw, int sh, int sstride, int n)
{
    uint32_t row[1024];
    if (n < 1 || n > 4 || x < 0 || y < 0) return;
    if (n == 1) { t48_blit(s, x, y, src, sw, sh, sstride); return; }
    if (sw > (s->w - x) / n) sw = (s->w - x) / n;
    if (sh > (s->h - y) / n) sh = (s->h - y) / n;
    if (sw > (int)(sizeof row / sizeof row[0]) / n) sw = (int)(sizeof row / sizeof row[0]) / n;
    if (sw <= 0 || sh <= 0) return;
    for (int j = 0; j < sh; j++) {
        const uint32_t *in = src + j * sstride;
        uint32_t *out = row;
        if (n == 2) for (int i = 0; i < sw; i++) { uint32_t v = in[i]; out[0] = v; out[1] = v; out += 2; }
        else for (int i = 0; i < sw; i++) { uint32_t v = in[i]; for (int k = 0; k < n; k++) *out++ = v; }
        uint32_t *d = s->px + (y + j * n) * s->stride + x;
        for (int k = 0; k < n; k++, d += s->stride) memcpy(d, row, (size_t)sw * n * 4);
    }
}

int t48_text(t48_surface *s, int x, int y, const t48_font *f, uint32_t c, const char *str)
{
    for (; *str; str++) {
        int ch = (unsigned char)*str;
        if (ch < 32 || ch > 126) ch = '?';
        const t48_glyph *g = &f->g[ch - 32];
        const unsigned char *a = f->a + g->off;
        for (int j = 0; j < g->h; j++) {
            int py = y + g->yoff + j;
            if (py < 0 || py >= s->h) continue;
            for (int i = 0; i < g->w; i++) {
                int px = x + g->xoff + i;
                unsigned al = a[j * g->w + i];
                if (px < 0 || px >= s->w || !al) continue;
                uint32_t *d = s->px + py * s->stride + px;
                *d = al == 255 ? c : t48_mix(*d, c, al);
            }
        }
        x += g->adv;
    }
    return x;
}

int t48_text_width(const t48_font *f, const char *str)
{
    int w = 0;
    for (; *str; str++) {
        int ch = (unsigned char)*str;
        w += f->g[(ch < 32 || ch > 126 ? '?' : ch) - 32].adv;
    }
    return w;
}

void t48_text_fit(t48_surface *s, int x, int y, int maxw, const t48_font *f, uint32_t c, const char *str)
{
    char buf[128];
    size_t n = (size_t)snprintf(buf, sizeof buf - 3, "%s", str);
    if (n > sizeof buf - 4) n = sizeof buf - 4;
    if (t48_text_width(f, buf) > maxw) {
        while (n > 0) {
            buf[--n] = 0;
            strcpy(buf + n, "..");
            if (t48_text_width(f, buf) <= maxw) break;
        }
        if (t48_text_width(f, buf) > maxw) return;
    }
    t48_text(s, x, y, f, c, buf);
}

void t48_text_centre(t48_surface *s, int cx, int y, const t48_font *f, uint32_t c, const char *str)
{
    t48_text(s, cx - t48_text_width(f, str) / 2, y, f, c, str);
}

void t48_text_right(t48_surface *s, int rx, int y, const t48_font *f, uint32_t c, const char *str)
{
    t48_text(s, rx - t48_text_width(f, str), y, f, c, str);
}
