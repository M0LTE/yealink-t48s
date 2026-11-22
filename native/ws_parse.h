// SPDX-License-Identifier: AGPL-3.0-or-later
// From M0LTE/esp32-knob-pages main/ws_parse.h (AGPL-3.0), unchanged apart from this header.
// WebSocket (RFC 6455) client-side frame decoding, and the one frame a
// listen-only client ever sends (a masked pong or close). No ESP-IDF
// includes, so the host test in test/ builds it too.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_OP_CONT 0x0
#define WS_OP_TEXT 0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING 0x9
#define WS_OP_PONG 0xA

// A complete message (text or binary, reassembled from fragments) or a control
// frame (close, ping, pong). payload is valid only during the call.
typedef void (*ws_msg_cb_t)(void *ctx, uint8_t op, const uint8_t *payload, size_t len);

typedef struct {
    uint8_t *buf;           // message buffer, owned by the caller (one, reused)
    size_t cap;
    size_t len;             // bytes of the current message so far
    uint8_t msg_op;         // opcode of the message being reassembled, 0 = none
    bool overflow;          // current message is too big for buf: it is dropped
    // frame header being read
    uint8_t hdr[14];
    int hlen, hneed;
    // frame payload being read
    uint8_t op;
    bool fin, masked;
    uint8_t mask[4];
    uint64_t remain;
    uint64_t pos;           // payload bytes of this frame so far (for unmasking)
    uint8_t ctrl[125];      // control frame payload
    size_t clen;
} ws_dec_t;

void ws_dec_init(ws_dec_t *d, uint8_t *buf, size_t cap);
// Feed received bytes. Calls cb for each complete message and each control
// frame. Returns 0, or -1 on a protocol error (the connection should close).
int ws_dec_feed(ws_dec_t *d, const uint8_t *p, size_t n, ws_msg_cb_t cb, void *ctx);

// Builds a masked client control frame (pong or close, payload up to 125
// bytes) into out (room for 6 + len bytes). Returns its length.
size_t ws_control_frame(uint8_t op, const uint8_t *payload, size_t len, const uint8_t mask[4], uint8_t *out);

// Sec-WebSocket-Key from 16 random bytes (base64, 24 chars + NUL in out).
void ws_key(const uint8_t rnd[16], char out[25]);
