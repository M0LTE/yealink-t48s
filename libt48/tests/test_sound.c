// SPDX-License-Identifier: AGPL-3.0-or-later
/* libt48 test: the resampler, and the sound queue when nobody is taking from
 * it (which is how the simulator runs): it must still keep time. Makes no
 * sound. */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "t48.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* amplitude of hz in x (16 kHz) */
static double amp(const int16_t *x, int n, double hz)
{
    double re = 0, im = 0, w = 2 * M_PI * hz / 16000;
    for (int i = 0; i < n; i++) { re += x[i] * cos(w * i); im += x[i] * sin(w * i); }
    return 2 * sqrt(re * re + im * im) / n;
}

static int16_t in[2 * 96000], out[20000];

/* one second of hz at `rate` with `ch` channels, fed in odd-sized pieces; returns samples out */
static int run(int rate, int ch, double hz, int level)
{
    t48_resampler r;
    int n = 0, at = 0, piece = 0;
    for (int i = 0; i < rate; i++) {
        int v = (int)lrint(level * sin(2 * M_PI * hz * i / rate));
        if (ch == 1) in[i] = (int16_t)v;
        else { in[2 * i] = (int16_t)v; in[2 * i + 1] = (int16_t)v; }
    }
    t48_resample_init(&r, rate, ch);
    while (at < rate) {
        int k = 1 + (piece++ * 37) % 500;
        if (k > rate - at) k = rate - at;
        int cap = t48_resample_max(&r, k);
        int got = t48_resample(&r, in + at * ch, k, out + n, cap);
        CHECK(got <= cap, "%d Hz: %d frames gave %d, more than the %d promised", rate, k, got, cap);
        n += got;
        at += k;
    }
    return n;
}

int main(void)
{
    static const int rates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000};

    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++)
        for (int ch = 1; ch <= 2; ch++) {
            int n = run(rates[i], ch, 440, 20000);
            double a = amp(out + 100, n - 200, 440);
            CHECK(abs(n - 16000) <= 2, "%d Hz, %d channels: one second gave %d samples", rates[i], ch, n);
            CHECK(a > 19000 && a < 20100, "%d Hz, %d channels: 440 Hz at %.0f, want 20000", rates[i], ch, a);
        }
    /* loud and at full scale, nothing wraps */
    int n = run(44100, 2, 1000, 32767), peak = 0;
    for (int i = 0; i < n; i++) if (abs(out[i]) > peak) peak = abs(out[i]);
    CHECK(peak > 31000 && peak <= 32767, "full scale 1 kHz from 44100: peak %d", peak);
    /* down: a tone that cannot be kept (12 kHz from 48000 would fold to 4 kHz) is mostly gone */
    n = run(48000, 1, 12000, 20000);
    CHECK(amp(out + 100, n - 200, 4000) < 20000 * 0.18, "12 kHz from 48000 folds to 4 kHz at %.0f", amp(out + 100, n - 200, 4000));
    /* stereo is the mean of the two: opposite channels cancel */
    for (int i = 0; i < 48000; i++) { in[2 * i] = 10000; in[2 * i + 1] = -10000; }
    t48_resampler r;
    t48_resample_init(&r, 48000, 2);
    n = t48_resample(&r, in, 48000, out, 20000);
    peak = 0;
    for (int i = 0; i < n; i++) if (abs(out[i]) > peak) peak = abs(out[i]);
    CHECK(n == 16000 && peak <= 1, "opposite channels: %d samples, peak %d", n, peak);

    /* The queue with nobody taking: a ring as the shell makes it, state OFF.
     * One second of sound takes one second to write, and the ring is not touched. */
    CHECK(t48_sound_open() < 0 && t48_sound_write(out, 10) < 0 && t48_sound_put(out, 10) == 0, "no ring: no sound, and nothing hangs");
    int fd = memfd_create("t48-sound", 0);
    struct t48_sound_ring h = {T48_SOUND_MAGIC, T48_SOUND_RATE, 4096, 4096, 0, T48_SOUND_OFF, 0, 0, 0, 0};
    if (fd < 0 || ftruncate(fd, 4096 + 8192) < 0 || pwrite(fd, &h, sizeof h, 0) < 0) return 77;
    char num[16];
    snprintf(num, sizeof num, "%d", fd);
    setenv(T48_ENV_SOUND, num, 1);
    CHECK(t48_sound_open() == 0, "the ring opens");
    struct t48_sound_ring *ring = mmap(NULL, 4096 + 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    long long t0 = t48_now_us();
    for (int i = 0; i < 100; i++) CHECK(t48_sound_write(out, 160) == 160, "write");
    long long dt = t48_now_us() - t0;
    CHECK(dt > 900000 && dt < 1100000, "nobody taking: 1 s of sound took %lld ms to write", dt / 1000);
    CHECK(ring->wr == 0, "nobody taking: the ring is left alone");
    CHECK(t48_sound_queued() <= 1024 && t48_sound_free() < 1024 && !t48_sound_playing(), "the queue is the size asked for");
    /* without waiting: only what there is room for */
    usleep(100000);
    t48_sound_buffer(640);
    int took = t48_sound_put(out, 5000);
    CHECK(took == 640 && t48_sound_put(out, 5000) < 20, "put takes what fits: %d", took);
    CHECK(abs(t48_sound_latency_us() - 40000) < 3000, "latency is what waits: %d us", t48_sound_latency_us());

    /* somebody taking (this test, as the service): the samples go in the ring in order */
    usleep(60000);
    ring->state = T48_SOUND_PLAYING;
    ring->delay = 400;
    for (int i = 0; i < 6000; i++) in[i] = (int16_t)i;
    int done = 0, bad = 0;
    uint32_t rd = 0;
    while (done < 6000 || rd != ring->wr) {
        if (done < 6000) done += t48_sound_put(in + done, 6000 - done);
        for (int k = 0; k < 320 && rd != ring->wr; k++, rd++)
            bad += ((int16_t *)((char *)ring + 4096))[rd & 4095] != (int16_t)rd;
        ring->rd = rd;
    }
    CHECK(done == 6000 && !bad && rd == 6000, "6000 samples through the ring in order (%d wrong)", bad);
    CHECK(t48_sound_playing() && t48_sound_latency_us() == (160 + 400) * 1000 / 16, "playing: latency is the service's delay and half a tick: %d us", t48_sound_latency_us());
    /* the service stops taking and does not say: a blocking write gives up waiting and keeps time by itself */
    t0 = t48_now_us();
    for (int i = 0; i < 100; i++) t48_sound_write(out, 160);
    dt = t48_now_us() - t0;
    CHECK(dt > 800000 && dt < 1500000, "the service gone quiet: 1 s of sound took %lld ms to write", dt / 1000);

    printf("%s test_sound: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
