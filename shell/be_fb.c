// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell on the phone: the framebuffer, and the keypad, hook switch and
 * touchscreen input devices of our kernel (kernel/README.md), all grabbed so
 * nothing else sees them.
 *
 * The shell draws one picture the size of the screen, 32 bits a pixel. On the
 * T48S rows are copied to the screen as they are. A phone with another screen
 * (the T46S: 480x272, 16 bits, mounted upside down) is drawn for at its own
 * size, and each row is packed to 16 bits and turned on the way (fit.c). An
 * app that asks for the T48S's 800x480 there has its picture fitted. See
 * docs/models.md. */
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "shell.h"

/* The kernel's event layout. Not musl's struct input_event: on 32-bit ARM that
 * one has a 64-bit time and is 24 bytes, the kernel's is 16. */
struct kev { unsigned long sec, usec; unsigned short type, code; int value; };

/* the kernel's page flipping (kernel6/drivers/t48s_flip.c) */
struct flip_info { uint32_t width, height, stride, count, size, period_ns; };
struct flip_event { int32_t buffer; uint32_t frame; uint64_t time_ns; };
#define T48FLIP_INFO _IOR('T', 0x48, struct flip_info)
#define T48FLIP_SHOW _IOW('T', 0x49, int32_t)
#define T48FLIP_CLEAR _IO('T', 0x4a)
#define EVIOCSCLOCKID_ _IOW('E', 0xa0, int)

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
/* the panel: its size, depth, bytes per row, and whether it is upside down */
static int pw, ph, pbpp, pline, turned;
static long cal[7] = {14798, 13, -4134128, 53, 10207, -5441552, 65536};  /* factory values, from U-Boot */
static int raw_x, raw_y, touching, was_touching, last_x, last_y;
/* the flip device, what it offers, and what the screen is showing: a flip buffer, or -1 for the frame we draw in */
static int flip_fd = -1, flip_showing = -1;
static const char *flip_dev = "/dev/t48flip";
static struct flip_info flip;
static int timed;   /* the input devices stamp events with CLOCK_MONOTONIC */
static int scan;    /* the keypad's MSC_SCAN that came last, 0 once its key event has: the key's place in the matrix */

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
    if ((v.bits_per_pixel != 32 && v.bits_per_pixel != 16) || v.xres < 320 || v.yres < 200) {
        fprintf(stderr, "%s is %ux%u at %u bpp; need 320x200 or more, at 32 or 16 bpp\n", opt.fb, v.xres, v.yres, v.bits_per_pixel);
        return -1;
    }
    /* a screen bigger than the T48S's is used as far as that */
    pw = v.xres > APP_W ? APP_W : (int)v.xres;
    ph = v.yres > APP_H ? APP_H : (int)v.yres;
    pbpp = (int)v.bits_per_pixel;
    pline = (int)f.line_length;
    logmsg("screen %dx%d at %d bpp%s", pw, ph, pbpp, turned ? ", upside down" : "");
    fb = mmap(0, (size_t)f.line_length * (size_t)ph, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mapping the framebuffer"); return -1; }
    /* The kernel's spare frames, for apps that flip. Only on a screen that shows a
     * picture as an app draws it: an app's buffer could not be shown otherwise. T48_FLIP_DEV
     * names another device, or none ("") to try the copying path on the phone. */
    const char *fdev = getenv("T48_FLIP_DEV");
    if (fdev) flip_dev = fdev;
    if (pbpp == 32 && !turned && (flip_fd = open(flip_dev, O_RDWR | O_NONBLOCK | O_CLOEXEC)) >= 0) {
        int32_t home = -1;
        if (ioctl(flip_fd, T48FLIP_INFO, &flip) < 0 || (int)flip.width != pw || (int)flip.height != ph || (int)flip.stride != pw * 4 || flip.count < 2) {
            close(flip_fd);
            flip_fd = -1;
        } else {
            ioctl(flip_fd, T48FLIP_SHOW, &home);   /* whatever a shell before us left showing */
            logmsg("flipping: %u buffers, refresh every %u.%03u ms", flip.count, flip.period_ns / 1000000, flip.period_ns / 1000 % 1000);
        }
    }
    if (flip_fd < 0) logmsg("flipping: by copying");
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
        int mono = CLOCK_MONOTONIC;
        if (which == DEV_KEYS) timed = ioctl(d, EVIOCSCLOCKID_, &mono) == 0;
        else ioctl(d, EVIOCSCLOCKID_, &mono);
        dev[which] = d;
        logmsg("%s: %s", path, name);
    }
    if (dev[DEV_KEYS] < 0) logmsg("no keypad found: the keys will not work");
    if (dev[DEV_HOOK] < 0) logmsg("no hook switch found");
    if (dev[DEV_TOUCH] < 0) logmsg("no touchscreen found");
    /* The shell draws at the screen's own size. T48_FIT in the environment has it draw the T48S's
     * 800x480 and fit that to the screen, as it did before it had a layout for small screens: for
     * comparing. Soft keys: the keypad says it has them (KEY_MACRO1), as on the T46S. */
    unsigned char kb[KEY_MAX / 8 + 1] = {0};
    int soft = dev[DEV_KEYS] >= 0 && ioctl(dev[DEV_KEYS], EVIOCGBIT(EV_KEY, sizeof kb), kb) >= 0 &&
               (kb[KEY_MACRO1 / 8] >> (KEY_MACRO1 % 8) & 1);
    if (getenv("T48_FIT")) shell_screen_is(APP_W, APP_H, dev[DEV_TOUCH] >= 0, soft);
    else shell_screen_is(pw, ph, dev[DEV_TOUCH] >= 0, soft);

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

