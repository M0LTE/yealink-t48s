/* fbinfo: print the framebuffer's geometry and test whether it can pan between pages.
 *   fbinfo            print info
 *   fbinfo pan N      show page N (sets yres_virtual to cover 4 pages if needed)
 * Built with musl (see build-dropbear.sh for the toolchain): $T/bin/armcc -Os -o fbinfo fbinfo.c
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY 0x4606

struct fb_bitfield { unsigned offset, length, msb_right; };
struct fb_var_screeninfo {
    unsigned xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    unsigned nonstd, activate, height, width, accel_flags;
    unsigned pixclock, left_margin, right_margin, upper_margin, lower_margin, hsync_len, vsync_len,
        sync, vmode, rotate, reserved[5];
};
struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    unsigned smem_len, type, type_aux, visual;
    unsigned short xpanstep, ypanstep, ywrapstep;
    unsigned line_length;
    unsigned long mmio_start;
    unsigned mmio_len, accel;
    unsigned short capabilities, reserved[2];
};

static void show(int fd)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    memset(&f, 0, sizeof f);
    if (ioctl(fd, FBIOGET_FSCREENINFO, &f) == 0)
        printf("fix: id=%.16s smem_start=0x%lx smem_len=%u line_length=%u xpanstep=%u ypanstep=%u ywrapstep=%u visual=%u\n",
               f.id, f.smem_start, f.smem_len, f.line_length, f.xpanstep, f.ypanstep, f.ywrapstep, f.visual);
    else
        perror("FBIOGET_FSCREENINFO");
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0)
        printf("var: %ux%u virtual %ux%u offset %u,%u bpp=%u red=%u/%u green=%u/%u blue=%u/%u transp=%u/%u activate=%u\n",
               v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.xoffset, v.yoffset, v.bits_per_pixel,
               v.red.offset, v.red.length, v.green.offset, v.green.length, v.blue.offset, v.blue.length,
               v.transp.offset, v.transp.length, v.activate);
    else
        perror("FBIOGET_VSCREENINFO");
}

int main(int argc, char **argv)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    show(fd);
    if (argc == 3 && !strcmp(argv[1], "pan")) {
        struct fb_var_screeninfo v;
        ioctl(fd, FBIOGET_VSCREENINFO, &v);
        unsigned page = atoi(argv[2]);
        if (v.yres_virtual < v.yres * (page + 1)) {
            v.yres_virtual = v.yres * 4;
            v.activate = 0;
            if (ioctl(fd, FBIOPUT_VSCREENINFO, &v)) perror("FBIOPUT_VSCREENINFO (yres_virtual)");
            ioctl(fd, FBIOGET_VSCREENINFO, &v);
        }
        v.yoffset = v.yres * page;
        v.xoffset = 0;
        if (ioctl(fd, FBIOPAN_DISPLAY, &v)) perror("FBIOPAN_DISPLAY");
        else printf("panned to page %u\n", page);
        show(fd);
    }
    return 0;
}
