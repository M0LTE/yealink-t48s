// SPDX-License-Identifier: AGPL-3.0-or-later
/* libt48: a small WebSocket client (RFC 6455), plain ws:// for now.
 *
 * The frame decoder is from M0LTE/esp32-knob-pages main/ws_parse.c (AGPL-3.0),
 * by way of the firmware repo's old native/ws_parse.c; the connection handling
 * around it is new. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "t48.h"

#define OP_CONT 0x0
#define OP_TEXT 0x1
#define OP_BINARY 0x2
#define OP_CLOSE 0x8
#define OP_PING 0x9
#define OP_PONG 0xA

struct t48_ws {
    int fd;
    int closing;
    unsigned char *buf;     /* message being reassembled; one spare byte for a NUL */
    size_t cap, len;
    uint8_t msg_op;         /* opcode of the message being reassembled, 0 = none */
    int overflow;           /* the current message is too big: it is dropped */
    uint8_t hdr[14];        /* frame header being read */
    int hlen, hneed;
    uint8_t op;
    int fin, masked;
    uint8_t mask[4];
    uint64_t remain, pos;
    uint8_t ctrl[125];
    size_t clen;
    unsigned char pending[4096];  /* frames that came in with the HTTP answer */
    size_t npending;
};

/* ---------- sending (client frames are always masked) ---------- */

