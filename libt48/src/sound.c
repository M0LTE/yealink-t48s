// SPDX-License-Identifier: AGPL-3.0-or-later
/* libt48: sound (API level 4, docs/app-protocol.md). The app's samples go
 * into a ring in memory shared with the audio service, which takes them 20 ms
 * at a time: no socket, and no copy but the one into the ring. When nobody is
 * taking (the app is off the screen, a shell with no audio service, the
 * simulator) the samples are thrown away at the rate they would have played,
 * so a program that keeps time by its sound keeps the same time.
 *
 * And a resampler, since the ring takes one format only. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include "t48.h"

#define STALE_US 250000   /* a full ring that has not moved for this long has nobody taking from it */
#define HALF_TICK 160     /* the service takes every 320 samples: on average a sample waits half of that */

/* volatile: the audio service writes to it behind our back */
static volatile struct t48_sound_ring *ring, *mapped;   /* mapped once; ring is NULL while sound is closed */
static int16_t *data;
static uint32_t frames, mask, wr;
static int limit = 1024;          /* samples that may wait */
static uint32_t seen_rd;          /* for telling a ring that has stopped moving */
static long long seen_at;
static int stale;
static long long off_until;       /* nobody taking: when what has been "written" would have been played, microseconds */

int t48_sound_open(void)
{
    const char *fd = getenv(T48_ENV_SOUND);
    struct t48_sound_ring h;
    struct stat st;

    if (ring) return 0;
    if (mapped) { ring = mapped; wr = ring->wr; return 0; }
    if (!fd || fstat(atoi(fd), &st) < 0 || st.st_size < (off_t)sizeof h) return -1;
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, atoi(fd), 0);
    if (m == MAP_FAILED) return -1;
    memcpy(&h, m, sizeof h);
    if (h.magic != T48_SOUND_MAGIC || h.rate != T48_SOUND_RATE || h.frames < 1024 || (h.frames & (h.frames - 1)) ||
        h.data < sizeof h || h.data > (size_t)st.st_size || h.frames > ((size_t)st.st_size - h.data) / 2) {
        munmap(m, (size_t)st.st_size);
        return -1;
    }
    ring = mapped = m;
    data = (int16_t *)((char *)m + h.data);
    frames = h.frames;
    mask = frames - 1;
    wr = ring->wr;
    seen_rd = ring->rd;
    seen_at = t48_now_us();
    stale = 0;
    off_until = 0;
    if (limit > (int)frames) limit = (int)frames;
    return 0;
}

void t48_sound_close(void)
{
    /* the mapping stays: the ring is the app's for as long as it lives, and what is in it plays out */
    ring = NULL;
}

int t48_sound_rate(void) { return T48_SOUND_RATE; }

void t48_sound_buffer(int samples)
{
    limit = samples < 320 ? 320 : samples;
    if (ring && limit > (int)frames) limit = (int)frames;
}

/* Is anybody taking from the ring? Not while the service says so, and not if
 * the ring has been full and still for a while (the service has gone). */
static int live(long long now)
{
    uint32_t rd = ring->rd;
    if (rd != seen_rd) { seen_rd = rd; seen_at = now; stale = 0; }
    else if (wr - rd < (uint32_t)limit) seen_at = now;   /* room left: it has had no reason to move */
    else if (now - seen_at > STALE_US) stale = 1;
    return ring->state != T48_SOUND_OFF && !stale;
}

/* samples waiting: in the ring, or (nobody taking) in the ring we pretend to have */
static int queued(long long now)
{
    if (live(now)) {
        uint32_t q = wr - ring->rd;
        off_until = 0;
        return q > frames ? (int)frames : (int)q;
    }
    if (off_until <= now) return 0;
    return (int)((off_until - now) * T48_SOUND_RATE / 1000000);
}

int t48_sound_queued(void) { return ring ? queued(t48_now_us()) : 0; }

int t48_sound_free(void)
{
    if (!ring) return 0;
    int q = queued(t48_now_us());
    return q >= limit ? 0 : limit - q;
}

int t48_sound_playing(void) { return ring && ring->state == T48_SOUND_PLAYING && !stale; }
unsigned t48_sound_dry(void) { return ring ? ring->dry : 0; }

int t48_sound_latency_us(void)
{
    if (!ring) return 0;
    long long now = t48_now_us();
    int q = queued(now);
    if (!live(now)) return (int)((long long)q * 1000000 / T48_SOUND_RATE);
    return (int)((long long)(q + HALF_TICK + (int)ring->delay) * 1000000 / T48_SOUND_RATE);
}

static void put(const int16_t *s, int n)
{
    uint32_t at = wr & mask, first = frames - at;
    if (first > (uint32_t)n) first = (uint32_t)n;
    memcpy(data + at, s, first * sizeof *s);
    memcpy(data, s + first, ((uint32_t)n - first) * sizeof *s);
    wr += (uint32_t)n;
    __asm__ __volatile__("" ::: "memory");   /* the samples first, then the count that says they are there */
    ring->wr = wr;
}

