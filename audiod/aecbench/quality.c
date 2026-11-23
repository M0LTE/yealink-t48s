// SPDX-License-Identifier: AGPL-3.0-or-later
/* Echo canceller comparison (issue #35): how well each candidate works.
 *   aecq SPEECHDIR [-w OUTDIR] [CANDIDATE...]
 *
 * A made-up speakerphone, built from what was measured on the phone
 * (docs/audio.md, docs/aec.md):
 *  - the loudspeaker frame reaches the microphone 45 ms later;
 *  - loudspeaker to microphone is loudest at 1.4 kHz (+7 dB against the frame
 *    played), about +5 dB at 1 kHz and -9 dB at 400 Hz;
 *  - after the direct sound comes a room: reflections dying away over 0.35 s
 *    (longer than any tail tried, as a real room is);
 *  - optionally the small loudspeaker distorts when it is loud;
 *  - the microphone has noise at -49 dBFS and the owner, at arm's length, is
 *    at -34 dBFS.
 *
 * The call, 40 s (far end: one CMU Arctic speaker, owner: the other):
 *    0 to 12   only the far end talks
 *   12 to 20   both talk
 *   20 to 26   only the far end
 *   26 to 32   only the owner
 *   32 to 40   only the far end, and at 32 the room changes (something is
 *              put down next to the phone)
 *
 * Figures, all in dB:
 *   echo reduction   microphone level with no echo control over what is sent
 *                    to the call, far end talking alone (ERLE). The room noise
 *                    limits it unless the candidate also removes noise.
 *   owner            how much of the owner's voice gets through (0 = all)
 *   residue          during both-talking: everything sent that is not the
 *                    owner's voice (left-over echo, distortion, noise),
 *                    against the echo at the microphone
 *   loudspeaker      how far the loudspeaker was turned down while both talk
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cand.h"

#define SEC      SRATE
#define TOTAL    (40 * SEC)
#define DELAY    (45 * SRATE / 1000)
#define HLEN     6400		/* the room: 0.4 s */

static float h[2][HLEN];
static int16_t far[TOTAL], near[TOTAL], noise[TOTAL];
static float played[TOTAL];	/* after the loudspeaker's distortion */

struct run {
	int16_t spk[TOTAL], mic[TOTAL], out[TOTAL];
};

static uint32_t seed;
static double frand(void)
{
	seed = seed * 1664525u + 1013904223u;
	return (seed >> 8) / 8388608.0 - 1.0;
}
static double gauss(void)
{
	return (frand() + frand() + frand() + frand()) * 0.866;
}

/* --- speech ------------------------------------------------------------- */

static int load_wav(const char *path, int16_t *dst, int max)
{
	FILE *f = fopen(path, "rb");
	unsigned char hd[12], ck[8];
	int n = 0;

	if (!f)
		return -1;
	if (fread(hd, 1, 12, f) != 12 || memcmp(hd, "RIFF", 4)) {
		fclose(f);
		return -1;
	}
	while (fread(ck, 1, 8, f) == 8) {
		unsigned len = ck[4] | ck[5] << 8 | ck[6] << 16 | (unsigned)ck[7] << 24;
		if (!memcmp(ck, "data", 4)) {
			n = (int)(len / 2) < max ? (int)(len / 2) : max;
			n = (int)fread(dst, 2, n, f);
			break;
		}
		fseek(f, len + (len & 1), SEEK_CUR);
	}
	fclose(f);
	return n;
}

/* Fill dst with the speaker's sentences end to end, then bring the talking
 * parts (20 ms frames above -55 dBFS) to the level asked for */
static int load_speaker(const char *dir, const char *who, int16_t *dst, double dbfs)
{
	char path[512];
	int n = 0, i, k, f;
	double s = 0, g;
	long cnt = 0;

	for (k = 1; n < TOTAL; k = k % 12 + 1) {
		snprintf(path, sizeof(path), "%s/%s_%04d.wav", dir, who, k);
		i = load_wav(path, dst + n, TOTAL - n);
		if (i <= 0) {
			fprintf(stderr, "%s: cannot read (run ./fetch.sh)\n", path);
			return -1;
		}
		n += i;
	}
	for (f = 0; f + FRAME <= TOTAL; f += FRAME) {
		double e = 0;
		for (i = 0; i < FRAME; i++)
			e += (double)dst[f + i] * dst[f + i];
		if (e / FRAME > 58.0 * 58.0) {
			s += e;
			cnt += FRAME;
		}
	}
	g = 32768.0 * pow(10, dbfs / 20) / sqrt(s / cnt);
	for (i = 0; i < TOTAL; i++)
		dst[i] = sat16((int32_t)lrint(dst[i] * g));
	return 0;
}

