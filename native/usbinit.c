/*
 * usbinit: /sbin/init for the USB boot smoke test (ramdisk T48S.rfs).
 *
 * U-Boot's recovery mode boots <phonename>.bin with <phonename>.rfs as an ext2
 * ramdisk. This init:
 *   1. clears BOOTMODE in the userenv partition, exactly as Yealink's rcS does
 *      on every boot, so the next normal boot does not run usbmode.sh;
 *   2. draws a banner on the screen through /dev/mem (U-Boot leaves the LCD
 *      running with its framebuffer at 0x07A24000);
 *   3. prints an uptime counter on the serial port every second, to see whether
 *      a watchdog resets the board and after how long;
 *   4. echoes anything typed on the serial port, to prove the console works
 *      both ways outside Yealink's software.
 * It never exits (init exiting panics the kernel).
 */
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define FB_PHYS 0x07A24000
#define W 800
#define H 480

static int ser = -1;

static void say(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 1) n = sizeof b - 1;
    if (ser >= 0) write(ser, b, n);
}

static void cat_file(const char *path)
{
    char b[1024];
    int fd = open(path, O_RDONLY);
    if (fd < 0) { say("(cannot open %s)\r\n", path); return; }
    int n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n < 0) n = 0;
    b[n] = 0;
    say("%s: %s\r\n", path, b);
}

static int run(char *const argv[])
{
    pid_t p = fork();
    if (p == 0) {
        if (ser >= 0) { dup2(ser, 1); dup2(ser, 2); }
        execv(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* the same tiny 5x7 font as fbtest */
static const struct { char ch; unsigned char rows[7]; } font[] = {
    {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}}, {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}}, {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}}, {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}}, {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},
};

static void draw_banner(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { say("no /dev/mem, no banner\r\n"); return; }
    unsigned *fb = mmap(0, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, FB_PHYS);
    if (fb == MAP_FAILED) { say("mmap of the framebuffer failed\r\n"); return; }
    for (int i = 0; i < W * H; i++) fb[i] = 0x00103050;
    const char *s = "BOOTED FROM USB";
    int scale = 7, x = 60, y = 200;
    for (; *s; s++, x += 6 * scale)
        for (unsigned k = 0; k < sizeof font / sizeof font[0]; k++) {
            if (font[k].ch != *s) continue;
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (font[k].rows[r] & (0x10 >> c))
                        for (int dy = 0; dy < scale; dy++)
                            for (int dx = 0; dx < scale; dx++)
                                fb[(y + r * scale + dy) * W + x + c * scale + dx] = 0x00ffc43c;
        }
    say("banner drawn at 0x%08x\r\n", FB_PHYS);
}

int main(void)
{
    mount("proc", "/proc", "proc", 0, 0);
    ser = open("/dev/ttyAMA0", O_RDWR | O_NOCTTY);
    if (ser >= 0) {
        struct termios t;
        tcgetattr(ser, &t);
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag |= CLOCAL | CREAD;
        tcsetattr(ser, TCSANOW, &t);
    }
    say("\r\n\r\n===== T48S USB boot smoke test: our init is running =====\r\n");
    cat_file("/proc/cmdline");
    cat_file("/proc/version");

    say("BOOTMODE before: ");
    char *get[] = {"/usr/sbin/userenv", "-g", "BOOTMODE", 0};
    run(get);
    char *clr[] = {"/usr/sbin/userenv", "-s", "BOOTMODE", 0};
    say("clearing BOOTMODE: exit %d\r\nBOOTMODE after: ", run(clr));
    run(get);
    say("\r\n");

    draw_banner();
    say("type on the serial port and it will be echoed back\r\n");

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long last = -1;
    for (;;) {
        struct pollfd p = {ser, POLLIN, 0};
        if (poll(&p, ser >= 0 ? 1 : 0, 200) > 0) {
            char b[64];
            int n = read(ser, b, sizeof b);
            if (n > 0) { say("[echo] "); write(ser, b, n); say("\r\n"); }
        }
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        long s = t.tv_sec - t0.tv_sec;
        if (s != last) { last = s; say("alive %ld s\r\n", s); }
    }
}
