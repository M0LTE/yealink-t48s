/* cpubench: the same CPU-bound work timed under any kernel, to compare CPU speed
 * between Yealink's kernel and ours (issue #9). Static, plain ASCII output.
 *   delay    the kernel's delay loop (subs/bhi), at four code alignments
 *   nopN     the same loop padded with N register moves: (time per pass) - (delay)
 *            gives the cost of one instruction, hence the core clock
 *   mix      integer work in the L1 cache (xorshift, multiply, table lookups)
 *   mem      32 MB of memcpy through a 4 MB buffer (caches too small: bus and RAM speed)
 * Each line: name, loops, microseconds of CPU time, million loops per second. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>

/* CPU time used by this process (user + system), so a busy system (Yealink's UI)
 * does not skew the result; 10 ms steps, against runs of about a second */
static long long now(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000LL + ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
}

#define LOOP(name, align, body) \
    __attribute__((naked, noinline)) static void name(unsigned n) \
    { __asm__(".balign 32\n" align "1: subs r0, r0, #1\n" body "bhi 1b\nbx lr\n"); }
#define M "mov r1, r1\n"
LOOP(delay0, "", "")
LOOP(delay4, "nop\n", "")
LOOP(delay8, "nop\nnop\n", "")
LOOP(delay12, "nop\nnop\nnop\n", "")
LOOP(nop4, "", M M M M)
LOOP(nop16, "", M M M M M M M M M M M M M M M M)

static unsigned tab[1024];
__attribute__((noinline)) static unsigned mix(unsigned n)
{
    unsigned x = 2463534242u, s = 0;
    while (n--) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        s += tab[x & 1023] * (x | 1);
        tab[(x >> 10) & 1023] = s;
    }
    return s;
}

static void report(const char *name, unsigned loops, long long us)
{
    printf("%-8s %10u loops %8lld us %8.2f Mloops/s\n", name, loops, us, (double)loops / us);
}

#define RUN(f, n) do { long long t = now(); f(n); report(#f, n, now() - t); } while (0)

int main(void)
{
    static char a[4 << 20], b[4 << 20];
    unsigned i, r;
    long long t;
    for (i = 0; i < 3; i++) {
        RUN(delay0, 200000000u);
        RUN(delay4, 200000000u);
        RUN(delay8, 200000000u);
        RUN(delay12, 200000000u);
        RUN(nop4, 100000000u);
        RUN(nop16, 50000000u);
        t = now(); r = mix(30000000u); report("mix", 30000000u, now() - t);
        memset(a, r, sizeof a);
        t = now();
        for (r = 0; r < 8; r++) { memcpy(b, a, sizeof a); a[r] = b[r + 1]; }
        t = now() - t;
        printf("%-8s %10u MB    %8lld us %8.2f MB/s\n", "mem", 32u, t, 32e6 / t);
    }
    return 0;
}
