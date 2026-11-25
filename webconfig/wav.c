// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-web: a WAV file made into a custom ringtone (issue #77, docs/web.md).
 * The phone plays 16 kHz mono 16-bit samples (the audio service's own
 * format, docs/audiod.md), so the file is turned into that here, once, when
 * it is given: channels averaged, resampled, cut to ten seconds, its peak
 * brought to the level the built-in ringtones have (so the ringer volume
 * means the same for all), and faded in and out over 20 ms so it never
 * starts or stops with a click (issue #57). The loudspeaker's limiter in the
 * audio service still has the last word.
 *
 * Integer arithmetic only, apart from a small table made once: the phone has
 * no floating-point unit.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

#define OUT_RATE 16000
#define PEAK 16000                 /* the built-in ringtones' level */
#define FADE (OUT_RATE / 50)       /* 20 ms */
#define ZEROS 8                    /* the resampling filter: zero crossings each side */
#define PHASES 64                  /* steps between input samples */

static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t u32(const unsigned char *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* one input sample, all channels averaged, as a 24-bit signed value */
static int32_t frame_at(const unsigned char *d, unsigned ch, unsigned bytes, int is_float)
{
    int64_t sum = 0;
    for (unsigned c = 0; c < ch; c++, d += bytes) {
        int32_t v;
        if (is_float) {
            uint32_t bits = u32(d);
            float fv;
            memcpy(&fv, &bits, 4);
            if (!(fv == fv)) fv = 0;   /* NaN */
            v = fv >= 1 ? 8388607 : fv <= -1 ? -8388608 : (int32_t)(fv * 8388607.0f);
        } else if (bytes == 1) {
            v = ((int32_t)d[0] - 128) << 16;
        } else if (bytes == 2) {
            v = (int16_t)u16(d) * 256;
        } else if (bytes == 3) {
            v = (int32_t)((uint32_t)d[0] << 8 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 24) >> 8;
        } else {
            v = (int32_t)u32(d) >> 8;
        }
        sum += v;
    }
    return (int32_t)(sum / ch);
}

/* A WAV file's samples as the phone's: *out is malloc'd, *n samples. 0, or
 * -1 with a short reason in why. */
int wav_to_ringtone(const unsigned char *in, size_t len, int16_t **out, size_t *n, int *cut, const char **why)
{
    const unsigned char *fmt = NULL, *data = NULL;
    size_t dlen = 0, fmt_size = 0;
    *out = NULL;
    *n = 0;
    *cut = 0;
    if (len < 12 || memcmp(in, "RIFF", 4) || memcmp(in + 8, "WAVE", 4)) { *why = "not a WAV file"; return -1; }
    for (size_t at = 12; at + 8 <= len;) {
        uint32_t size = u32(in + at + 4);
        const unsigned char *body = in + at + 8;
        size_t room = len - at - 8;
        if (!memcmp(in + at, "fmt ", 4) && size >= 16 && size <= room) { fmt = body; fmt_size = size; }
        if (!memcmp(in + at, "data", 4)) { data = body; dlen = size < room ? size : room; break; }
        if (size > room) break;
        at += 8 + size + (size & 1);
    }
    if (!fmt || !data) { *why = "not a WAV file"; return -1; }
    unsigned tag = u16(fmt), ch = u16(fmt + 2), bits = u16(fmt + 14);
    uint32_t rate = u32(fmt + 4);
    /* WAVE_FORMAT_EXTENSIBLE: its subformat, when the chunk is long enough to hold it */
    if (tag == 0xfffe) tag = fmt_size >= 40 && u16(fmt + 16) >= 22 ? u16(fmt + 24) : 0;
    int is_float = tag == 3 && bits == 32;
    if (!(tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !is_float) { *why = "WAV must be PCM or 32-bit float"; return -1; }
    if (ch < 1 || ch > 8 || rate < 8000 || rate > 192000) { *why = "WAV must be 8 to 192 kHz, 1 to 8 channels"; return -1; }
    unsigned bytes = bits / 8, step = bytes * ch;
    size_t frames = dlen / step;
    if (!frames) { *why = "the sound is empty"; return -1; }

    /* how many output samples, ten seconds at most */
    uint64_t want = (uint64_t)frames * OUT_RATE / rate;
    if (want > T48AC_CUSTOM_MAX) { want = T48AC_CUSTOM_MAX; *cut = 1; }
    if (!want) { *why = "the sound is too short"; return -1; }
    int32_t *acc = malloc(want * sizeof *acc);
    if (!acc) { *why = "out of memory"; return -1; }

    if (rate == OUT_RATE) {
        for (size_t i = 0; i < want; i++) acc[i] = frame_at(data + i * step, ch, bytes, is_float);
    } else {
        /* A windowed sinc, its cut-off just under the lower of the two
         * Nyquist rates, as a table of Q14 taps at PHASES steps. For each
         * output sample the taps around its place in the input. */
        static int16_t taps[2 * ZEROS * PHASES + 1];
        uint32_t lower = rate < OUT_RATE ? rate : OUT_RATE;
        double fc = 0.45 * lower / rate;               /* of the input rate */
        int width = (int)ceil(ZEROS / (2 * fc));       /* input samples each side */
        if (width > 64) width = 64;
        /* table steps per input sample, in 1/65536 */
        int64_t kscale = llrint(2 * fc * PHASES * 65536);
        for (int k = 0; k <= 2 * ZEROS * PHASES; k++) {
            double x = (double)(k - ZEROS * PHASES) / PHASES;   /* in zero crossings */
            double sinc = x == 0 ? 1 : sin(M_PI * x) / (M_PI * x);
            double w = 0.5 + 0.5 * cos(M_PI * x / ZEROS);
            taps[k] = (int16_t)lrint(16384 * sinc * w);
        }
        for (size_t i = 0; i < want; i++) {
            /* where this sample falls in the input, in 1/65536 of a sample */
            uint64_t pos = ((uint64_t)i * rate << 16) / OUT_RATE;
            int64_t centre = (int64_t)(pos >> 16), sum = 0, norm = 0;
            uint32_t frac = (uint32_t)(pos & 0xffff);
            for (int j = -width; j <= width; j++) {
                int64_t at = centre + j;
                /* distance in table steps: (j - frac) * 2 fc * PHASES, rounded */
                int64_t dist = ((int64_t)j * 65536 - frac) * kscale;
                int k = (int)((dist + ((int64_t)1 << 31)) >> 32) + ZEROS * PHASES;
                if (k < 0 || k > 2 * ZEROS * PHASES) continue;
                norm += taps[k];
                if (at < 0 || at >= (int64_t)frames) continue;
                sum += (int64_t)taps[k] * frame_at(data + (size_t)at * step, ch, bytes, is_float);
            }
            acc[i] = norm ? (int32_t)(sum / norm) : 0;
        }
    }

    int32_t peak = 0;
    for (size_t i = 0; i < want; i++) {
        int32_t a = acc[i] < 0 ? -acc[i] : acc[i];
        if (a > peak) peak = a;
    }
    if (peak < 256) { free(acc); *why = "the sound is silent"; return -1; }
    int16_t *o = malloc(want * sizeof *o);
    if (!o) { free(acc); *why = "out of memory"; return -1; }
    for (size_t i = 0; i < want; i++) {
        int64_t v = (int64_t)acc[i] * PEAK / peak;
        size_t edge = i < want - 1 - i ? i : want - 1 - i;
        if (edge < FADE) v = v * (int64_t)edge / FADE;
        o[i] = (int16_t)v;
    }
    free(acc);
    *out = o;
    *n = want;
    return 0;
}
