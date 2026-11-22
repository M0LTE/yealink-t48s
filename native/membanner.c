// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * membanner LINE...: write lines of text on the T48S screen through /dev/mem.
 *
 * No display driver needed: U-Boot sets up the LCD controller and it keeps
 * scanning out the framebuffer at 0x07A24000 (800x480, 32 bpp). The first line
 * is drawn large, the rest small. Static musl build.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gen/fonts.h"

#define FB_PHYS 0x07A24000
#define W 800
#define H 480

static unsigned *fb;

static unsigned blend(unsigned d, unsigned s, unsigned a)
{
    unsigned r = (((s >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
    unsigned g = (((s >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
    unsigned b = ((s & 255) * a + (d & 255) * (255 - a)) / 255;
    return (r << 16) | (g << 8) | b;
}

static void text(int x, int y, const struct font *f, unsigned c, const char *s)
{
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const struct glyph *g = &f->g[ch - 32];
        const unsigned char *a = f->a + g->off;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                int px = x + g->xoff + i, py = y + g->yoff + j;
                unsigned al = a[j * g->w + i];
                if (px < 0 || px >= W || py < 0 || py >= H || !al) continue;
                unsigned *d = fb + py * W + px;
                *d = al == 255 ? c : blend(*d, c, al);
            }
        x += g->adv;
    }
}

int main(int argc, char **argv)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    fb = mmap(0, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, FB_PHYS);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }
    for (int i = 0; i < W * H; i++) fb[i] = 0x000e141e;
    for (int i = 0; i < W * 64; i++) fb[i] = 0x001a2434;
    int y = 10;
    for (int k = 1; k < argc; k++) {
        const struct font *f = k == 1 ? &f_big : &f_mono;
        text(24, y, f, k == 1 ? 0x00ffbe3c : 0x00e1e8f0, argv[k]);
        y += k == 1 ? 74 : 28;
    }
    return 0;
}