static int send_all(int fd, const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n) {
        ssize_t k = send(fd, b, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = {.fd = fd, .events = POLLOUT};
            if (poll(&pf, 1, 2000) <= 0) return -1;
            continue;
        }
        if (k <= 0) return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static int send_frame(t48_ws *ws, int op, const void *payload, size_t len)
{
    unsigned char h[14], m[4], chunk[512];
    size_t hl = 2;
    for (int i = 0; i < 4; i++) m[i] = (unsigned char)rand();
    h[0] = 0x80 | (op & 0x0F);
    if (len < 126) {
        h[1] = 0x80 | (unsigned char)len;
    } else if (len < 65536) {
        h[1] = 0x80 | 126;
        h[2] = (unsigned char)(len >> 8);
        h[3] = (unsigned char)len;
        hl = 4;
    } else {
        return -1;  /* nothing this library sends is that long */
    }
    memcpy(h + hl, m, 4);
    if (send_all(ws->fd, h, hl + 4)) return -1;
    const unsigned char *p = payload;
    for (size_t at = 0; at < len;) {
        size_t n = len - at < sizeof chunk ? len - at : sizeof chunk;
        for (size_t k = 0; k < n; k++) chunk[k] = p[at + k] ^ m[(at + k) & 3];
        if (send_all(ws->fd, chunk, n)) return -1;
        at += n;
    }
    return 0;
}

int t48_ws_send_text(t48_ws *ws, const char *text, size_t len)
{
    return ws && !ws->closing ? send_frame(ws, OP_TEXT, text, len) : -1;
}

/* ---------- receiving ---------- */

static void frame_done(t48_ws *ws, t48_ws_cb cb, void *ctx)
{
    if (ws->op >= 0x8) {
        if (ws->op == OP_PING) {
            send_frame(ws, OP_PONG, ws->ctrl, ws->clen);
        } else if (ws->op == OP_CLOSE) {
            if (!ws->closing) send_frame(ws, OP_CLOSE, ws->ctrl, ws->clen >= 2 ? 2 : 0);
            ws->closing = 1;
        }
    } else if (ws->fin) {
        if (!ws->overflow && ws->msg_op) {
            ws->buf[ws->len] = 0;
            cb(ctx, ws->msg_op == OP_TEXT ? T48_WS_TEXT : T48_WS_BINARY, ws->buf, ws->len);
        }
        ws->msg_op = 0;
        ws->len = 0;
        ws->overflow = 0;
    }
    ws->hlen = 0;
    ws->hneed = 2;
}

/* the header is complete: work out the payload. -1 on a protocol error */
static int header_done(t48_ws *ws)
{
    uint8_t b0 = ws->hdr[0], b1 = ws->hdr[1];
    uint64_t len = b1 & 0x7F;
    int i = 2;
    ws->fin = b0 & 0x80;
    ws->op = b0 & 0x0F;
    ws->masked = b1 & 0x80;
    if (len == 126) {
        len = (uint64_t)ws->hdr[2] << 8 | ws->hdr[3];
        i = 4;
    } else if (len == 127) {
        len = 0;
        for (int k = 0; k < 8; k++) len = len << 8 | ws->hdr[2 + k];
        i = 10;
    }
    if (ws->masked) memcpy(ws->mask, ws->hdr + i, 4);
    ws->remain = len;
    ws->pos = 0;
    if (ws->op >= 0x8) {
        if (!ws->fin || len > 125) return -1;
        ws->clen = 0;
    } else if (ws->op == OP_CONT) {
        if (!ws->msg_op) return -1;  /* continuation of nothing */
    } else if (ws->op == OP_TEXT || ws->op == OP_BINARY) {
        if (ws->msg_op) return -1;   /* a new message while one is unfinished */
        ws->msg_op = ws->op;
        ws->len = 0;
        ws->overflow = 0;
    } else {
        return -1;                   /* a reserved opcode */
    }
    return 0;
}

static int feed(t48_ws *ws, const uint8_t *p, size_t n, t48_ws_cb cb, void *ctx)
{
    size_t i = 0;
    while (i < n) {
        if (ws->hlen < ws->hneed) {
            ws->hdr[ws->hlen++] = p[i++];
            if (ws->hlen == 2) {
                uint8_t l = ws->hdr[1] & 0x7F;
                ws->hneed = 2 + (l == 126 ? 2 : l == 127 ? 8 : 0) + ((ws->hdr[1] & 0x80) ? 4 : 0);
            }
            if (ws->hlen == ws->hneed) {
                if (header_done(ws) < 0) return -1;
                if (ws->remain == 0) frame_done(ws, cb, ctx);
            }
            continue;
        }
        size_t take = n - i;
        if (take > ws->remain) take = (size_t)ws->remain;
        const uint8_t *src = p + i;
        if (ws->op >= 0x8) {
            for (size_t k = 0; k < take; k++)
                ws->ctrl[ws->clen++] = ws->masked ? src[k] ^ ws->mask[(ws->pos + k) & 3] : src[k];
        } else if (!ws->overflow) {
            if (ws->len + take > ws->cap) {
                ws->overflow = 1;
            } else {
                uint8_t *dst = ws->buf + ws->len;
                if (ws->masked)
                    for (size_t k = 0; k < take; k++) dst[k] = src[k] ^ ws->mask[(ws->pos + k) & 3];
                else
                    memcpy(dst, src, take);
                ws->len += take;
            }
        }
        ws->pos += take;
        ws->remain -= take;
        i += take;
        if (ws->remain == 0) frame_done(ws, cb, ctx);
    }
    return 0;
}

int t48_ws_read(t48_ws *ws, t48_ws_cb cb, void *ctx)
{
    uint8_t b[8192];
    if (!ws || ws->closing) return -1;
    if (ws->npending) {
        size_t n = ws->npending;
        ws->npending = 0;
        if (feed(ws, ws->pending, n, cb, ctx)) return -1;
        if (ws->closing) return -1;
    }
    ssize_t k = recv(ws->fd, b, sizeof b, MSG_DONTWAIT);
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    if (k <= 0) return -1;
    if (feed(ws, b, (size_t)k, cb, ctx)) return -1;
    return ws->closing ? -1 : 0;
}

/* ---------- connecting ---------- */

int t48_ws_parse_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn)
{
    if (strncmp(url, "ws://", 5)) return 0;
    const char *h = url + 5, *slash = strchr(h, '/'), *colon = strchr(h, ':');
    const char *hend = slash ? slash : h + strlen(h);
    if (colon && colon < hend) {
        *port = atoi(colon + 1);
        hend = colon;
    } else {
        *port = 80;
    }
    if (hend == h || (size_t)(hend - h) >= hn) return 0;
    memcpy(host, h, (size_t)(hend - h));
    host[hend - h] = 0;
    snprintf(path, pn, "%s", slash ? slash : "/");
    return *port > 0 && *port < 65536;
}