/* --- the room ----------------------------------------------------------- */

struct biquad { double b0, b1, b2, a1, a2; };

static struct biquad bq_peak(double hz, double q, double db)
{
	double A = pow(10, db / 40), w = 2 * M_PI * hz / SRATE, al = sin(w) / (2 * q);
	double a0 = 1 + al / A;
	struct biquad b = { (1 + al * A) / a0, -2 * cos(w) / a0, (1 - al * A) / a0,
			    -2 * cos(w) / a0, (1 - al / A) / a0 };
	return b;
}
static struct biquad bq_pass(double hz, double q, int high)
{
	double w = 2 * M_PI * hz / SRATE, al = sin(w) / (2 * q), c = cos(w), a0 = 1 + al;
	struct biquad b;
	if (high) {
		b.b0 = (1 + c) / 2 / a0; b.b1 = -(1 + c) / a0; b.b2 = b.b0;
	} else {
		b.b0 = (1 - c) / 2 / a0; b.b1 = (1 - c) / a0; b.b2 = b.b0;
	}
	b.a1 = -2 * c / a0;
	b.a2 = (1 - al) / a0;
	return b;
}
static void bq_run(struct biquad b, float *x, int n)
{
	double x1 = 0, x2 = 0, y1 = 0, y2 = 0, y;
	int i;
	for (i = 0; i < n; i++) {
		y = b.b0 * x[i] + b.b1 * x1 + b.b2 * x2 - b.a1 * y1 - b.a2 * y2;
		x2 = x1; x1 = x[i]; y2 = y1; y1 = y;
		x[i] = (float)y;
	}
}

static double gain_at(const float *x, double hz)
{
	double re = 0, im = 0, w = 2 * M_PI * hz / SRATE;
	int i;
	for (i = 0; i < HLEN; i++) {
		re += x[i] * cos(w * i);
		im += x[i] * sin(w * i);
	}
	return 20 * log10(sqrt(re * re + im * im));
}

/* which: 0 the room at the start, 1 after something moved */
static void make_room(int which)
{
	float *x = h[which];
	double rt60 = which ? 0.30 : 0.35, e_dir = 0, e_rev = 0, g;
	int i;

	seed = which ? 99 : 7;
	memset(x, 0, sizeof(h[0]));
	/* direct sound, and the desk */
	x[0] = 1.0f;	/* fixed: loudspeaker and microphone are in one case */
	x[which ? 31 : 24] += which ? 0.6f : 0.4f;
	x[which ? 70 : 0] += which ? -0.35f : 0;
	for (i = 0; i < HLEN; i++)
		e_dir += x[i] * x[i];
	/* the room: noise dying away, 8 dB under the direct sound in total */
	for (i = 48; i < HLEN; i++) {
		double v = gauss() * exp(-6.9 * i / (rt60 * SRATE));
		x[i] += (float)(v * 0.01);
		e_rev += v * v * 1e-4;
	}
	g = sqrt(e_dir / e_rev) * pow(10, (which ? -6.0 : -8.0) / 20);
	for (i = 48; i < HLEN; i++) {
		double v = x[i];
		if (i != 70 || !which)
			x[i] = (float)(v * g);
	}
	/* the loudspeaker in its case */
	bq_run(bq_pass(300, 0.7, 1), x, HLEN);
	bq_run(bq_pass(350, 0.7, 1), x, HLEN);
	bq_run(bq_peak(1400, 1.6, 11), x, HLEN);
	bq_run(bq_pass(6000, 0.7, 0), x, HLEN);
}

static void scale_rooms(void)
{
	double g = pow(10, (7.0 - gain_at(h[0], 1400)) / 20);
	int i;
	for (i = 0; i < HLEN; i++) {
		h[0][i] *= (float)g;
		h[1][i] *= (float)g;
	}
}

/* --- one call ------------------------------------------------------------ */

static int distort;

/* The loudspeaker: clean when quiet, squashed and a little lopsided when
 * loud. 9000 counts is where it starts to matter. */
static float speaker(float x)
{
	float u;
	if (!distort)
		return x;
	u = x / 9000.0f;
	if (u > 1.4f) u = 1.4f;
	if (u < -1.4f) u = -1.4f;
	return 9000.0f * (u - 0.04f * u * u - 0.12f * u * u * u);
}