static void fb_flush(const uint32_t *src, int stride, int sw, int sh, int x, int y, int w, int h)
{
    fit_put(fb, pline, pw, ph, pbpp, turned, src, stride, sw, sh, x, y, w, h);
}

static int fb_fds(struct pollfd *p, int max)
{
    int n = 0;
    for (int i = 0; i < NDEV && n < max; i++)
        if (dev[i] >= 0) { p[n].fd = dev[i]; p[n].events = POLLIN; p[n++].revents = 0; }
    if (flip_fd >= 0 && n < max) { p[n].fd = flip_fd; p[n].events = POLLIN; p[n++].revents = 0; }
    return n;
}

/* The app gets an open of its own. Ours it must never hold: flags such as
 * O_NONBLOCK belong to the open, and an app that cleared them on a shared one
 * would have the shell wait in read() for ever. */
static int fb_flip_info(int *fd, int *count, size_t *size, unsigned *period_us)
{
    if (flip_fd < 0 || (*fd = open(flip_dev, O_RDWR | O_CLOEXEC)) < 0) return -1;
    *count = (int)flip.count;
    *size = flip.size;
    *period_us = flip.period_ns / 1000;
    return 0;
}

static void fb_flip_show(int n)
{
    int32_t b = n;
    if (flip_fd < 0 || (n < 0 && flip_showing < 0)) return;
    if (ioctl(flip_fd, T48FLIP_SHOW, &b) == 0) flip_showing = n;
}

static void fb_flip_clear(void)
{
    if (flip_fd >= 0) ioctl(flip_fd, T48FLIP_CLEAR);
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
        if (p[i].fd == flip_fd) {
            /* one read for one POLLIN: the driver keeps only the latest, and this must never wait */
            struct flip_event fe;
            if (read(flip_fd, &fe, sizeof fe) == sizeof fe && fe.buffer >= 0)
                shell_flipped(fe.buffer, fe.frame, (long long)(fe.time_ns / 1000));
            continue;
        }
        while (read(p[i].fd, &ev, sizeof ev) == sizeof ev) {
            shell_input_us = timed ? (long long)ev.sec * 1000000 + (long long)ev.usec : t48_now_us();
            if (p[i].fd == dev[DEV_TOUCH]) {
                touch_event(&ev);
            } else if (p[i].fd == dev[DEV_HOOK]) {
                if (ev.type == EV_KEY && ev.code == KEY_PHONE && ev.value != 2) shell_hook(ev.value);
            } else if (ev.type == EV_MSC && ev.code == MSC_SCAN) {
                if (scan) shell_scan(scan, 0, 0, -1);
                scan = ev.value;
            } else if (ev.type == EV_KEY && ev.value != 2) {  /* 2 is auto-repeat */
                shell_scan(scan, ev.code, key_code(ev.code), ev.value);
                scan = 0;
            } else if (ev.type == EV_SYN && scan) {
                /* a place with no key event: a kernel that sends none for what its table does not hold */
                shell_scan(scan, 0, 0, -1);
                scan = 0;
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

const struct backend be_fb = {"phone", fb_init, fb_flush, fb_fds, fb_handle, fb_timeout, fb_tick, fb_fini,
                              fb_flip_info, fb_flip_show, fb_flip_clear};