static void ws_key(const uint8_t rnd[16], char out[25])
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < 16; i += 3) {
        uint32_t v = (uint32_t)rnd[i] << 16 | (i + 1 < 16 ? (uint32_t)rnd[i + 1] << 8 : 0) | (i + 2 < 16 ? rnd[i + 2] : 0);
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = i + 1 < 16 ? b64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < 16 ? b64[v & 63] : '=';
    }
    out[o] = 0;
}

#define ERR(...) do { if (err && errn) snprintf(err, errn, __VA_ARGS__); } while (0)

t48_ws *t48_ws_open(const char *url, int timeout_ms, size_t max_msg, char *err, size_t errn)
{
    static int seeded;
    char host[128], path[256], ps[12], key[25], req[512];
    int port, s = -1;
    uint8_t rnd[16];
    unsigned char head[4096];
    size_t got = 0;
    char *end = NULL;
    long long deadline = t48_now_ms() + timeout_ms;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    t48_ws *ws;

    if (!seeded) { srand((unsigned)time(NULL) ^ (unsigned)getpid()); seeded = 1; }
    if (!t48_ws_parse_url(url, host, sizeof host, &port, path, sizeof path)) { ERR("not a ws:// address"); return NULL; }
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { ERR("cannot find %s", host); return NULL; }
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) { freeaddrinfo(res); ERR("no socket"); return NULL; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    int r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r && errno != EINPROGRESS) { ERR("%s", strerror(errno)); goto fail; }
    if (r) {
        struct pollfd pf = {.fd = s, .events = POLLOUT};
        int left = (int)(deadline - t48_now_ms()), e = 0;
        socklen_t el = sizeof e;
        if (left <= 0 || poll(&pf, 1, left) <= 0) { ERR("no answer"); goto fail; }
        getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el);
        if (e) { ERR("%s", strerror(e)); goto fail; }
    }
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)rand();
    ws_key(rnd, key);
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                     path, host, port, key);
    if (send_all(s, req, (size_t)n)) { ERR("cannot send"); goto fail; }
    while (!end && got < sizeof head - 1) {
        struct pollfd pf = {.fd = s, .events = POLLIN};
        int left = (int)(deadline - t48_now_ms());
        if (left <= 0 || poll(&pf, 1, left) <= 0) break;
        ssize_t k = recv(s, head + got, sizeof head - 1 - got, 0);
        if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (k <= 0) break;
        got += (size_t)k;
        head[got] = 0;
        end = strstr((char *)head, "\r\n\r\n");
    }
    if (!end || (strncmp((char *)head, "HTTP/1.1 101", 12) && strncmp((char *)head, "HTTP/1.0 101", 12))) {
        ERR(got ? "refused: %.40s" : "no answer to the upgrade", (char *)head);
        goto fail;
    }
    ws = calloc(1, sizeof *ws);
    if (!ws || !(ws->buf = malloc(max_msg + 1))) { free(ws); ERR("out of memory"); goto fail; }
    ws->fd = s;
    ws->cap = max_msg;
    ws->hneed = 2;
    size_t hl = (size_t)((end + 4) - (char *)head);
    /* bytes after the HTTP head are the first frames: the first read takes them */
    if (got > hl) {
        memcpy(ws->pending, head + hl, got - hl);
        ws->npending = got - hl;
    }
    return ws;
fail:
    close(s);
    return NULL;
}

int t48_ws_fd(const t48_ws *ws) { return ws ? ws->fd : -1; }

void t48_ws_close(t48_ws *ws)
{
    if (!ws) return;
    if (!ws->closing) send_frame(ws, OP_CLOSE, "\x03\xe8", 2);  /* 1000: normal */
    close(ws->fd);
    free(ws->buf);
    free(ws);
}
