// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * The T48 app protocol, API level 4: what goes over the socket between the
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

/* 2 added THEME and the font file; a level 1 app or shell skips both.
 * 3 added flipping (FLIPINFO, FLIP, FLIPPED, the flip buffers), times on KEY
 * and TOUCH, and the soft keys; an older app or shell sees none of it.
 * 4 added sound (the sound ring); nothing on the socket changed. */
#define T48_API_LEVEL 4

/* The shell starts the app with these in its environment. */
#define T48_ENV_SOCKET "T48_SOCKET_FD"   /* number of the socket file descriptor */
#define T48_ENV_BUFFER "T48_BUFFER_FD"   /* number of the screen buffer file descriptor */
#define T48_ENV_APP_ID "T48_APP_ID"      /* the id from the manifest */
#define T48_ENV_APP_DATA "T48_APP_DATA"  /* the app's own writable folder */
#define T48_ENV_FONTS "T48_FONTS_FD"     /* level 2: number of the font file's descriptor (struct t48_fonts_hdr) */
#define T48_ENV_FLIP "T48_FLIP_FD"       /* level 3: number of the flip buffers' descriptor (struct t48_msg_flipinfo) */
#define T48_ENV_SOUND "T48_SOUND_FD"     /* level 4: number of the sound ring's descriptor (struct t48_sound_ring);
                                          * only for an app whose manifest has the "audio-out" permission */

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
#define T48_MSG_THEME 9     /* level 2: struct t48_msg_theme; before START, and whenever the owner changes the look */
#define T48_MSG_FLIPINFO 10 /* level 3: struct t48_msg_flipinfo; before START */
#define T48_MSG_FLIPPED 11  /* level 3: struct t48_msg_flipped: the answer to FLIP */

/* app to shell */
#define T48_MSG_READY 0x81   /* struct t48_msg_ready; the answer to START */
#define T48_MSG_DAMAGE 0x82  /* struct t48_msg_rect: this part of the buffer changed */
#define T48_MSG_PAUSED 0x83  /* no payload: optional answer to PAUSE, "freeze me now" */
/* 0x84 is not used */
#define T48_MSG_FLIP 0x85    /* level 3: struct t48_msg_flip: show this flip buffer at the next refresh */

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

/* Level 3. An app that answered READY with 3 or more gets KEY and TOUCH in
 * these longer forms: the same start, then when it happened, in microseconds
 * of CLOCK_MONOTONIC. */
struct t48_msg_key3 {
    uint16_t code;
    uint8_t state;
    uint8_t pad[5];
    uint64_t time_us;
};

struct t48_msg_touch3 {
    uint8_t state;
    uint8_t pad;
    int16_t x, y;
    uint16_t pad2;
    uint64_t time_us;
};

/* Flipping (level 3). T48_ENV_FLIP names a descriptor holding `count` pictures
 * of the screen one after another, each `size` bytes, laid out as HELLO says
 * (width, height, stride, format). The app draws a whole picture into one and
 * sends FLIP; the shell shows it at the screen's next refresh, without
 * tearing, and answers FLIPPED. On the phone the screen is pointed at the
 * buffer itself: nothing is copied. count 0: this shell cannot flip. */
struct t48_msg_flipinfo {
    uint16_t count;     /* buffers */
    uint16_t pad;
    uint32_t size;      /* bytes from one buffer to the next */
    uint32_t period_us; /* one refresh of the screen, in microseconds */
};

struct t48_msg_flip {
    uint16_t buffer;    /* 0 to count - 1 */
    uint16_t pad;
};

struct t48_msg_flipped {
    uint16_t buffer;    /* the buffer of the FLIP this answers */
    uint16_t pad;
    uint32_t frame;     /* the screen's refreshes, counted: a step of more than one is a missed refresh */
    uint64_t time_us;   /* when it reached the screen: microseconds of CLOCK_MONOTONIC */
};

/* The look: colours by what they are for, shapes, and the font family. A later
 * level may add to the end: take the part you know. Colours are 0x00RRGGBB. */
#define T48_EDGE_NONE 0     /* a key or a plain button is a flat fill */
#define T48_EDGE_OUTLINE 1  /* it has a one-pixel outline */
#define T48_EDGE_LIP 2      /* it has a darker lip under it, like a key cap */
#define T48_ROWS_BLOCK 0    /* each row of a list is a box */
#define T48_ROWS_RULED 1    /* rows are parted by rules */
#define T48_ROWS_SOFT 2     /* each row is a slightly lighter box */

