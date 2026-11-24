// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-keys: the keypad for shell scripts. The first stage's rescue menu uses it
 * (issue 32, docs/install.md); nothing else needs to.
 *
 *   t48-keys held NAME        exit 0 if that key is down now, 1 if not, 2 if
 *                             there is no keypad
 *   t48-keys read [SECONDS]   wait for a key to be pressed and print its name;
 *                             exit 1 if none was in that time (default: for ever),
 *                             2 if there is no keypad
 *
 * Names: 0 to 9, star, hash, up, down, left, right, ok, x; any other key is
 * "other". The keypad is the input device named "T48S keypad", or the one
 * T48_KEYS names.
 */
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* the kernel's 32-bit layout: musl's struct timeval has a 64-bit time_t */
struct ev32 { unsigned sec, usec; unsigned short type, code; int value; };

static const struct { int code; const char *name; } keys[] = {
	{KEY_0, "0"}, {KEY_1, "1"}, {KEY_2, "2"}, {KEY_3, "3"}, {KEY_4, "4"},
	{KEY_5, "5"}, {KEY_6, "6"}, {KEY_7, "7"}, {KEY_8, "8"}, {KEY_9, "9"},
	{KEY_KPASTERISK, "star"}, {KEY_NUMERIC_POUND, "hash"},
	{KEY_UP, "up"}, {KEY_DOWN, "down"}, {KEY_LEFT, "left"}, {KEY_RIGHT, "right"},
	{KEY_ENTER, "ok"}, {KEY_BACKSPACE, "x"},
};
#define NKEYS (int)(sizeof keys / sizeof keys[0])

static int keypad(void)
{
	const char *path = getenv("T48_KEYS");
	char p[40], name[80];
	int fd, i;

	if (path)
		return open(path, O_RDONLY);
	for (i = 0; i < 8; i++) {
		snprintf(p, sizeof p, "/dev/input/event%d", i);
		fd = open(p, O_RDONLY);
		if (fd < 0)
			continue;
		memset(name, 0, sizeof name);
		if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) >= 0 && !strcmp(name, "T48S keypad"))
			return fd;
		close(fd);
	}
	return -1;
}

static long long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	int fd, i;

	if (argc < 2 || (strcmp(argv[1], "held") && strcmp(argv[1], "read")) ||
	    (!strcmp(argv[1], "held") && argc != 3)) {
		fprintf(stderr, "usage: t48-keys held NAME | read [SECONDS]\n");
		return 2;
	}
	fd = keypad();
	if (fd < 0)
		return 2;
	if (!strcmp(argv[1], "held")) {
		unsigned char bits[KEY_MAX / 8 + 1];
		memset(bits, 0, sizeof bits);
		if (ioctl(fd, EVIOCGKEY(sizeof bits), bits) < 0)
			return 2;
		for (i = 0; i < NKEYS; i++)
			if (!strcmp(keys[i].name, argv[2]))
				return (bits[keys[i].code / 8] >> (keys[i].code % 8)) & 1 ? 0 : 1;
		return 2;
	} else {
		long long end = argc > 2 ? now_ms() + atol(argv[2]) * 1000LL : -1;
		struct pollfd p = { .fd = fd, .events = POLLIN };
		struct ev32 e;

		for (;;) {
			int left = -1;
			if (end >= 0) {
				long long l = end - now_ms();
				if (l <= 0)
					return 1;
				left = (int)l;
			}
			if (poll(&p, 1, left) < 0)
				return 2;
			if (!(p.revents & POLLIN)) {
				if (p.revents)
					return 2;
				continue;
			}
			if (read(fd, &e, sizeof e) != sizeof e)
				return 2;
			if (e.type != EV_KEY || e.value != 1)
				continue;
			for (i = 0; i < NKEYS; i++)
				if (keys[i].code == e.code) {
					puts(keys[i].name);
					return 0;
				}
			puts("other");
			return 0;
		}
	}
}
