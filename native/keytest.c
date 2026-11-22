// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * keytest: press a key or move the handset, see its name on the phone's screen.
 * Reads the keypad (/dev/input/event0) and hook switch (/dev/input/event1) our
 * kernel provides, and writes the name to the screen console (/dev/tty1) and
 * stdout. Ctrl-C to stop.
 */
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>

static const char *name(int code)
{
	switch (code) {
	case KEY_1: return "1";
	case KEY_2: return "2";
	case KEY_3: return "3";
	case KEY_4: return "4";
	case KEY_5: return "5";
	case KEY_6: return "6";
	case KEY_7: return "7";
	case KEY_8: return "8";
	case KEY_9: return "9";
	case KEY_0: return "0";
	case KEY_KPASTERISK: return "*";
	case KEY_NUMERIC_POUND: return "#";
	case KEY_UP: return "UP";
	case KEY_DOWN: return "DOWN";
	case KEY_LEFT: return "LEFT";
	case KEY_RIGHT: return "RIGHT";
	case KEY_ENTER: return "OK";
	case KEY_BACKSPACE: return "X (cancel)";
	case KEY_VOLUMEDOWN: return "VOLUME -";
	case KEY_VOLUMEUP: return "VOLUME +";
	case KEY_PROG1: return "HEADSET";
	case KEY_MUTE: return "MUTE";
	case KEY_MESSENGER: return "MESSAGE";
	case KEY_PAUSE: return "HOLD";
	case KEY_AGAIN: return "REDIAL";
	case KEY_FORWARD: return "TRANSFER";
	case KEY_PROG2: return "SPEAKERPHONE";
	default: return NULL;
	}
}

int main(void)
{
	struct pollfd p[2];
	int tty = open("/dev/tty1", O_WRONLY);
	char buf[256];

	p[0].fd = open("/dev/input/event0", O_RDONLY);
	p[1].fd = open("/dev/input/event1", O_RDONLY);
	p[0].events = p[1].events = POLLIN;
	if (p[0].fd < 0) {
		perror("/dev/input/event0");
		return 1;
	}
	/* take the keys for ourselves, so the text console does not also echo them */
	ioctl(p[0].fd, EVIOCGRAB, 1);
	if (p[1].fd >= 0)
		ioctl(p[1].fd, EVIOCGRAB, 1);
	if (tty >= 0) {
		const char *s = "\033[2J\033[H\n\n   KEY TEST: press any key, or lift the handset\n";
		write(tty, s, strlen(s));
	}
	printf("press keys (Ctrl-C to stop)\n");
	fflush(stdout);

	for (;;) {
		int i;

		if (poll(p, p[1].fd >= 0 ? 2 : 1, -1) < 0)
			return 1;
		for (i = 0; i < 2; i++) {
			struct input_event ev;
			const char *n;
			int len;

			if (!(p[i].revents & POLLIN))
				continue;
			if (read(p[i].fd, &ev, sizeof(ev)) != sizeof(ev) || ev.type != EV_KEY || ev.value == 2)
				continue;
			if (ev.code == KEY_PHONE)
				n = ev.value ? "HANDSET LIFTED" : "HANDSET DOWN";
			else if (!ev.value)
				continue;	/* key release */
			else
				n = name(ev.code);
			if (n)
				len = snprintf(buf, sizeof(buf), "%s", n);
			else
				len = snprintf(buf, sizeof(buf), "unknown key, code %d", ev.code);
			printf("%s\n", buf);
			fflush(stdout);
			if (tty >= 0) {
				char out[400];
				int m = snprintf(out, sizeof(out),
					"\033[2J\033[H\n\n   KEY TEST\n\n\n\n            %s\n", buf);
				write(tty, out, m);
			}
			(void)len;
		}
	}
}
