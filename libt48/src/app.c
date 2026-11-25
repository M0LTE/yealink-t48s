// SPDX-License-Identifier: AGPL-3.0-or-later
/* libt48, the app side of the app protocol (docs/app-protocol.md): the socket
 * to the shell, the shared screen buffer, events and timers. */
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

/* flipping (level 3): the shell's buffers, and where each one is */
#define NFLIP 8
static struct t48_msg_flipinfo flipinfo;
static t48_surface flip_s[NFLIP];
static int flip_on, flip_draw, flip_pending = -1, flip_front = -1;
static t48_event flip_ev;          /* a FLIPPED taken while waiting, still to be reported by t48_poll */
static int flip_ev_waiting;

/* the shell's font file (level 2): every family it has, to point the faces into */
static const unsigned char *fonts;
static size_t fonts_n;
static t48_font_family fonts_mapped;

_Static_assert(sizeof(t48_glyph) == sizeof(struct t48_fonts_glyph) && offsetof(t48_glyph, off) == offsetof(struct t48_fonts_glyph, off),
               "a glyph in the font file is a t48_glyph");

static void fonts_map(void)
{
    const char *fd = getenv(T48_ENV_FONTS);
    struct stat st;
    struct t48_fonts_hdr h;
    if (!fd || fstat(atoi(fd), &st) < 0 || st.st_size < (off_t)sizeof h) return;
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, atoi(fd), 0);
    if (m == MAP_FAILED) return;
    memcpy(&h, m, sizeof h);
    if (memcmp(h.magic, T48_FONTS_MAGIC, 4) || h.version != 1) { munmap(m, (size_t)st.st_size); return; }
    fonts = m;
    fonts_n = (size_t)st.st_size;
}

/* The family of that name: B612 is built in, the others come from the font
 * file. NULL if there is none, or the file is not sound. */
static const t48_font_family *fonts_named(const char *name)
{
    struct t48_fonts_hdr h;
    if (!strcmp(name, t48_fonts_b612.name) || !fonts) return strcmp(name, t48_fonts_b612.name) ? NULL : &t48_fonts_b612;
    memcpy(&h, fonts, sizeof h);
    for (size_t i = 0; i < h.families; i++) {
        struct t48_fonts_family f;
        size_t at = sizeof h + i * sizeof f;
        if (at + sizeof f > fonts_n) return NULL;
        memcpy(&f, fonts + at, sizeof f);
        if (strncmp(f.name, name, sizeof f.name)) continue;
        snprintf(fonts_mapped.name, sizeof fonts_mapped.name, "%s", name);
        for (int k = 0; k < T48_FACES; k++) {
            size_t g = f.face[k].glyphs, a = f.face[k].coverage;
            if (g % 4 || g > fonts_n || fonts_n - g < 95 * sizeof(t48_glyph) || a > fonts_n) return NULL;
            const t48_glyph *gl = (const t48_glyph *)(fonts + g);
            for (int c = 0; c < 95; c++)
                if (gl[c].w < 0 || gl[c].h < 0 || gl[c].off > fonts_n - a || (size_t)gl[c].w * (size_t)gl[c].h > fonts_n - a - gl[c].off)
                    return NULL;
            fonts_mapped.face[k] = (t48_font){f.face[k].ascent, f.face[k].height, gl, fonts + a};
        }
        return &fonts_mapped;
    }
    return NULL;
}

/* THEME: the look, and the family that goes with it (B612 if we have not got that one) */
static void theme_msg(const unsigned char *p, size_t len)
{
    t48_theme t;
    if (len < sizeof t) return;   /* a later level only adds to the end */
    memcpy(&t, p, sizeof t);
    t.name[sizeof t.name - 1] = t.font[sizeof t.font - 1] = 0;
    const t48_font_family *f = fonts_named(t.font);
    t48_theme_use(&t, f ? f : &t48_fonts_b612);
}

long long t48_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

