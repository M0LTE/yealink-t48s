// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: the client for the audio service's control socket
 * (audiod/include/t48_audio_ctl.h, docs/audiod.md): routes, volume, mute,
 * tones, the ringer. Keeps trying once a second while the service is not
 * there; phone.c says how things should be again each time it comes back. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "shell.h"

static const char *path;
static int fd = -1;
static long long retry_at;
static unsigned char rx[512];
static size_t rxn;
static struct t48ac_state state;
static int have_state;

void audioc_init(const char *socket_path) { path = socket_path; }
int audioc_fd(void) { return fd; }
int audioc_connected(void) { return fd >= 0; }
const struct t48ac_state *audioc_state(void) { return have_state ? &state : NULL; }

static void drop(const char *why)
{
    if (fd < 0) return;
    logmsg("audio: lost the audio service (%s)", why);
    close(fd);
    fd = -1;
    rxn = 0;
    have_state = 0;
    retry_at = t48_now_ms() + 1000;
    phone_audio_link(0);
}

static void send_msg(int type, const unsigned char *payload, int len)
{
    unsigned char m[8] = {(unsigned char)type, (unsigned char)(type >> 8), (unsigned char)len, 0};
    if (fd < 0) return;
    memcpy(m + 4, payload, (size_t)len);
    if (send(fd, m, (size_t)len + 4, MSG_NOSIGNAL) != len + 4) drop("cannot send");
}

static void send1(int type, int v) { unsigned char b = (unsigned char)v; send_msg(type, &b, 1); }

void audioc_route(int route) { send1(T48AC_ROUTE, route); }
void audioc_mute(int on) { send1(T48AC_MUTE, on); }
void audioc_tone(int tone) { send1(T48AC_TONE, tone); }
void audioc_ring(int on) { send1(T48AC_RING, on); }
void audioc_key(int ch) { send1(T48AC_KEY, ch); }
/* An app's sound: whether it is in front, and with ring_fd its ring, which
 * travels as a descriptor beside the message. */
void audioc_app(int what, int ring_fd)
{
    unsigned char m[5] = {T48AC_APP, 0, 1, 0, (unsigned char)what};
    union { struct cmsghdr h; char space[CMSG_SPACE(sizeof(int))]; } cm;
    struct iovec iov = {m, sizeof m};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};

    if (fd < 0) return;
    if (ring_fd >= 0) {
        memset(&cm, 0, sizeof cm);
        mh.msg_control = &cm;
        mh.msg_controllen = sizeof cm;
        cm.h.cmsg_level = SOL_SOCKET;
        cm.h.cmsg_type = SCM_RIGHTS;
        cm.h.cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(&cm.h), &ring_fd, sizeof(int));
    }
    if (sendmsg(fd, &mh, MSG_NOSIGNAL) != (ssize_t)sizeof m) drop("cannot send");
}

void audioc_volume(int which, int level)
{
    unsigned char b[2] = {(unsigned char)which, (unsigned char)level};
    send_msg(T48AC_VOLUME, b, 2);
}

static void try_connect(long long now)
{
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    unsigned char hello[2] = {T48AC_VERSION, 0};

    retry_at = now + 1000;
    if (!path || strlen(path) >= sizeof sa.sun_path) return;
    strcpy(sa.sun_path, path);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) { close(s); return; }
    fd = s;
    rxn = 0;
    logmsg("audio: connected to the audio service at %s", path);
    send_msg(T48AC_HELLO, hello, 2);
}

int audioc_timeout(long long now)
{
    if (fd >= 0 || !path) return -1;
    return retry_at > now ? (int)(retry_at - now) : 0;
}

void audioc_tick(long long now)
{
    if (fd < 0 && path && now >= retry_at) try_connect(now);
}

void audioc_readable(void)
{
    if (fd < 0) return;
    ssize_t n = recv(fd, rx + rxn, sizeof rx - rxn, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n <= 0) { drop(n ? strerror(errno) : "closed"); return; }
    rxn += (size_t)n;
    while (fd >= 0 && rxn >= 4) {
        unsigned type = rx[0] | rx[1] << 8, len = rx[2] | rx[3] << 8;
        if (len > sizeof rx - 4) { drop("bad framing"); return; }
        if (rxn < 4 + len) break;
        if (type == T48AC_STATE && len >= 8) {
            int first = !have_state;
            memset(&state, 0, sizeof state);
            memcpy(&state, rx + 4, len < sizeof state ? len : sizeof state);
            have_state = 1;
            if (first) phone_audio_link(1);
            else phone_audio_state();
        } else if (type == T48AC_ERROR && len >= 4) {
            logmsg("audio: the service refused message type %d (reason %d)", rx[4] | rx[5] << 8, rx[6] | rx[7] << 8);
        }
        memmove(rx, rx + 4 + len, rxn - (4 + len));
        rxn -= 4 + len;
    }
}
