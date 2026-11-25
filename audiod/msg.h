/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48-audiod: message framing shared by both sockets (u16 type, u16 len,
 * payload; little endian), for sockets in non-blocking mode.
 */
#ifndef AUDIOD_MSG_H
#define AUDIOD_MSG_H
#include <stddef.h>

#define MSG_MAX 2048	/* largest payload accepted */

struct msgbuf {
	unsigned char buf[MSG_MAX + 4];
	size_t len;	/* bytes held */
	size_t used;	/* bytes at the front already handed out by msg_next */
};

/* Read what the socket has. 1 if anything arrived, 0 if nothing yet, -1 if
 * the other end closed or failed. */
int msg_fill(int fd, struct msgbuf *b);
/* The same, and a descriptor that came with the bytes (SCM_RIGHTS) is put in
 * *passed, closing any that was there. One at a time is all that is kept. */
int msg_fill_fd(int fd, struct msgbuf *b, int *passed);
/* Next whole message: 1 (payload is valid until the next msg_fill), 0 if
 * there is not a whole one yet, -1 if the length is over MSG_MAX. */
int msg_next(struct msgbuf *b, unsigned *type, const unsigned char **payload, unsigned *len);
/* Send one message whole or not at all: 0, or -1 (the caller should drop a
 * client that cannot keep up). */
int msg_send(int fd, unsigned type, const void *payload, unsigned len);

#endif
