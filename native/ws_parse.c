// SPDX-License-Identifier: AGPL-3.0-or-later
// From M0LTE/esp32-knob-pages main/ws_parse.c (AGPL-3.0), unchanged apart from this header.
#include "ws_parse.h"

#include <string.h>

void ws_dec_init(ws_dec_t *d, uint8_t *buf, size_t cap)
{
    memset(d, 0, sizeof *d);
    d->buf = buf;
    d->cap = cap;
    d->hneed = 2;
}

static void frame_done(ws_dec_t *d, ws_msg_cb_t cb, void *ctx)
{
    if (d->op >= 0x8) {
        cb(ctx, d->op, d->ctrl, d->clen);
    } else if (d->fin) {
        if (!d->overflow && d->msg_op) cb(ctx, d->msg_op, d->buf, d->len);
        d->msg_op = 0;
        d->len = 0;
        d->overflow = false;
    }
    d->hlen = 0;
    d->hneed = 2;
}

// Header complete: work out the payload. -1 on a protocol error.
static int header_done(ws_dec_t *d)
{
    uint8_t b0 = d->hdr[0], b1 = d->hdr[1];
    d->fin = b0 & 0x80;
    d->op = b0 & 0x0F;
    d->masked = b1 & 0x80;
    uint64_t len = b1 & 0x7F;
    int i = 2;
    if (len == 126) {
        len = (uint64_t)d->hdr[2] << 8 | d->hdr[3];
        i = 4;
    } else if (len == 127) {
        len = 0;
        for (int k = 0; k < 8; k++) len = len << 8 | d->hdr[2 + k];
        i = 10;
    }
    if (d->masked) memcpy(d->mask, d->hdr + i, 4);
    d->remain = len;
    d->pos = 0;
    if (d->op >= 0x8) {
        if (!d->fin || len > 125) return -1;
        d->clen = 0;
    } else if (d->op == WS_OP_CONT) {
        if (!d->msg_op) return -1;  // continuation of nothing
    } else if (d->op == WS_OP_TEXT || d->op == WS_OP_BINARY) {
        if (d->msg_op) return -1;   // new message while one is unfinished
        d->msg_op = d->op;
        d->len = 0;
        d->overflow = false;
    } else {
        return -1;                  // reserved opcode
    }
    return 0;
}

int ws_dec_feed(ws_dec_t *d, const uint8_t *p, size_t n, ws_msg_cb_t cb, void *ctx)
{
    size_t i = 0;
    while (i < n) {
        if (d->hlen < d->hneed) {
            d->hdr[d->hlen++] = p[i++];
            if (d->hlen == 2) {
                uint8_t l = d->hdr[1] & 0x7F;
                d->hneed = 2 + (l == 126 ? 2 : l == 127 ? 8 : 0) + ((d->hdr[1] & 0x80) ? 4 : 0);
            }
            if (d->hlen == d->hneed) {
                if (header_done(d) < 0) return -1;
                if (d->remain == 0) frame_done(d, cb, ctx);
            }
            continue;
        }
        // Payload: take as much as is here in one go.
        size_t take = n - i;
        if (take > d->remain) take = (size_t)d->remain;
        const uint8_t *src = p + i;
        if (d->op >= 0x8) {
            for (size_t k = 0; k < take; k++)
                d->ctrl[d->clen++] = d->masked ? src[k] ^ d->mask[(d->pos + k) & 3] : src[k];
        } else if (!d->overflow) {
            if (d->len + take > d->cap) {
                d->overflow = true;
            } else {
                uint8_t *dst = d->buf + d->len;
                if (d->masked)
                    for (size_t k = 0; k < take; k++) dst[k] = src[k] ^ d->mask[(d->pos + k) & 3];
                else
                    memcpy(dst, src, take);
                d->len += take;
            }
        }
        d->pos += take;
        d->remain -= take;
        i += take;
        if (d->remain == 0) frame_done(d, cb, ctx);
    }
    return 0;
}

size_t ws_control_frame(uint8_t op, const uint8_t *payload, size_t len, const uint8_t mask[4], uint8_t *out)
{
    if (len > 125) len = 125;
    out[0] = 0x80 | (op & 0x0F);
    out[1] = 0x80 | (uint8_t)len;
    memcpy(out + 2, mask, 4);
    for (size_t k = 0; k < len; k++) out[6 + k] = payload[k] ^ mask[k & 3];
    return 6 + len;
}

void ws_key(const uint8_t rnd[16], char out[25])
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