static void run_call(const struct cand *c, int tail, struct run *r)
{
	int16_t spk[FRAME], mic[FRAME];
	int t, i, k;

	c->init(tail);
	memset(played, 0, sizeof(played));
	for (t = 0; t + FRAME <= TOTAL; t += FRAME) {
		const float *hh = h[t >= 32 * SEC];
		int nearon = (t >= 12 * SEC && t < 20 * SEC) || (t >= 26 * SEC && t < 32 * SEC);

		memcpy(spk, far + t, sizeof(spk));
		for (i = 0; i < FRAME; i++) {
			int p = t + i - DELAY, kmax = p + 1 < HLEN ? p + 1 : HLEN;
			float acc = 0;
			for (k = 0; k < kmax; k++)
				acc += hh[k] * played[p - k];
			mic[i] = sat16((int32_t)lrintf(acc) + noise[t + i] + (nearon ? near[t + i] : 0));
		}
		memcpy(r->mic + t, mic, sizeof(mic));
		c->run(spk, mic);
		memcpy(r->spk + t, spk, sizeof(spk));
		memcpy(r->out + t, mic, sizeof(mic));
		for (i = 0; i < FRAME; i++)
			played[t + i] = speaker(spk[i]);
	}
}

static double power(const int16_t *x, int a, int b)
{
	double s = 0;
	int i;
	for (i = a; i < b; i++)
		s += (double)x[i] * x[i];
	return s / (b - a) + 1e-3;
}

/* How much of ref is in x over a..b, allowing x to be up to 40 ms late:
 * returns the gain and leaves in *res the power of what is left */
static double project(const int16_t *x, const int16_t *ref, int a, int b, double *res)
{
	double best = 0, bres = 0, rr = 0;
	int lag, i;

	for (i = a; i < b; i++)
		rr += (double)ref[i] * ref[i];
	for (lag = 0; lag <= 640; lag++) {
		double xr = 0, xx = 0, g;
		for (i = a; i < b; i++) {
			xr += (double)x[i + lag] * ref[i];
			xx += (double)x[i + lag] * x[i + lag];
		}
		g = xr / rr;
		if (g > best) {
			best = g;
			bres = (xx - g * g * rr) / (b - a);
		}
	}
	if (res)
		*res = bres + 1e-3;
	return best + 1e-6;
}

static double dB(double p) { return 10 * log10(p); }

static struct run *ref, *cur;

static void report(const char *name, int tail, int tails)
{
	double res, g, echo_dt;
	char label[32];

	if (tails)
		snprintf(label, sizeof(label), "%s %d", name, tail);
	else
		snprintf(label, sizeof(label), "%s", name);
	printf("%-15s", label);
	/* far end alone: first 2 s, settled, after both talked, after the room changed */
	printf(" %6.1f %6.1f %6.1f", dB(power(ref->out, 1 * SEC / 2, 5 * SEC / 2) / power(cur->out, 1 * SEC / 2, 5 * SEC / 2)),
	       dB(power(ref->out, 6 * SEC, 12 * SEC) / power(cur->out, 6 * SEC, 12 * SEC)),
	       dB(power(ref->out, 20 * SEC + SEC / 2, 26 * SEC) / power(cur->out, 20 * SEC + SEC / 2, 26 * SEC)));
	printf(" %6.1f %6.1f", dB(power(ref->out, 32 * SEC, 34 * SEC) / power(cur->out, 32 * SEC, 34 * SEC)),
	       dB(power(ref->out, 36 * SEC, 40 * SEC - SEC / 4) / power(cur->out, 36 * SEC, 40 * SEC - SEC / 4)));
	/* both talking: the echo there would have been is what the reference
	 * run's microphone had over and above the owner */
	g = project(ref->out, near, 13 * SEC, 19 * SEC, &echo_dt);
	g = project(cur->out, near, 13 * SEC, 19 * SEC, &res);
	printf("   %6.1f %6.1f %6.1f", 20 * log10(g), dB(echo_dt / res),
	       dB(power(cur->spk, 13 * SEC, 19 * SEC) / power(ref->spk, 13 * SEC, 19 * SEC)));
	/* owner alone */
	g = project(cur->out, near, 27 * SEC, 31 * SEC, &res);
	printf("   %6.1f\n", 20 * log10(g));
}

