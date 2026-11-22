// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * fbswap.so: LD_PRELOAD shim for Yealink's dskPhone.exx (Qt Embedded).
 *
 * Records where the process maps /dev/fb0. On signal FBSWAP_SIG it reads the
 * wanted mode from /tmp/fbswap.mode:
 *   'o' (ours):    the visible part of that mapping is replaced, at the same
 *                  address, by a tmpfs file (/tmp/fbswap.shadow). Qt keeps
 *                  drawing, unseen, and the real screen is free for our UI.
 *   'y' (yealink): the real framebuffer is mapped back and the shadow's
 *                  contents are copied onto the screen.
 * The shadow file always holds Yealink's latest picture while in 'o' mode.
 *
 * Freestanding (raw syscalls), so it does not care which glibc the host has.
 * Only errno comes from the host libc. Inactive in every process except dskPhone.
 */
typedef unsigned long size_t;
typedef long off_t;

#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_getpid 20
#define SYS_readlink 85
#define SYS_munmap 91
#define SYS_rt_sigaction 174
#define SYS_pread64 180
#define SYS_mmap2 192
#define SYS_ftruncate 93

#define PROT_RW 3
#define MAP_SHARED 1
#define MAP_FIXED 0x10
#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000

#define FB_VISIBLE (800 * 480 * 4) /* 1536000 bytes, exactly 375 pages */
#define FBSWAP_SIG 44               /* a realtime signal nothing else on the phone uses */

extern int *__errno_location(void);

static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r5 __asm__("r5") = f;
    register long r7 __asm__("r7") = n;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7) : "memory");
    return r0;
}
#define sc3(n, a, b, c) sc6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

static int active;       /* 1 in dskPhone */
static char *fb_base;    /* where dskPhone mapped /dev/fb0 */
static int mode = 'y';

static void logmsg(const char *s)
{
    int fd = sc3(SYS_open, "/tmp/fbswap.log", 02000 | 01 | O_CREAT, 0644); /* O_APPEND|O_WRONLY */
    if (fd < 0) return;
    long n = 0;
    while (s[n]) n++;
    sc3(SYS_write, fd, s, n);
    sc3(SYS_close, fd, 0, 0);
}

static int streq_prefix(const char *a, const char *b, long n)
{
    for (long i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int is_fb0(int fd)
{
    char path[32] = "/proc/self/fd/", link[32];
    int i = 14;
    if (fd >= 100) path[i++] = '0' + fd / 100;
    if (fd >= 10) path[i++] = '0' + fd / 10 % 10;
    path[i++] = '0' + fd % 10;
    path[i] = 0;
    long n = sc3(SYS_readlink, path, link, sizeof link - 1);
    return n == 8 && streq_prefix(link, "/dev/fb0", 8);
}

static void *real_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    long r = sc6(SYS_mmap2, (long)addr, len, prot, flags, fd, off >> 12);
    if (r < 0 && r > -4096) {
        *__errno_location() = -r;
        return (void *)-1;
    }
    return (void *)r;
}

static void swap_to(int want)
{
    if (!fb_base || want == mode) return;
    if (want == 'o') {
        int sf = sc3(SYS_open, "/tmp/fbswap.shadow", O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (sf < 0) { logmsg("shadow open failed\n"); return; }
        sc3(SYS_write, sf, fb_base, FB_VISIBLE); /* current picture into the shadow */
        long r = sc6(SYS_mmap2, (long)fb_base, FB_VISIBLE, PROT_RW, MAP_SHARED | MAP_FIXED, sf, 0);
        sc3(SYS_close, sf, 0, 0);
        if (r != (long)fb_base) { logmsg("map shadow failed\n"); return; }
        mode = 'o';
        logmsg("mode o\n");
    } else {
        int fb = sc3(SYS_open, "/dev/fb0", O_RDWR, 0);
        int sf = sc3(SYS_open, "/tmp/fbswap.shadow", O_RDONLY, 0);
        if (fb < 0 || sf < 0) { logmsg("reopen failed\n"); return; }
        long r = sc6(SYS_mmap2, (long)fb_base, FB_VISIBLE, PROT_RW, MAP_SHARED | MAP_FIXED, fb, 0);
        sc3(SYS_close, fb, 0, 0);
        if (r != (long)fb_base) { sc3(SYS_close, sf, 0, 0); logmsg("map fb failed\n"); return; }
        sc6(SYS_pread64, sf, (long)fb_base, FB_VISIBLE, 0, 0, 0); /* Yealink's latest picture back on screen */
        sc3(SYS_close, sf, 0, 0);
        mode = 'y';
        logmsg("mode y\n");
    }
}

static void on_signal(int sig)
{
    (void)sig;
    int saved = *__errno_location();
    char c = 'y';
    int fd = sc3(SYS_open, "/tmp/fbswap.mode", O_RDONLY, 0);
    if (fd >= 0) {
        sc3(SYS_read, fd, &c, 1);
        sc3(SYS_close, fd, 0, 0);
    }
    swap_to(c == 'o' ? 'o' : 'y');
    *__errno_location() = saved;
}

struct k_sigaction { void (*handler)(int); unsigned long flags; void (*restorer)(void); unsigned long mask[2]; };

static void write_pid(void)
{
    char b[16];
    int i = 15;
    long p = sc3(SYS_getpid, 0, 0, 0);
    b[i] = '\n';
    do { b[--i] = '0' + p % 10; p /= 10; } while (p);
    int fd = sc3(SYS_open, "/tmp/fbswap.pid", 01 | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { sc3(SYS_write, fd, b + i, 16 - i); sc3(SYS_close, fd, 0, 0); }
}

__attribute__((constructor)) static void init(void)
{
    char cmd[64];
    int fd = sc3(SYS_open, "/proc/self/cmdline", O_RDONLY, 0);
    if (fd < 0) return;
    long n = sc3(SYS_read, fd, cmd, sizeof cmd - 1);
    sc3(SYS_close, fd, 0, 0);
    if (n <= 0) return;
    cmd[n] = 0;
    for (long i = 0; i + 9 <= n; i++)
        if (streq_prefix(cmd + i, "dskPhone", 8)) active = 1;
    if (!active) return;
    struct k_sigaction sa = {on_signal, 0x10000000 /* SA_RESTART */, 0, {0, 0}};
    sc6(SYS_rt_sigaction, FBSWAP_SIG, (long)&sa, 0, 8, 0, 0);
    write_pid();
    logmsg("fbswap loaded in dskPhone\n");
}

static void *hooked(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    void *p = real_mmap(addr, len, prot, flags, fd, off);
    if (active && p != (void *)-1 && !fb_base && off == 0 && len >= FB_VISIBLE && is_fb0(fd)) {
        fb_base = p;
        logmsg("fb0 mapping recorded\n");
    }
    return p;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    return hooked(addr, len, prot, flags, fd, off);
}

void *mmap64(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    return hooked(addr, len, prot, flags, fd, (off_t)off);
}
