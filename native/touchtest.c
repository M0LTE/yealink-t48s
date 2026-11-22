// SPDX-License-Identifier: AGPL-3.0-or-later
/* touchtest: print what the touchscreen reports and draw a dot where it thinks the finger is.
 * Finds the input device by name ("T48S touchscreen" in our kernel, "YL tsc" in Yealink's),
 * maps raw 12-bit values to pixels with the tslib calibration in /etc/pointercal
 * (built-in numbers from U-Boot's calparam if that file is missing).
 * Usage: touchtest          until Ctrl-C */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define W 800
#define H 480
#define EVIOCGNAME64 0x80404506u

/* the kernel's 32-bit layout; musl's struct timeval is 64-bit, so spell it out */
struct input_event { unsigned sec, usec; unsigned short type, code; int value; };
enum { EV_SYN = 0, EV_KEY = 1, EV_ABS = 3, ABS_X = 0, ABS_Y = 1, ABS_PRESSURE = 24, BTN_TOUCH = 0x14a };

static long cal[7] = {14798, 13, -4134128, 53, 10207, -5441552, 65536};

static int open_touch(char *name)
{
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        name[0] = 0;
        ioctl(fd, EVIOCGNAME64, name);
        if (strstr(name, "touch") || strstr(name, "tsc")) {
            printf("%s: %s\n", path, name);
            return fd;
        }
        close(fd);
    }
    return -1;
}

int main(void)
{
    char name[64];
    int fd = open_touch(name);
    if (fd < 0) { fprintf(stderr, "no touchscreen input device found\n"); return 1; }

    FILE *f = fopen("/etc/pointercal", "r");
    if (f) {
        long c[7];
        if (fscanf(f, "%ld %ld %ld %ld %ld %ld %ld", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6]) == 7 && c[6])
            memcpy(cal, c, sizeof cal);
        fclose(f);
    }
    printf("calibration: %ld %ld %ld %ld %ld %ld %ld\n", cal[0], cal[1], cal[2], cal[3], cal[4], cal[5], cal[6]);

    unsigned *fb = MAP_FAILED;
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd >= 0) fb = mmap(0, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    printf("touch the screen%s; Ctrl-C to stop\n", fb == MAP_FAILED ? "" : " (a white dot is drawn at each position)");
    fflush(stdout);

    int x = 0, y = 0, p = 0, down = 0, was_down = 0;
    struct input_event ev;
    while (read(fd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type == EV_ABS && ev.code == ABS_X) x = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_Y) y = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_PRESSURE) p = ev.value;
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) down = ev.value;
        else if (ev.type == EV_SYN) {
            long sx = (cal[0] * x + cal[1] * y + cal[2]) / cal[6];
            long sy = (cal[3] * x + cal[4] * y + cal[5]) / cal[6];
            if (!down) {
                printf("up\n");
            } else {
                printf("%s raw %4d %4d pressure %4d -> screen %3ld,%3ld\n", was_down ? "move" : "down", x, y, p, sx, sy);
                if (fb != MAP_FAILED)
                    for (long j = sy - 3; j <= sy + 3; j++)
                        for (long i = sx - 3; i <= sx + 3; i++)
                            if (i >= 0 && i < W && j >= 0 && j < H) fb[j * W + i] = 0x00ffffff;
            }
            was_down = down;
            fflush(stdout);
        }
    }
    return 0;
}