long long t48_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
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
    fonts_map();
    for (;;) {  /* HELLO, settings, the theme, START */
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
        } else if (type == T48_MSG_THEME) {
            theme_msg(p, len);
        } else if (type == T48_MSG_FLIPINFO && len >= sizeof flipinfo) {
            memcpy(&flipinfo, p, sizeof flipinfo);
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
    flip_on = 0;   /* the shell goes back to this buffer */
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

/* FLIPPED: that buffer is on the screen, and the one before it is free */
static void flipped_msg(const unsigned char *p, t48_event *ev)
{
    struct t48_msg_flipped f;
    memcpy(&f, p, sizeof f);
    flip_front = f.buffer;
    if (flip_pending == f.buffer) flip_pending = -1;
    memset(ev, 0, sizeof *ev);
    ev->type = T48_EV_FLIPPED;
    ev->id = f.buffer;
    ev->frame = f.frame;
    ev->time_us = (long long)f.time_us;
}

/* Events set aside by t48_flip_wait() when rx filled up before the shell's
 * answer came: reported by t48_poll, oldest first. If even this fills, the
 * oldest go. */
#define NQUEUE 256
static t48_event queue[NQUEUE];
static unsigned queue_at, queue_n;

/* turn the next message in rx into an event: 1 if there was one worth reporting */
static int rx_event(t48_event *ev)
{
    unsigned char p[512];
    size_t len;
    int type;

    while (take(&type, p, sizeof p, &len)) {
        memset(ev, 0, sizeof *ev);
        if (type == T48_MSG_KEY && len >= sizeof(struct t48_msg_key)) {
            struct t48_msg_key3 k = {0};
            memcpy(&k, p, len < sizeof k ? sizeof(struct t48_msg_key) : sizeof k);
            ev->type = T48_EV_KEY;
            ev->key = k.code;
            ev->state = k.state;
            ev->time_us = (long long)k.time_us;
            return 1;
        }
        if (type == T48_MSG_TOUCH && len >= sizeof(struct t48_msg_touch)) {
            struct t48_msg_touch3 t = {0};
            memcpy(&t, p, len < sizeof t ? sizeof(struct t48_msg_touch) : sizeof t);
            ev->type = T48_EV_TOUCH;
            ev->state = t.state;
            ev->x = t.x;
            ev->y = t.y;
            ev->time_us = (long long)t.time_us;
            return 1;
        }
        if (type == T48_MSG_FLIPPED && len >= sizeof(struct t48_msg_flipped)) {
            flipped_msg(p, ev);
            return 1;
        }
        if (type == T48_MSG_PAUSE) { ev->type = T48_EV_PAUSE; return 1; }
        if (type == T48_MSG_RESUME) { ev->type = T48_EV_RESUME; return 1; }
        if (type == T48_MSG_QUIT) { ev->type = T48_EV_QUIT; return 1; }
        if (type == T48_MSG_THEME) { theme_msg(p, len); ev->type = T48_EV_THEME; return 1; }
        /* anything else: from a newer shell, skip it */
    }
    return 0;
}

static int next_event(t48_event *ev)
{
    if (flip_ev_waiting) { *ev = flip_ev; flip_ev_waiting = 0; return 1; }
    if (queue_n) { *ev = queue[queue_at]; queue_at = (queue_at + 1) % NQUEUE; queue_n--; return 1; }
    return rx_event(ev);
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

/* ---------- flipping ---------- */

int t48_flip_period_us(void) { return flipinfo.count >= 2 ? (int)flipinfo.period_us : 0; }

t48_surface *t48_flip_start(void)
{
    const char *fd = getenv(T48_ENV_FLIP);
    int n = flipinfo.count > NFLIP ? NFLIP : flipinfo.count;
    if (n < 2 || !fd || !screen.px) return NULL;
    if (!flip_s[0].px) {
        unsigned char *m = mmap(NULL, (size_t)n * flipinfo.size, PROT_READ | PROT_WRITE, MAP_SHARED, atoi(fd), 0);
        if (m == MAP_FAILED) return NULL;
        for (int i = 0; i < n; i++) {
            flip_s[i] = screen;
            flip_s[i].px = (uint32_t *)(m + (size_t)i * flipinfo.size);
        }
    }
    flipinfo.count = (uint16_t)n;
    if (!flip_on) {
        /* a buffer that is neither on the screen nor on its way there */
        flip_draw = 0;
        while (flip_draw == flip_front || flip_draw == flip_pending) flip_draw++;
        flip_on = 1;
    }
    return &flip_s[flip_draw];
}

void t48_flip_stop(void) { flip_on = 0; }

/* Take the first FLIPPED out of rx, wherever it is among the other messages,
 * reading more if it has not come yet. The others stay for t48_poll. */
void t48_flip_wait(void)
{
    while (flip_pending >= 0 && !closed) {
        size_t at = 0;
        struct t48_msg_hdr h;
        int found = 0;
        while (rx_n - at >= sizeof h) {
            memcpy(&h, rx + at, sizeof h);
            if (rx_n - at < sizeof h + h.len) break;
            if (h.type == T48_MSG_FLIPPED && h.len >= sizeof(struct t48_msg_flipped)) {
                flipped_msg(rx + at + sizeof h, &flip_ev);   /* t48_poll reports it later */
                flip_ev_waiting = 1;
                rx_n -= sizeof h + h.len;
                memmove(rx + at, rx + at + sizeof h + h.len, rx_n - at);
                found = 1;
                break;
            }
            at += sizeof h + h.len;
        }
        if (found) continue;
        if (rx_n == sizeof rx) {
            /* full of keys and touches and still no answer: set them aside to make room */
            t48_event ev;
            while (rx_event(&ev)) {
                if (queue_n == NQUEUE) { queue_at = (queue_at + 1) % NQUEUE; queue_n--; }
                queue[(queue_at + queue_n++) % NQUEUE] = ev;
            }
            if (rx_n == sizeof rx) { closed = 1; return; }   /* not messages at all */
            continue;
        }
        fill_rx();
    }
}

t48_surface *t48_flip(void)
{
    if (!flip_s[0].px) return t48_flip_start();   /* never started: nothing has been drawn to show */
    flip_on = 1;   /* after a t48_damage() too: the picture drawn in the buffer is shown all the same */
    if (flip_pending >= 0) t48_flip_wait();   /* one at a time: this is what keeps time with the screen */
    struct t48_msg_flip f = {(uint16_t)flip_draw, 0};
    send_msg(T48_MSG_FLIP, &f, sizeof f);
    flip_pending = flip_draw;
    int next = -1;
    for (int i = 0; i < flipinfo.count && next < 0; i++)
        if (i != flip_pending && i != flip_front) next = i;
    if (next < 0) {
        /* two buffers: the other one is free once this one is on the screen */
        t48_flip_wait();
        next = flip_draw ? 0 : 1;
    }
    flip_draw = next;
    return &flip_s[flip_draw];
}
