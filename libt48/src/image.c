// SPDX-License-Identifier: AGPL-3.0-or-later
/* libt48 images: read a binary PPM, write a PPM or an (uncompressed) PNG. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t48.h"

/* the next number in a PPM header, skipping white space and # comments */
static int ppm_int(FILE *f)
{
    int c, v = 0, any = 0;
    for (;;) {
        c = fgetc(f);
        if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n') {} continue; }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { if (any) return v; continue; }
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
        any = 1;
        if (v > 100000) return -1;
    }
}

int t48_load_ppm(const char *path, t48_surface *img)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int ok = fgetc(f) == 'P' && fgetc(f) == '6';
    int w = ok ? ppm_int(f) : -1, h = ok ? ppm_int(f) : -1, max = ok ? ppm_int(f) : -1;
    if (w < 1 || h < 1 || w > 4096 || h > 4096 || max != 255) { fclose(f); return -1; }
    uint32_t *px = malloc((size_t)w * h * 4);
    unsigned char *row = malloc((size_t)w * 3);
    if (!px || !row) { free(px); free(row); fclose(f); return -1; }
    for (int j = 0; j < h; j++) {
        if (fread(row, 3, (size_t)w, f) != (size_t)w) { free(px); free(row); fclose(f); return -1; }
        for (int i = 0; i < w; i++) px[j * w + i] = T48_RGB(row[i * 3], row[i * 3 + 1], row[i * 3 + 2]);
    }
    free(row);
    fclose(f);
    img->px = px;
    img->w = img->stride = w;
    img->h = h;
    return 0;
}

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const unsigned char *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int b = 0; b < 8; b++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            crc_table[i] = k;
        }
    while (n--) c = crc_table[(c ^ *p++) & 255] ^ (c >> 8);
    return c;
}

static void be32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const unsigned char *data, size_t n)
{
    unsigned char b[8];
    be32(b, (uint32_t)n);
    memcpy(b + 4, type, 4);
    fwrite(b, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc(0xFFFFFFFFu, b + 4, 4);
    c = crc(c, data, n) ^ 0xFFFFFFFFu;
    be32(b, c);
    fwrite(b, 1, 4, f);
}

static int save_png(const t48_surface *s, FILE *f)
{
    size_t row = (size_t)s->w * 3 + 1, raw_n = row * s->h;
    size_t blocks = (raw_n + 65534) / 65535, z_n = 2 + raw_n + blocks * 5 + 4;
    unsigned char *z = malloc(z_n), *raw = malloc(raw_n);
    if (!z || !raw) { free(z); free(raw); return -1; }
    uint32_t a = 1, b = 0;
    for (int j = 0; j < s->h; j++) {
        unsigned char *r = raw + j * row;
        *r++ = 0;  /* filter: none */
        for (int i = 0; i < s->w; i++) {
            uint32_t p = s->px[j * s->stride + i];
            *r++ = p >> 16; *r++ = p >> 8; *r++ = p;
        }
    }
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    unsigned char *o = z;
    *o++ = 0x78; *o++ = 0x01;
    for (size_t at = 0; at < raw_n;) {  /* stored blocks: valid deflate, no compression */
        size_t n = raw_n - at > 65535 ? 65535 : raw_n - at;
        *o++ = at + n == raw_n;
        *o++ = n & 255; *o++ = n >> 8; *o++ = ~n & 255; *o++ = (~n >> 8) & 255;
        memcpy(o, raw + at, n);
        o += n; at += n;
    }
    be32(o, (b << 16) | a);
    unsigned char ihdr[13];
    be32(ihdr, (uint32_t)s->w);
    be32(ihdr + 4, (uint32_t)s->h);
    memcpy(ihdr + 8, "\x08\x02\x00\x00\x00", 5);  /* 8 bits, RGB */
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, z_n);
    chunk(f, "IEND", NULL, 0);
    free(z);
    free(raw);
    return 0;
}

int t48_save(const t48_surface *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(path);
    int r = 0;
    if (n > 4 && !strcmp(path + n - 4, ".png")) {
        r = save_png(s, f);
    } else {
        fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
        for (int j = 0; j < s->h; j++)
            for (int i = 0; i < s->w; i++) {
                uint32_t p = s->px[j * s->stride + i];
                unsigned char c[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
                fwrite(c, 1, 3, f);
            }
    }
    if (fclose(f)) r = -1;
    return r;
}
