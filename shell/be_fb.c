// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell on the phone: the framebuffer, and the keypad, hook switch and
 * touchscreen input devices of our kernel (kernel/README.md), all grabbed so
 * nothing else sees them.
 *
 * The shell draws one picture of SW x SH pixels, 32 bits each. On the T48S that
 * is the screen, and rows are copied to it as they are. A phone with another
 * screen (the T46S: 480x272, 16 bits, mounted upside down) gets the same
 * picture made smaller, each screen pixel the average of the picture's pixels
 * under it, turned if the device tree says so. See docs/models.md. */
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "shell.h"

/* The kernel's event layout. Not musl's struct input_event: on 32-bit ARM that
 * one has a 64-bit time and is 24 bytes, the kernel's is 16. */
struct kev { unsigned long sec, usec; unsigned short type, code; int value; };

#define KDSETMODE 0x4B3A
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
struct fb_var_min { unsigned xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, rest[33]; };
struct fb_fix_min {
    char id[16];
    unsigned long smem_start;
    unsigned smem_len, type, type_aux, visual;
    unsigned short xpanstep, ypanstep, ywrapstep;
    unsigned line_length;
    unsigned long mmio_start;
    unsigned mmio_len, accel;
    unsigned short capabilities, reserved[2];
};

enum { DEV_KEYS, DEV_HOOK, DEV_TOUCH, NDEV };
static int dev[NDEV] = {-1, -1, -1};
static uint32_t *fb;
static unsigned fb_stride;
/* a screen that is not SW x SH at 32 bits: its size, depth, bytes per row, and whether it is upside down */
static int scaled, pw, ph, pbpp, pline, turned;
static long cal[7] = {14798, 13, -4134128, 53, 10207, -5441552, 65536};  /* factory values, from U-Boot */
static int raw_x, raw_y, touching, was_touching, last_x, last_y;

static int key_code(int code)
{
    switch (code) {
    case KEY_0: return '0';
    case KEY_1: return '1';
    case KEY_2: return '2';
    case KEY_3: return '3';
    case KEY_4: return '4';
    case KEY_5: return '5';
    case KEY_6: return '6';
    case KEY_7: return '7';
    case KEY_8: return '8';
    case KEY_9: return '9';
    case KEY_KPASTERISK: return T48_KEY_STAR;
    case KEY_NUMERIC_POUND: return T48_KEY_HASH;
    case KEY_UP: return T48_KEY_UP;
    case KEY_DOWN: return T48_KEY_DOWN;
    case KEY_LEFT: return T48_KEY_LEFT;
    case KEY_RIGHT: return T48_KEY_RIGHT;
    case KEY_ENTER: return T48_KEY_OK;
    case KEY_BACKSPACE: return T48_KEY_BACK;  /* the X key */
    case KEY_VOLUMEDOWN: return SK_VOLDOWN;
    case KEY_VOLUMEUP: return SK_VOLUP;
    case KEY_PROG1: return SK_HEADSET;
    case KEY_MUTE: return SK_MUTE;
    case KEY_MESSENGER: return SK_MESSAGE;
    case KEY_PAUSE: return SK_HOLD;
    case KEY_AGAIN: return SK_REDIAL;
    case KEY_FORWARD: return SK_TRANSFER;
    case KEY_PROG2: return SK_SPEAKER;
    /* the T46S's soft keys, left to right: KEY_MACRO1 to 4, by number as the
     * root filesystem's kernel headers are older than the names */
    case 0x290: return SK_SOFT1;
    case 0x291: return SK_SOFT2;
    case 0x292: return SK_SOFT3;
    case 0x293: return SK_SOFT4;
    default: return 0;
    }
}

/* The device tree's word on how the panel is mounted: 0, or 180 for upside
 * down. T48_ROTATE in the environment overrides it, for trying. */
static int fb_rotation(void)
{
    const char *e = getenv("T48_ROTATE");
    glob_t g;
    int rot = 0;

    if (e) return atoi(e);
    if (glob("/proc/device-tree/framebuffer*/rotation", 0, NULL, &g) == 0 && g.gl_pathc) {
        unsigned char b[4];
        FILE *f = fopen(g.gl_pathv[0], "rb");
        if (f) {
            if (fread(b, 1, 4, f) == 4) rot = b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
            fclose(f);
        }
    }
    globfree(&g);
    return rot;
}

