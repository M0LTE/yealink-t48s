// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod test: message framing over a socket, whole, in pieces and bad. */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "t.h"
#include "../msg.h"

int main(void)
{
	static struct msgbuf mb;
	static unsigned char big[MSG_MAX], raw[8];
	const unsigned char *p;
	unsigned type, len, i;
	int sv[2];

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	fcntl(sv[1], F_SETFL, O_NONBLOCK);

	/* nothing yet */
	CHECK(msg_fill(sv[1], &mb) == 0 && msg_next(&mb, &type, &p, &len) == 0);

	/* three messages in one read come out one at a time, in order */
	for (i = 0; i < sizeof(big); i++)
		big[i] = (unsigned char)(i * 7);
	CHECK(msg_send(sv[0], 3, big, 644) == 0);
	CHECK(msg_send(sv[0], 0x81, "abc", 3) == 0);
	CHECK(msg_send(sv[0], 8, 0, 0) == 0);
	CHECK(msg_fill(sv[1], &mb) == 1);
	CHECK(msg_next(&mb, &type, &p, &len) == 1 && type == 3 && len == 644 && !memcmp(p, big, 644));
	CHECK(msg_next(&mb, &type, &p, &len) == 1 && type == 0x81 && len == 3 && !memcmp(p, "abc", 3));
	CHECK(msg_next(&mb, &type, &p, &len) == 1 && type == 8 && len == 0);
	CHECK(msg_next(&mb, &type, &p, &len) == 0);

	/* a message arriving a byte at a time */
	raw[0] = 5; raw[1] = 0; raw[2] = 2; raw[3] = 0; raw[4] = 0x34; raw[5] = 0x12;
	for (i = 0; i < 6; i++) {
		CHECK(msg_next(&mb, &type, &p, &len) == 0);
		CHECK(write(sv[0], raw + i, 1) == 1);
		CHECK(msg_fill(sv[1], &mb) == 1);
	}
	CHECK(msg_next(&mb, &type, &p, &len) == 1 && type == 5 && len == 2 && p[0] == 0x34 && p[1] == 0x12);

	/* the largest payload fits; a length over the limit is refused */
	CHECK(msg_send(sv[0], 9, big, MSG_MAX) == 0);
	CHECK(msg_fill(sv[1], &mb) == 1);
	CHECK(msg_next(&mb, &type, &p, &len) == 1 && len == MSG_MAX && !memcmp(p, big, MSG_MAX));
	raw[0] = 1; raw[1] = 0; raw[2] = 0xff; raw[3] = 0xff;
	CHECK(write(sv[0], raw, 4) == 4);
	CHECK(msg_fill(sv[1], &mb) == 1 && msg_next(&mb, &type, &p, &len) == -1);

	/* the other end going away is reported */
	close(sv[0]);
	mb.len = mb.used = 0;
	CHECK(msg_fill(sv[1], &mb) == -1);

	/* sending to a full socket fails cleanly instead of blocking */
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	for (i = 0; i < 100000 && msg_send(sv[0], 3, big, 644) == 0; i++)
		;
	CHECK(i > 10 && i < 100000);

	T_DONE("msg");
}