struct t48_msg_theme {
    char name[16];          /* "switchboard", "directory" or "twotone", NUL padded */
    char font[16];          /* the family to use from the font file: "b612", "plex", "figtree" */
    uint8_t radius;         /* corners of buttons and boxes, in pixels */
    uint8_t key_radius;     /* corners of a keypad key; 100 is a round key */
    uint8_t edge;           /* T48_EDGE_* */
    uint8_t rows;           /* T48_ROWS_* */
    uint8_t head_rule;      /* 1: a rule under the header */
    uint8_t dark;           /* 1: light ink on a dark ground */
    uint8_t pad[2];
    uint32_t bg, panel, line;                /* the ground, a quiet box on it, a rule */
    uint32_t text, dim, faint;               /* ink on the ground */
    uint32_t key, on_key, on_key_dim;        /* a key or plain button, and ink on it */
    uint32_t edge_colour;                    /* the outline, for T48_EDGE_OUTLINE */
    uint32_t accent, on_accent;              /* lit: switched on, or wants attention */
    uint32_t good, on_good, bad, on_bad;     /* go and stop */
    uint32_t sel, on_sel, on_sel_dim;        /* the selected row */
    uint32_t head, on_head, on_head_dim;     /* the header */
    uint32_t first, on_first;                /* the one button that matters most */
    uint32_t ground_ring, ground_call, ground_hold;   /* the shell's ground while a call rings, is up, is held */
};

/* The font file (T48_ENV_FONTS): map it read-only. This header, then `families`
 * of struct t48_fonts_family, then the tables they point at. Offsets are from
 * the start of the file. A face's glyphs are 95 of struct t48_fonts_glyph, for
 * ASCII 32 to 126; its coverage is one byte a pixel, 0 to 255, row by row. */
#define T48_FONTS_MAGIC "T48F"
enum { T48_FACE_TINY, T48_FACE_SMALL, T48_FACE_NORMAL, T48_FACE_BOLD, T48_FACE_MONO, T48_FACE_HEAD, T48_FACE_BIG,
       T48_FACE_HUGE, T48_FACES };   /* 12 fixed, 15, 19, 19 bold, 15 fixed, 26 bold, 40 bold, 104 bold (digits only) */

struct t48_fonts_hdr {
    char magic[4];
    uint16_t version;       /* 1 */
    uint16_t families;
};

struct t48_fonts_family {
    char name[16];
    struct {
        int16_t ascent, height;     /* pixels above the baseline; from one line to the next */
        uint32_t glyphs, coverage;  /* offsets */
    } face[T48_FACES];
};

struct t48_fonts_glyph {
    int16_t adv, w, h, xoff, yoff;  /* advance; the box of coverage and where it sits from the pen and the top */
    uint16_t pad;
    uint32_t off;                   /* into the face's coverage */
};

/* Sound (level 4). T48_ENV_SOUND names a descriptor to map shared and
 * writable: this header, then at `data` a ring of `frames` samples, signed
 * 16-bit, one channel, `rate` a second. The app is the only writer of the
 * samples and of `wr`; the audio service takes them 20 ms at a time and is the
 * only writer of the rest. Nothing is copied on the way and no message is
 * sent. `wr` and `rd` count samples and wrap at 2^32; sample k is at
 * data[k % frames]; wr - rd are waiting and must never pass `frames`.
 *
 * To wait for room: set `waiting` to 1 and sleep in futex(FUTEX_WAIT) on `rd`
 * with a timeout. The service wakes it after each take. While `state` is
 * T48_SOUND_OFF nothing is taken: throw the samples away at the rate they
 * would have played. */
#define T48_SOUND_MAGIC 0x53383454u   /* "T48S" */
#define T48_SOUND_RATE 16000
#define T48_SOUND_OFF 0       /* nobody is taking: the app is off the screen, or there is no audio service */
#define T48_SOUND_PLAYING 1   /* what is written is played */
#define T48_SOUND_HELD 2      /* taken at the same rate and not played: a call is ringing or up */

struct t48_sound_ring {
    uint32_t magic;       /* T48_SOUND_MAGIC */
    uint32_t rate;        /* samples a second: 16000 */
    uint32_t frames;      /* samples the ring holds: a power of two */
    uint32_t data;        /* where the samples start, in bytes from the start of the file */
    /* the audio service's */
    uint32_t rd;          /* samples taken */
    uint32_t state;       /* T48_SOUND_* */
    uint32_t delay;       /* samples between leaving the ring and reaching the loudspeaker */
    uint32_t dry;         /* times the ring ran empty while it was being played */
    /* the app's */
    uint32_t wr;          /* samples written */
    uint32_t waiting;     /* not 0: wake me when rd moves */
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
#define T48_KEY_SOFT1 0x110  /* level 3: the four soft keys under the screen of a phone that has them, left to right */
#define T48_KEY_SOFT2 0x111
#define T48_KEY_SOFT3 0x112
#define T48_KEY_SOFT4 0x113

#define T48_TOUCH_UP 0
#define T48_TOUCH_DOWN 1
#define T48_TOUCH_MOVE 2

#endif