static int fb_init(void)
{
    int fd = open(opt.fb, O_RDWR);
    struct fb_var_min v;
    struct fb_fix_min f;

    if (fd < 0) { perror(opt.fb); return -1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror("framebuffer ioctl"); return -1; }
    turned = fb_rotation() == 180;
    if (v.xres < SW || v.yres < SH || v.bits_per_pixel != 32 || turned) {
        if ((v.bits_per_pixel != 32 && v.bits_per_pixel != 16) || v.xres < 160 || v.yres < 90 || v.xres > SW || v.yres > SH) {
            fprintf(stderr, "%s is %ux%u at %u bpp; need %dx%d or smaller, at 32 or 16 bpp\n", opt.fb, v.xres, v.yres, v.bits_per_pixel, SW, SH);
            return -1;
        }
        scaled = 1; pw = (int)v.xres; ph = (int)v.yres; pbpp = (int)v.bits_per_pixel; pline = (int)f.line_length;
        logmsg("screen %dx%d at %d bpp%s: the %dx%d picture is fitted to it", pw, ph, pbpp, turned ? ", upside down" : "", SW, SH);
    }
    fb_stride = f.line_length / 4;
    fb = mmap(0, (size_t)f.line_length * (scaled ? (size_t)ph : SH), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mapping the framebuffer"); return -1; }
    /* keep the text console from drawing over us */
    int tty = open("/dev/tty0", O_RDWR);
    if (tty >= 0) { ioctl(tty, KDSETMODE, 1); close(tty); }

    /* find the input devices by name, as the event numbers are not promised */
    for (int i = 0; i < 16; i++) {
        char path[32], name[64] = "";
        int which = -1;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int d = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (d < 0) continue;
        ioctl(d, EVIOCGNAME(sizeof name - 1), name);
        if (strstr(name, "keypad")) which = DEV_KEYS;
        else if (strstr(name, "hook")) which = DEV_HOOK;
        else if (strstr(name, "touch") || strstr(name, "tsc")) which = DEV_TOUCH;
        if (which < 0 || dev[which] >= 0) { close(d); continue; }
        if (ioctl(d, EVIOCGRAB, 1) < 0) logmsg("%s (%s): could not be grabbed, something else has it", path, name);
        dev[which] = d;
        logmsg("%s: %s", path, name);
    }
    if (dev[DEV_KEYS] < 0) logmsg("no keypad found: the keys will not work");
    if (dev[DEV_HOOK] < 0) logmsg("no hook switch found");
    if (dev[DEV_TOUCH] < 0) logmsg("no touchscreen found");

    FILE *pc = fopen("/etc/pointercal", "r");
    if (pc) {
        long c[7];
        if (fscanf(pc, "%ld %ld %ld %ld %ld %ld %ld", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6]) == 7 && c[6])
            memcpy(cal, c, sizeof cal);
        fclose(pc);
    }

    /* a handset that is already off the hook sends no event: ask */
    if (dev[DEV_HOOK] >= 0) {
        unsigned char bits[KEY_MAX / 8 + 1] = {0};
        if (ioctl(dev[DEV_HOOK], EVIOCGKEY(sizeof bits), bits) >= 0 && (bits[KEY_PHONE / 8] & (1 << (KEY_PHONE % 8))))
            shell_hook(1);
    }
    /* the kernel sends a coordinate only when it changes, so a touch on a line
     * with the last one before this program started would come without it: ask
     * where that last one was (seen on the phone after the shell was restarted) */
    if (dev[DEV_TOUCH] >= 0) {
        struct input_absinfo a;
        if (ioctl(dev[DEV_TOUCH], EVIOCGABS(ABS_X), &a) >= 0) raw_x = a.value;
        if (ioctl(dev[DEV_TOUCH], EVIOCGABS(ABS_Y), &a) >= 0) raw_y = a.value;
    }
    return 0;
}

