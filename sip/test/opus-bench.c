// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Opus CPU on the phone, silently (docs/sip.md): encodes a mono s16 WAV in
// 20 ms frames, then decodes the packets, and prints the CPU each took as a
// share of real time. Nothing is played.
//
//   opus-bench FILE.wav BITRATE COMPLEXITY [FEC-LOSS-PERCENT [DTX [OUT.wav]]]
//
// FEC-LOSS-PERCENT above 0 switches on in-band FEC with that expected loss
// and drops every Nth packet in the decoder, recovering it from the next
// packet's FEC as baresip does; DTX 1 switches on discontinuous
// transmission. OUT.wav gets the decoded audio, for listening.
//
// Build: $T/bin/armcc -O2 -I$OPUS/include/opus opus-bench.c $OPUS/lib/libopus.a -o opus-bench
#include <opus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void put32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: opus-bench FILE.wav BITRATE COMPLEXITY [FEC-LOSS-PERCENT [DTX [OUT.wav]]]\n");
        return 2;
    }
    int bitrate = atoi(argv[2]), complexity = atoi(argv[3]);
    int loss = argc > 4 ? atoi(argv[4]) : 0, dtx = argc > 5 ? atoi(argv[5]) : 0;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    unsigned char hdr[44];
    if (fread(hdr, 1, 44, f) != 44 || memcmp(hdr, "RIFF", 4) || hdr[22] != 1 || hdr[34] != 16) {
        fprintf(stderr, "%s: not a mono 16-bit WAV with a 44-byte header\n", argv[1]);
        return 1;
    }
    int rate = hdr[24] | hdr[25] << 8 | hdr[26] << 16 | hdr[27] << 24;
    fseek(f, 0, SEEK_END);
    long n = (ftell(f) - 44) / 2;
    fseek(f, 44, SEEK_SET);
    int16_t *pcm = malloc(n * 2), *out = calloc(n, 2);
    if (fread(pcm, 2, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    int fs = rate / 50, frames = n / fs, err;
    OpusEncoder *enc = opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &err);
    OpusDecoder *dec = opus_decoder_create(rate, 1, &err);
    if (!enc || !dec) { fprintf(stderr, "opus: %s\n", opus_strerror(err)); return 1; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(loss > 0));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(loss));
    opus_encoder_ctl(enc, OPUS_SET_DTX(dtx));

    unsigned char (*pkt)[1500] = malloc((size_t)frames * 1500);
    int *len = malloc(frames * sizeof *len);
    long bytes = 0;
    int wide = 0;
    double worst = 0, t0 = now();
    for (int i = 0; i < frames; i++) {
        double a = now();
        len[i] = opus_encode(enc, pcm + (long)i * fs, fs, pkt[i], 1500);
        double d = now() - a;
        if (d > worst) worst = d;
        if (len[i] < 0) { fprintf(stderr, "encode: %s\n", opus_strerror(len[i])); return 1; }
        bytes += len[i];
        if (len[i] > 2 && opus_packet_get_bandwidth(pkt[i]) >= OPUS_BANDWIDTH_WIDEBAND)
            wide++;
    }
    double tenc = now() - t0, wenc = worst;

    // loss: drop every (100/loss)th packet, then rebuild it from the next one's FEC
    int every = loss > 0 ? 100 / loss : 0, lost = 0;
    worst = 0;
    t0 = now();
    for (int i = 0; i < frames; i++) {
        double a = now();
        int r;
        if (every && i % every == every - 1 && i + 1 < frames) {
            r = opus_decode(dec, pkt[i + 1], len[i + 1], out + (long)i * fs, fs, 1);
            lost++;
        } else {
            r = opus_decode(dec, len[i] > 2 ? pkt[i] : NULL, len[i] > 2 ? len[i] : 0, out + (long)i * fs, fs, 0);
        }
        double d = now() - a;
        if (d > worst) worst = d;
        if (r < 0) { fprintf(stderr, "decode: %s\n", opus_strerror(r)); return 1; }
    }
    double tdec = now() - t0, wdec = worst;
    double secs = (double)frames * fs / rate;

    printf("rate %d bitrate %d complexity %d fec-loss %d dtx %d: %.1f s audio, %.1f kbit/s sent, %d%% wideband or wider, "
           "encode %.2f%% CPU (worst frame %.2f ms), decode %.2f%% CPU (worst frame %.2f ms), %d recovered by FEC\n",
           rate, bitrate, complexity, loss, dtx, secs, bytes * 8 / secs / 1000, 100 * wide / frames,
           100 * tenc / secs, wenc * 1000, 100 * tdec / secs, wdec * 1000, lost);

    if (argc > 6) {
        FILE *o = fopen(argv[6], "wb");
        if (!o) { perror(argv[6]); return 1; }
        put32(hdr + 40, frames * fs * 2);
        put32(hdr + 4, 36 + frames * fs * 2);
        fwrite(hdr, 1, 44, o);
        fwrite(out, 2, (size_t)frames * fs, o);
        fclose(o);
    }
    return 0;
}
