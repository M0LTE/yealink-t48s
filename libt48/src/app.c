// SPDX-License-Identifier: AGPL-3.0-or-later
/* libt48, the app side of the app protocol (docs/app-protocol.md): the socket
 * to the shell, the shared screen buffer, events and timers. */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "t48.h"

#define NTIMERS 8
#define NWATCH 8
#define NSETTINGS 32

static int sock = -1, closed;
static t48_surface screen;
static unsigned char rx[4096];
static size_t rx_n;
static struct { int period, repeat; long long next; } timers[NTIMERS];
static struct { int fd, id; } watch[NWATCH];
static int nwatch;
static char *settings[NSETTINGS];  /* "key=value" */
static int nsettings;

long long t48_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void send_msg(int type, const void *payload, size_t len)
{
    unsigned char b[64];
    struct t48_msg_hdr h = {(uint16_t)type, (uint16_t)len};
    if (sock < 0 || closed || len > sizeof b - sizeof h) return;
    memcpy(b, &h, sizeof h);
    if (len) memcpy(b + sizeof h, payload, len);
    size_t n = sizeof h + len, at = 0;
    while (at < n) {
        ssize_t k = send(sock, b + at, n - at, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { closed = 1; return; }
        at += (size_t)k;
    }
}

/* read more from the shell; blocks if nothing is waiting */
static void fill_rx(void)
{
    ssize_t k = read(sock, rx + rx_n, sizeof rx - rx_n);
    if (k < 0 && errno == EINTR) return;
    if (k <= 0) { closed = 1; return; }
    rx_n += (size_t)k;
}

/* take one whole message out of rx: 1 and its type, payload and length, or 0 */
static int take(int *type, unsigned char *payload, size_t cap, size_t *len)
{
    struct t48_msg_hdr h;
    if (rx_n < sizeof h) return 0;
    memcpy(&h, rx, sizeof h);
    if (h.len > sizeof rx - sizeof h) { closed = 1; return 0; }  /* nothing in level 1 is this long */
    if (rx_n < sizeof h + h.len) return 0;
    *type = h.type;
    *len = h.len < cap ? h.len : cap;
    memcpy(payload, rx + sizeof h, *len);
    rx_n -= sizeof h + h.len;
    memmove(rx, rx + sizeof h + h.len, rx_n);
    return 1;
}

int t48_app_init(void)
{
    const char *sfd = getenv(T48_ENV_SOCKET), *bfd = getenv(T48_ENV_BUFFER);
    unsigned char p[1024];
    size_t len;
    int type;

    if (!sfd || !bfd) {
        fprintf(stderr, "libt48: not started by the shell (run the app in the simulator, t48-sim)\n");
        return -1;
    }
    sock = atoi(sfd);
    for (;;) {  /* HELLO, settings, START */
        while (!take(&type, p, sizeof p - 1, &len)) {
            if (closed) return -1;
            fill_rx();
        }
        if (type == T48_MSG_HELLO && len >= sizeof(struct t48_msg_hello)) {
            struct t48_msg_hello h;
            memcpy(&h, p, sizeof h);
            if (h.format != T48_FORMAT_XRGB8888 || h.stride % 4) {
                fprintf(stderr, "libt48: the shell offers a screen format this library does not know\n");
                return -1;
            }
            void *m = mmap(NULL, h.size, PROT_READ | PROT_WRITE, MAP_SHARED, atoi(bfd), 0);
            if (m == MAP_FAILED) { perror("libt48: mapping the screen buffer"); return -1; }
            screen.px = m;
            screen.w = h.width;
            screen.h = h.height;
            screen.stride = (int)(h.stride / 4);
        } else if (type == T48_MSG_SETTING && nsettings < NSETTINGS) {
            p[len] = 0;
            settings[nsettings++] = strdup((char *)p);
        } else if (type == T48_MSG_START) {
            break;
        } else if (type == T48_MSG_QUIT) {
            return -1;
        }
    }
    if (!screen.px) return -1;
    struct t48_msg_ready r = {T48_API_LEVEL};
    send_msg(T48_MSG_READY, &r, sizeof r);
    return closed ? -1 : 0;
}

t48_surface *t48_screen(void) { return &screen; }
int t48_fd(void) { return sock; }

void t48_damage(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen.w) w = screen.w - x;
    if (y + h > screen.h) h = screen.h - y;
    if (w <= 0 || h <= 0) return;
    struct t48_msg_rect r = {(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h};
    send_msg(T48_MSG_DAMAGE, &r, sizeof r);
}

void t48_damage_all(void) { t48_damage(0, 0, screen.w, screen.h); }
void t48_paused(void) { send_msg(T48_MSG_PAUSED, NULL, 0); }

void t48_timer(int id, int period_ms, int repeat)
{
    if (id < 0 || id >= NTIMERS) return;
    timers[id].period = period_ms > 0 ? period_ms : 0;
    timers[id].repeat = repeat;
    timers[id].next = t48_now_ms() + period_ms;
}

int t48_watch_fd(int fd, int id)
{
    if (nwatch >= NWATCH) return -1;
    watch[nwatch].fd = fd;
    watch[nwatch++].id = id;
    return 0;
}

void t48_unwatch_fd(int fd)
{
    for (int i = 0; i < nwatch; i++)
        if (watch[i].fd == fd) { watch[i] = watch[--nwatch]; return; }
}

const char *t48_setting(const char *key, const char *def)
{
    size_t n = strlen(key);
    for (int i = 0; i < nsettings; i++)
        if (!strncmp(settings[i], key, n) && settings[i][n] == '=') return settings[i] + n + 1;
    return def;
}

/* turn the next message in rx into an event: 1 if there was one worth reporting */
static int next_event(t48_event *ev)
{
    unsigned char p[64];
    size_t len;
    int type;

    while (take(&type, p, sizeof p, &len)) {
        memset(ev, 0, sizeof *ev);
        if (type == T48_MSG_KEY && len >= sizeof(struct t48_msg_key)) {
            struct t48_msg_key k;
            memcpy(&k, p, sizeof k);
            ev->type = T48_EV_KEY;
            ev->key = k.code;
            ev->state = k.state;
            return 1;
        }
        if (type == T48_MSG_TOUCH && len >= sizeof(struct t48_msg_touch)) {
            struct t48_msg_touch t;
            memcpy(&t, p, sizeof t);
            ev->type = T48_EV_TOUCH;
            ev->state = t.state;
            ev->x = t.x;
            ev->y = t.y;
            return 1;
        }
        if (type == T48_MSG_PAUSE) { ev->type = T48_EV_PAUSE; return 1; }
        if (type == T48_MSG_RESUME) { ev->type = T48_EV_RESUME; return 1; }
        if (type == T48_MSG_QUIT) { ev->type = T48_EV_QUIT; return 1; }
        /* anything else: from a newer shell, skip it */
    }
    return 0;
}

int t48_poll(t48_event *ev, int timeout_ms)
{
    long long deadline = timeout_ms < 0 ? -1 : t48_now_ms() + timeout_ms;

    for (;;) {
        if (next_event(ev)) return 1;
        if (closed || sock < 0) return -1;

        long long now = t48_now_ms(), wake = deadline;
        for (int i = 0; i < NTIMERS; i++) {
            if (!timers[i].period) continue;
            if (timers[i].next <= now) {
                if (timers[i].repeat) {
                    timers[i].next += timers[i].period;
                    if (timers[i].next <= now) timers[i].next = now + timers[i].period;
                } else {
                    timers[i].period = 0;
                }
                memset(ev, 0, sizeof *ev);
                ev->type = T48_EV_TIMER;
                ev->id = i;
                return 1;
            }
            if (wake < 0 || timers[i].next < wake) wake = timers[i].next;
        }
        /* past the deadline still looks once, so t48_poll(ev, 0) reads the socket */
        struct pollfd pf[1 + NWATCH];
        pf[0].fd = sock;
        pf[0].events = POLLIN;
        for (int i = 0; i < nwatch; i++) {
            pf[1 + i].fd = watch[i].fd;
            pf[1 + i].events = POLLIN;
        }
        int r = poll(pf, (nfds_t)(1 + nwatch), wake < 0 ? -1 : wake > now ? (int)(wake - now) : 0);
        if (r <= 0) {
            if (deadline >= 0 && t48_now_ms() >= deadline) {
                memset(ev, 0, sizeof *ev);
                return 0;
            }
            continue;
        }
        if (pf[0].revents) {
            fill_rx();
            continue;
        }
        for (int i = 0; i < nwatch; i++)
            if (pf[1 + i].revents) {
                memset(ev, 0, sizeof *ev);
                ev->type = T48_EV_FD;
                ev->id = watch[i].id;
                return 1;
            }
    }
}
