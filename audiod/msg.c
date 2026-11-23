// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-audiod: message framing. See msg.h. */
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include "msg.h"

int msg_fill(int fd, struct msgbuf *b)
{
	ssize_t n;

	if (b->used) {
		memmove(b->buf, b->buf + b->used, b->len - b->used);
		b->len -= b->used;
		b->used = 0;
	}
	if (b->len == sizeof(b->buf))
		return 1;	/* full: the caller has messages to take first */
	do
		n = read(fd, b->buf + b->len, sizeof(b->buf) - b->len);
	while (n < 0 && errno == EINTR);
	if (n > 0) {
		b->len += (size_t)n;
		return 1;
	}
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return 0;
	return -1;
}

int msg_next(struct msgbuf *b, unsigned *type, const unsigned char **payload, unsigned *len)
{
	const unsigned char *p = b->buf + b->used;
	size_t have = b->len - b->used;
	unsigned l;

	if (have < 4)
		return 0;
	l = p[2] | (unsigned)p[3] << 8;
	if (l > MSG_MAX)
		return -1;
	if (have < 4 + l)
		return 0;
	*type = p[0] | (unsigned)p[1] << 8;
	*payload = p + 4;
	*len = l;
	b->used += 4 + l;
	return 1;
}

int msg_send(int fd, unsigned type, const void *payload, unsigned len)
{
	unsigned char h[4] = { (unsigned char)type, (unsigned char)(type >> 8),
			       (unsigned char)len, (unsigned char)(len >> 8) };
	struct iovec iov[2] = { { h, 4 }, { (void *)payload, len } };
	struct msghdr mh;
	ssize_t n;

	memset(&mh, 0, sizeof(mh));
	mh.msg_iov = iov;
	mh.msg_iovlen = len ? 2 : 1;
	do
		n = sendmsg(fd, &mh, MSG_NOSIGNAL | MSG_DONTWAIT);
	while (n < 0 && errno == EINTR);
	return n == (ssize_t)(4 + len) ? 0 : -1;
}
