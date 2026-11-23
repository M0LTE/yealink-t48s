// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * The T48 app protocol, API level 1: what goes over the socket between the
 * shell and an app. The written description is docs/app-protocol.md; this
 * header is the same thing for C programs.
 *
 * Every message is a 4-byte header followed by `len` bytes of payload. All
 * numbers are little endian. A side that receives a type it does not know
 * skips it.
 */
#ifndef T48_PROTO_H
#define T48_PROTO_H

#include <stdint.h>

#define T48_API_LEVEL 1

/* The shell starts the app with these in its environment. */
#define T48_ENV_SOCKET "T48_SOCKET_FD"   /* number of the socket file descriptor */
#define T48_ENV_BUFFER "T48_BUFFER_FD"   /* number of the screen buffer file descriptor */
#define T48_ENV_APP_ID "T48_APP_ID"      /* the id from the manifest */
#define T48_ENV_APP_DATA "T48_APP_DATA"  /* the app's own writable folder */

struct t48_msg_hdr {
    uint16_t type;
    uint16_t len;   /* payload bytes after this header */
};

/* shell to app */
#define T48_MSG_HELLO 1     /* struct t48_msg_hello; always first */
#define T48_MSG_SETTING 2   /* "key=value", no terminator; zero or more, before START */
#define T48_MSG_START 3     /* no payload: the app is in front, draw now */
#define T48_MSG_KEY 4       /* struct t48_msg_key */
#define T48_MSG_TOUCH 5     /* struct t48_msg_touch */
#define T48_MSG_PAUSE 6     /* no payload: off the screen; frozen after one second */
#define T48_MSG_RESUME 7    /* no payload: in front again, redraw everything */
#define T48_MSG_QUIT 8      /* no payload: exit within one second or be killed */

/* app to shell */
#define T48_MSG_READY 0x81   /* struct t48_msg_ready; the answer to START */
#define T48_MSG_DAMAGE 0x82  /* struct t48_msg_rect: this part of the buffer changed */
#define T48_MSG_PAUSED 0x83  /* no payload: optional answer to PAUSE, "freeze me now" */
/* 0x84 is kept for asking for an audio stream (a later API level) */

#define T48_FORMAT_XRGB8888 0  /* one 32-bit word per pixel, 0x00RRGGBB */

struct t48_msg_hello {
    uint16_t api;       /* the shell's API level */
    uint16_t width;     /* pixels */
    uint16_t height;
    uint16_t format;    /* T48_FORMAT_* */
    uint32_t stride;    /* bytes from one row to the next */
    uint32_t size;      /* bytes to map from the buffer descriptor */
};

struct t48_msg_ready {
    uint16_t api;       /* the API level the app was written for */
};

struct t48_msg_key {
    uint16_t code;      /* T48_KEY_* */
    uint8_t state;      /* 1 pressed, 0 released */
    uint8_t pad;
};

struct t48_msg_touch {
    uint8_t state;      /* T48_TOUCH_* */
    uint8_t pad;
    int16_t x, y;       /* screen pixels */
};

struct t48_msg_rect {
    uint16_t x, y, w, h;
};

/* Key codes. Digits, star and hash are their ASCII characters. */
#define T48_KEY_STAR '*'
#define T48_KEY_HASH '#'
#define T48_KEY_UP 0x100
#define T48_KEY_DOWN 0x101
#define T48_KEY_LEFT 0x102
#define T48_KEY_RIGHT 0x103
#define T48_KEY_OK 0x104
#define T48_KEY_BACK 0x105   /* a short press on X; a long press is Home and never reaches the app */

#define T48_TOUCH_UP 0
#define T48_TOUCH_DOWN 1
#define T48_TOUCH_MOVE 2

#endif
