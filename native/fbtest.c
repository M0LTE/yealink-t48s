// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * First native program for the Yealink T48S (BCM11188, ARMv6, Linux 2.6.27 EABI).
 * Freestanding: no libc, raw syscalls only, so it runs regardless of the phone's glibc.
 * Draws a test card on /dev/fb0, then reports touchscreen and keypad events on stdout.
 */
typedef unsigned int u32;
typedef unsigned short u16;
typedef int s32;

#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_mmap2 192
#define SYS_poll 168

static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r5 __asm__("r5") = f;
    register long r7 __asm__("r7") = n;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
                     : "memory");
    return r0;
}
#define sc3(n, a, b, c) sc6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

static void put(const char *s)
{
    long n = 0;
    while (s[n]) n++;
    sc3(SYS_write, 1, s, n);
}

static void putnum(long v)
{
    char b[16];
    int i = 15;
    int neg = v < 0;
    b[i] = 0;
    if (neg) v = -v;
    do { b[--i] = '0' + v % 10; v /= 10; } while (v);
    if (neg) b[--i] = '-';
    put(b + i);
}

#define W 800
#define H 480

static u32 *fb;

static void rect(int x0, int y0, int w, int h, u32 c)
{
    for (int y = y0; y < y0 + h && y < H; y++)
        for (int x = x0; x < x0 + w && x < W; x++)
            if (x >= 0 && y >= 0) fb[y * W + x] = c;
}

/* 5x7 glyphs for the few characters we need, one byte per row, low 5 bits used */
static const struct { char ch; unsigned char rows[7]; } font[] = {
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
    {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},
};

static void text(int x, int y, const char *s, int scale, u32 c)
{
    for (; *s; s++, x += 6 * scale) {
        for (unsigned i = 0; i < sizeof font / sizeof font[0]; i++) {
            if (font[i].ch != *s) continue;
            for (int r = 0; r < 7; r++)
                for (int b = 0; b < 5; b++)
                    if (font[i].rows[r] & (0x10 >> b))
                        rect(x + b * scale, y + r * scale, scale, scale, c);
        }
    }
}

struct input_event { u32 sec, usec; u16 type, code; s32 value; };
struct pollfd { int fd; short events, revents; };

static const char *devs[] = {"/dev/input/TouchScreen", "/dev/keypad0", "/dev/input/event2"};

void _start_c(void)
{
    int fd = sc3(SYS_open, "/dev/fb0", 2, 0);
    if (fd < 0) { put("open fb0 failed\n"); sc3(SYS_exit, 1, 0, 0); }
    fb = (u32 *)sc6(SYS_mmap2, 0, W * H * 4, 3, 1, fd, 0);
    if ((long)fb < 0 && (long)fb > -4096) { put("mmap failed\n"); sc3(SYS_exit, 2, 0, 0); }

    /* background gradient, BGRA little-endian => 0x00RRGGBB */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            fb[y * W + x] = ((x * 255 / W) << 16) | ((y * 255 / H) << 8) | 0x40;
    rect(60, 140, 680, 200, 0x00101820);
    text(90, 170, "NATIVE CODE", 9, 0x00ffc43c);
    text(90, 260, "RUNS HERE!", 9, 0x0078dc8c);
    put("drawn; waiting for input events\n");

    struct pollfd p[3];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        int f = sc3(SYS_open, devs[i], 0, 0);
        put(devs[i]); put(f >= 0 ? " opened\n" : " not opened\n");
        if (f >= 0) { p[n].fd = f; p[n].events = 1; p[n].revents = 0; n++; }
    }
    int tx = -1, ty = -1;
    for (;;) {
        if (sc3(SYS_poll, p, n, -1) <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (!(p[i].revents & 1)) continue;
            struct input_event ev[16];
            long got = sc3(SYS_read, p[i].fd, ev, sizeof ev);
            for (long k = 0; k < got / (long)sizeof ev[0]; k++) {
                put("fd"); putnum(i); put(" type="); putnum(ev[k].type);
                put(" code="); putnum(ev[k].code); put(" value="); putnum(ev[k].value); put("\n");
                if (ev[k].type == 3 && ev[k].code == 0) tx = ev[k].value;
                if (ev[k].type == 3 && ev[k].code == 1) ty = ev[k].value;
                if (ev[k].type == 0 && tx >= 0 && ty >= 0 && tx < W && ty < H)
                    rect(tx - 4, ty - 4, 9, 9, 0x00ffffff);
            }
        }
    }
}

__asm__(".global _start\n_start:\n  mov fp, #0\n  bl _start_c\n");