static void fb_flush(const uint32_t *src, int stride, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    if (w <= 0 || h <= 0) return;
    if (scaled) {
        /* the screen pixels that any of this part of the picture falls on */
        int dx0 = x * pw / SW, dx1 = ((x + w) * pw + SW - 1) / SW;
        int dy0 = y * ph / SH, dy1 = ((y + h) * ph + SH - 1) / SH;
        for (int dy = dy0; dy < dy1; dy++) {
            int sy0 = dy * SH / ph, sy1 = ((dy + 1) * SH + ph - 1) / ph;
            int ty = turned ? ph - 1 - dy : dy;
            unsigned char *row = (unsigned char *)fb + (size_t)ty * pline;
            for (int dx = dx0; dx < dx1; dx++) {
                int sx0 = dx * SW / pw, sx1 = ((dx + 1) * SW + pw - 1) / pw;
                unsigned r = 0, g = 0, b = 0, n = 0;
                for (int sy = sy0; sy < sy1; sy++) {
                    const uint32_t *sp = src + sy * stride;
                    for (int sx = sx0; sx < sx1; sx++, n++) {
                        r += sp[sx] >> 16 & 255; g += sp[sx] >> 8 & 255; b += sp[sx] & 255;
                    }
                }
                r /= n; g /= n; b /= n;
                int tx = turned ? pw - 1 - dx : dx;
                if (pbpp == 16) ((uint16_t *)row)[tx] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
                else ((uint32_t *)row)[tx] = r << 16 | g << 8 | b;
            }
        }
        return;
    }
    for (int j = 0; j < h; j++)
        memcpy(fb + (y + j) * fb_stride + x, src + (y + j) * stride + x, (size_t)w * 4);
}

static int fb_fds(struct pollfd *p, int max)
{
    int n = 0;
    for (int i = 0; i < NDEV && n < max; i++)
        if (dev[i] >= 0) { p[n].fd = dev[i]; p[n].events = POLLIN; p[n++].revents = 0; }
    return n;
}

static void touch_event(const struct kev *ev)
{
    if (ev->type == EV_ABS && ev->code == ABS_X) raw_x = ev->value;
    else if (ev->type == EV_ABS && ev->code == ABS_Y) raw_y = ev->value;
    else if (ev->type == EV_KEY && ev->code == BTN_TOUCH) touching = ev->value;
    else if (ev->type == EV_SYN) {
        /* raw 12-bit readings to pixels, tslib style */
        long x = (cal[0] * raw_x + cal[1] * raw_y + cal[2]) / cal[6];
        long y = (cal[3] * raw_x + cal[4] * raw_y + cal[5]) / cal[6];
        if (x < 0) x = 0;
        if (x >= SW) x = SW - 1;
        if (y < 0) y = 0;
        if (y >= SH) y = SH - 1;
        if (touching && !was_touching) shell_touch(T48_TOUCH_DOWN, (int)x, (int)y);
        else if (touching && (x != last_x || y != last_y)) shell_touch(T48_TOUCH_MOVE, (int)x, (int)y);
        else if (!touching && was_touching) shell_touch(T48_TOUCH_UP, last_x, last_y);
        if (touching) { last_x = (int)x; last_y = (int)y; }
        was_touching = touching;
    }
}

static void fb_handle(const struct pollfd *p, int n)
{
    for (int i = 0; i < n; i++) {
        struct kev ev;
        if (!(p[i].revents & POLLIN)) continue;
        while (read(p[i].fd, &ev, sizeof ev) == sizeof ev) {
            if (p[i].fd == dev[DEV_TOUCH]) {
                touch_event(&ev);
            } else if (ev.type == EV_KEY && ev.value != 2) {  /* 2 is auto-repeat */
                if (p[i].fd == dev[DEV_HOOK]) {
                    if (ev.code == KEY_PHONE) shell_hook(ev.value);
                } else {
                    int code = key_code(ev.code);
                    if (code) shell_key(code, ev.value);
                    else logmsg("key code %d has no meaning yet", ev.code);
                }
            }
        }
    }
}

static int fb_timeout(long long now) { (void)now; return -1; }
static void fb_tick(long long now) { (void)now; }

static void fb_fini(void)
{
    int tty = open("/dev/tty0", O_RDWR);
    if (tty >= 0) { ioctl(tty, KDSETMODE, 0); close(tty); }
}

const struct backend be_fb = {"phone", fb_init, fb_flush, fb_fds, fb_handle, fb_timeout, fb_tick, fb_fini};