static void write_wav(const char *dir, const char *name, int tail, const char *cond, const struct run *r)
{
	char path[512];
	unsigned char hd[44] = "RIFF....WAVEfmt ";
	unsigned len = TOTAL * 4, v;
	FILE *f;
	int i;

	snprintf(path, sizeof(path), "%s/%s-%s-%d.wav", dir, cond, name, tail);
	f = fopen(path, "wb");
	if (!f)
		return;
#define LE32(o, x) (v = (x), hd[o] = v, hd[o + 1] = v >> 8, hd[o + 2] = v >> 16, hd[o + 3] = v >> 24)
	LE32(4, 36 + len); LE32(16, 16); LE32(20, 1 | 2 << 16); LE32(24, SRATE);
	LE32(28, SRATE * 4); LE32(32, 4 | 16 << 16);
	memcpy(hd + 36, "data", 4); LE32(40, len);
	fwrite(hd, 1, 44, f);
	/* left: what the far end hears; right: the loudspeaker */
	for (i = 0; i < TOTAL; i++) {
		fwrite(&r->out[i], 2, 1, f);
		fwrite(&r->spk[i], 2, 1, f);
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	static const struct { const char *id, *what; int distort; double far_db, noise_db; } conds[] = {
		{ "ideal", "clean loudspeaker, quiet room (noise -70 dBFS), far end at -26 dBFS", 0, -26, -70 },
		{ "loud", "as measured: noise -49 dBFS, loudspeaker distorting, far end loud (-26 dBFS played)", 1, -26, -49 },
		{ "mid", "as measured, middle volume (-38 dBFS played)", 1, -38, -49 },
	};
	static const int tails[] = { 64, 128, 200 };
	const char *dir, *wdir = 0;
	int first = 2, c, k, t, i;

	if (argc < 2) {
		fprintf(stderr, "usage: aecq SPEECHDIR [-w OUTDIR] [CANDIDATE...]\n");
		return 2;
	}
	dir = argv[1];
	if (argc > 3 && !strcmp(argv[2], "-w")) {
		wdir = argv[3];
		first = 4;
	}
	ref = malloc(sizeof(*ref));
	cur = malloc(sizeof(*cur));
	make_room(0);
	make_room(1);
	scale_rooms();
	printf("the made-up loudspeaker-to-microphone path, dB against the frame played:\n ");
	for (i = 0; i < 9; i++) {
		static const int hz[] = { 200, 400, 700, 1000, 1400, 2000, 3000, 4500, 6500 };
		printf(" %d Hz %+.1f%s", hz[i], gain_at(h[0], hz[i]), i == 8 ? "\n" : ",");
	}

	for (k = 0; k < 3; k++) {
		double na = 32768.0 * pow(10, conds[k].noise_db / 20), lp = 0;

		if (load_speaker(dir, "bdl", far, conds[k].far_db) || load_speaker(dir, "slt", near, -34))
			return 1;
		memset(far + 26 * SEC, 0, 6 * SEC * sizeof(int16_t));
		seed = 4242;
		for (i = 0; i < TOTAL; i++) {
			lp += (gauss() - lp) * 0.5;	/* a little darker than white */
			noise[i] = (int16_t)lrint(lp * na * 1.73);
		}
		distort = conds[k].distort;
		run_call(cand_find("none"), 0, ref);
		printf("\n%s: %s\n", conds[k].id, conds[k].what);
		printf("  at the microphone: echo %.1f dBFS, owner %.1f dBFS, noise %.1f dBFS\n",
		       dB(power(ref->mic, 6 * SEC, 12 * SEC)) - 90.3, dB(power(near, 27 * SEC, 31 * SEC)) - 90.3,
		       dB(power(noise, 0, TOTAL)) - 90.3);
		printf("%-15s %-34s   %-20s   %s\n", "", "echo reduction, far end alone", "both talking", "owner alone");
		printf("%-15s %6s %6s %6s %6s %6s   %6s %6s %6s   %6s\n", "candidate", "0-2 s", "later", "aft.DT",
		       "moved", "later", "owner", "resid.", "spkr", "owner");
		for (c = 0; c < ncands; c++) {
			if (argc > first) {
				for (i = first; i < argc && strcmp(argv[i], cands[c].name); i++)
					;
				if (i == argc)
					continue;
			}
			for (t = 0; t < (cands[c].tails ? 3 : 1); t++) {
				run_call(&cands[c], tails[t], cur);
				report(cands[c].name, tails[t], cands[c].tails);
				fflush(stdout);
				if (wdir)
					write_wav(wdir, cands[c].name, cands[c].tails ? tails[t] : 0, conds[k].id, cur);
			}
		}
	}
	return 0;
}