int t48_sound_put(const int16_t *s, int n)
{
    if (!ring || n <= 0) return 0;
    long long now = t48_now_us();
    int q = queued(now), room = limit - q;
    if (room <= 0) return 0;
    if (n > room) n = room;
    if (live(now)) {
        put(s, n);
    } else {
        /* thrown away, and counted as if it were playing */
        if (off_until < now) off_until = now;
        off_until += (long long)n * 1000000 / T48_SOUND_RATE;
        wr = ring->wr;
    }
    return n;
}

/* sleep until the service has taken some, or timeout_us has passed */
static void wait_rd(uint32_t rd, long long timeout_us)
{
#ifdef SYS_futex_time64
    struct { int64_t sec, nsec; } ts = {timeout_us / 1000000, timeout_us % 1000000 * 1000};
    long nr = SYS_futex_time64;
#else
    struct timespec ts = {(time_t)(timeout_us / 1000000), (long)(timeout_us % 1000000 * 1000)};
    long nr = SYS_futex;
#endif
    ring->waiting = 1;
    if (ring->rd != rd) return;   /* it moved before we said we were waiting: the kernel checks once more itself */
    if (syscall(nr, (void *)&ring->rd, FUTEX_WAIT, rd, &ts, NULL, 0) < 0 && errno == ENOSYS) {
        struct timespec nap = {0, 5000000};
        nanosleep(&nap, NULL);
    }
}

int t48_sound_write(const int16_t *s, int n)
{
    int done = 0;
    if (!ring) return -1;
    while (done < n) {
        int k = t48_sound_put(s + done, n - done);
        done += k;
        if (done == n) break;
        if (k) continue;
        long long now = t48_now_us();
        if (live(now)) {
            wait_rd(ring->rd, 50000);
        } else {
            /* nobody taking: wait as long as the sound would have taken to make room */
            int over = queued(now) - limit + 1;
            struct timespec ts = {0, (over > 0 ? over : 1) * (1000000000L / T48_SOUND_RATE)};
            if (ts.tv_nsec > 50000000) ts.tv_nsec = 50000000;
            nanosleep(&ts, NULL);
        }
    }
    return n;
}

/* ---------- resampling ---------- */

void t48_resample_init(t48_resampler *r, int rate, int channels)
{
    memset(r, 0, sizeof *r);
    r->channels = channels == 2 ? 2 : 1;
    r->rate = rate < 4000 ? 4000 : rate > 96000 ? 96000 : rate;
    /* one output sample is this much input, 1 << 13 a sample */
    r->step = (int32_t)(((int64_t)r->rate << 13) / T48_SOUND_RATE);
    r->need = r->step;
    r->inv = (int32_t)(((int64_t)1 << 28) / r->step);
}

int t48_resample_max(const t48_resampler *r, int in_frames)
{
    return (int)(((int64_t)in_frames << 13) / r->step) + 2;   /* from step: it is truncated, so the output can run a little over the nominal ratio */
}

int t48_resample(t48_resampler *r, const int16_t *in, int in_frames, int16_t *out, int cap)
{
    int n = 0;
    const int one = 1 << 13;

    if (r->rate == T48_SOUND_RATE) {
        if (in_frames > cap) in_frames = cap;
        if (r->channels == 1) memcpy(out, in, (size_t)in_frames * sizeof *out);
        else for (int i = 0; i < in_frames; i++) out[i] = (int16_t)((in[2 * i] + in[2 * i + 1]) >> 1);
        return in_frames;
    }
    for (int i = 0; i < in_frames; i++) {
        int32_t x = r->channels == 1 ? in[i] : (in[2 * i] + in[2 * i + 1]) >> 1;
        if (r->step >= 2 * one) {
            /* from 32000 up there is room to smooth first (1 2 1): what folds back is 6 to 30 dB lower for it,
             * and the top of what is kept loses 2 dB */
            int32_t raw = x;
            x = (r->p2 + 2 * r->p1 + x) >> 2;
            r->p2 = r->p1;
            r->p1 = raw;
        }
        if (r->step >= one) {
            /* down: each output sample is the mean of the input it covers, part samples counted in part.
             * That takes off most of what would fold back as a whistle. */
            int32_t w = one;
            while (w >= r->need) {
                if (n == cap) return n;
                r->acc += x * r->need;
                out[n++] = (int16_t)(((int64_t)r->acc * r->inv + (1 << 27)) >> 28);
                r->acc = 0;
                w -= r->need;
                r->need = r->step;
            }
            r->acc += x * w;
            r->need -= w;
        } else {
            /* up: a straight line from the last sample to this one */
            while (r->need <= one) {
                if (n == cap) return n;
                out[n++] = (int16_t)(r->last + (((x - r->last) * r->need) >> 13));
                r->need += r->step;
            }
            r->need -= one;
            r->last = x;
        }
    }
    return n;
}
