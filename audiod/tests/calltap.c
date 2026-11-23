// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * calltap: stand in for baresip on t48-audiod's call stream, on the phone.
 * It records what the service sends as the microphone into a WAV file and
 * answers every frame with silence or a test tone as the far end's voice.
 * For measuring the real audio path (docs/audiod.md).
 *
 *   calltap [-s SECONDS] [-o MIC.wav] [-f HZ -l DBFS] [-a START,STOP] [SOCKET]
 *     -s  how long to stay connected (default 5)
 *     -o  where to write the microphone frames
 *     -f  far-end tone frequency (default: silence)
 *     -l  its level in dB below full scale, rms, as a negative number (default -30)
 *     -a  send the tone only between these seconds (default: all the time)
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "t48audio_proto.h"

static int rd(int fd, void *p, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)p + got, n - got);
		if (r <= 0)
			return -1;
		got += (size_t)r;
	}
	return 0;
}

static int msg(int fd, unsigned type, const void *p, unsigned len)
{
	unsigned char b[4 + T48A_FRAME_LEN];
	b[0] = type & 255; b[1] = type >> 8; b[2] = len & 255; b[3] = len >> 8;
	memcpy(b + 4, p, len);
	return write(fd, b, 4 + len) == (ssize_t)(4 + len) ? 0 : -1;
}

int main(int argc, char **argv)
{
	double secs = 5, hz = 0, db = -30, t_on = 0, t_off = 1e9, amp;
	const char *out = 0, *path = T48A_SOCKET;
	struct sockaddr_un sa;
	uint16_t hello[2] = { T48A_VERSION, T48A_FRAME_MS };
	unsigned frames, n = 0, lost = 0, expect = 0, i;
	int16_t *pcm;
	int o, fd;

	while ((o = getopt(argc, argv, "s:o:f:l:a:")) != -1) {
		if (o == 's') secs = atof(optarg);
		else if (o == 'o') out = optarg;
		else if (o == 'f') hz = atof(optarg);
		else if (o == 'l') db = atof(optarg);
		else if (o == 'a' && sscanf(optarg, "%lf,%lf", &t_on, &t_off) == 2) ;
		else { fprintf(stderr, "usage: calltap [-s SEC] [-o MIC.wav] [-f HZ -l DBFS] [-a START,STOP] [SOCKET]\n"); return 2; }
	}
	if (optind < argc)
		path = argv[optind];
	if (db > -6) { fprintf(stderr, "calltap: level %.0f dBFS refused: -6 at most\n", db); return 2; }
	amp = 32767 * pow(10, db / 20) * sqrt(2);
	frames = (unsigned)(secs * 1000 / T48A_FRAME_MS);
	pcm = calloc(frames, T48A_FRAME * 2);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror(path); return 1; }
	msg(fd, T48A_HELLO, hello, sizeof(hello));
	while (n < frames) {
		unsigned char h[4], body[2048];
		unsigned type, len;
		struct { uint32_t seq; int16_t s[T48A_FRAME]; } spk;

		if (rd(fd, h, 4) < 0)
			break;
		type = h[0] | h[1] << 8;
		len = h[2] | h[3] << 8;
		if (len > sizeof(body) || rd(fd, body, len) < 0)
			break;
		if (type != T48A_MIC || len != T48A_FRAME_LEN)
			continue;
		memcpy(&spk.seq, body, 4);
		if (spk.seq != expect)
			lost += spk.seq - expect;
		expect = spk.seq + 1;
		memcpy(pcm + n * T48A_FRAME, body + 4, T48A_FRAME * 2);
		for (i = 0; i < T48A_FRAME; i++) {
			double t = (double)(n * T48A_FRAME + i) / T48A_SRATE;
			spk.s[i] = hz > 0 && t >= t_on && t < t_off ? (int16_t)lrint(amp * sin(2 * M_PI * hz * t)) : 0;
		}
		if (msg(fd, T48A_SPK, &spk, sizeof(spk)) < 0)
			break;
		n++;
	}
	msg(fd, T48A_BYE, 0, 0);
	close(fd);
	printf("calltap: %u frames, %u missing\n", n, lost);
	if (out && n) {
		uint32_t bytes = n * T48A_FRAME * 2;
		uint32_t hd[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16,
				    0x00010001, T48A_SRATE, T48A_SRATE * 2, 0x00100002, 0x61746164, bytes };
		FILE *f = fopen(out, "wb");
		if (!f) { perror(out); return 1; }
		fwrite(hd, 4, 11, f);
		fwrite(pcm, 2, n * T48A_FRAME, f);
		fclose(f);
	}
	return n ? 0 : 1;
}
