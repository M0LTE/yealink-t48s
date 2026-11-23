// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * libt48: the small library apps for the T48 firmware link statically.
 * Drawing into a 32-bit surface, built-in fonts, a small JSON reader, and the
 * app side of the app protocol (screen buffer, keys, touch, timers).
 *
 * A minimal app:
 *
 *     if (t48_app_init() < 0) return 1;
 *     t48_surface *s = t48_screen();
 *     t48_fill(s, 0, 0, s->w, s->h, T48_RGB(0, 0, 0));
 *     t48_text(s, 20, 20, &t48_font_big, T48_RGB(255, 255, 255), "Hello");
 *     t48_damage_all();
 *     t48_event ev;
 *     while (t48_poll(&ev, -1) >= 0)
 *         if (ev.type == T48_EV_QUIT) break;
 */
#ifndef T48_H
#define T48_H

#include <stddef.h>
#include <stdint.h>
#include "t48_proto.h"

/* ---------- surfaces and drawing ---------- */

typedef struct t48_surface {
    uint32_t *px;   /* 0x00RRGGBB */
    int w, h;
    int stride;     /* in pixels */
} t48_surface;

#define T48_RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

/* Everything clips to the surface. */
void t48_fill(t48_surface *s, int x, int y, int w, int h, uint32_t c);
/* a see-through fill: a is 0 (nothing) to 255 (solid) */
void t48_fill_alpha(t48_surface *s, int x, int y, int w, int h, uint32_t c, unsigned a);
/* a one-pixel outline */
void t48_rect(t48_surface *s, int x, int y, int w, int h, uint32_t c);
/* a filled box with rounded, smoothed corners of radius r */
void t48_round_fill(t48_surface *s, int x, int y, int w, int h, int r, uint32_t c);
/* copy sw x sh pixels (rows sstride pixels apart) to x, y */
void t48_blit(t48_surface *s, int x, int y, const uint32_t *src, int sw, int sh, int sstride);
/* mix colour c over d; a is 0 to 255 */
uint32_t t48_mix(uint32_t d, uint32_t c, unsigned a);
/* scale a colour towards black by num/den */
uint32_t t48_dim(uint32_t c, int num, int den);

/* ---------- text ---------- */

typedef struct t48_glyph { short adv, w, h, xoff, yoff; unsigned off; } t48_glyph;
typedef struct t48_font {
    short ascent, height;
    const t48_glyph *g;        /* ASCII 32 to 126 */
    const unsigned char *a;    /* 8-bit coverage */
} t48_font;

extern const t48_font t48_font_tiny;    /* 12 px fixed width */
extern const t48_font t48_font_small;   /* 15 px */
extern const t48_font t48_font_normal;  /* 19 px */
extern const t48_font t48_font_bold;    /* 19 px bold */
extern const t48_font t48_font_mono;    /* 15 px fixed width */
extern const t48_font t48_font_head;    /* 26 px bold */
extern const t48_font t48_font_big;     /* 40 px bold */
extern const t48_font t48_font_huge;    /* 104 px bold; digits, colon, dot, minus and space only */

/* Text with its top at y. Returns the x after the last character. ASCII only;
 * anything else is drawn as '?'. */
int t48_text(t48_surface *s, int x, int y, const t48_font *f, uint32_t c, const char *str);
int t48_text_width(const t48_font *f, const char *str);
/* cut to maxw pixels, ending in ".." if cut */
void t48_text_fit(t48_surface *s, int x, int y, int maxw, const t48_font *f, uint32_t c, const char *str);
void t48_text_centre(t48_surface *s, int cx, int y, const t48_font *f, uint32_t c, const char *str);
void t48_text_right(t48_surface *s, int rx, int y, const t48_font *f, uint32_t c, const char *str);

/* ---------- images ---------- */

/* Load a binary PPM (P6, 8 bits per channel). 0, or -1 if it cannot be read.
 * The surface's pixels are malloc'd: free(img->px) when done. */
int t48_load_ppm(const char *path, t48_surface *img);
/* Write a surface to a file: PNG if the name ends in .png, otherwise PPM. */
int t48_save(const t48_surface *s, const char *path);

/* ---------- a small JSON reader ---------- */

/* Works on text in memory from p to e without copying it. Values are returned
 * as pointers into the text. The text must be NUL terminated at or after e. */
const char *t48_json_get(const char *obj, const char *e, const char *key);  /* member of an object, or NULL */
int t48_json_num(const char *v, const char *e, double *out);               /* 1 if v is a number */
int t48_json_str(const char *v, const char *e, char *out, size_t n);       /* 1 if v is a string */
int t48_json_true(const char *v, const char *e);
const char *t48_json_first(const char *arr, const char *e);                /* first element, or NULL */
const char *t48_json_next(const char *v, const char *e);                   /* the one after v, or NULL */

/* ---------- the app side of the protocol ---------- */

enum {
    T48_EV_NONE = 0,   /* t48_poll timed out */
    T48_EV_KEY,        /* key: T48_KEY_*, state: 1 pressed, 0 released */
    T48_EV_TOUCH,      /* state: T48_TOUCH_*, x, y in pixels */
    T48_EV_PAUSE,      /* off the screen: stop drawing, close network connections */
    T48_EV_RESUME,     /* in front again: redraw everything */
    T48_EV_QUIT,       /* leave now */
    T48_EV_TIMER,      /* id: the timer that fired */
    T48_EV_FD,         /* id: a watched descriptor is readable */
};

typedef struct t48_event {
    int type;
    int key, state;
    int x, y;
    int id;
} t48_event;

/* Connect to the shell that started us and map the screen. 0, or -1 (not
 * started by a shell, or the shell speaks a protocol we cannot). */
int t48_app_init(void);
/* The screen. Draw into it, then say what changed. */
t48_surface *t48_screen(void);
void t48_damage(int x, int y, int w, int h);
void t48_damage_all(void);
/* Wait up to timeout_ms (-1: for ever) for the next event. Returns 1 with an
 * event, 0 on a timeout, -1 when the shell has gone (the app should exit). */
int t48_poll(t48_event *ev, int timeout_ms);
/* Timers, numbered 0 to 7. period_ms 0 stops one. A repeating timer that falls
 * behind skips the fires it missed. */
void t48_timer(int id, int period_ms, int repeat);
/* Have t48_poll report when fd is readable (up to 8; -1 on too many). */
int t48_watch_fd(int fd, int id);
void t48_unwatch_fd(int fd);
/* A setting from the app's manifest as the owner has set it, or def. */
const char *t48_setting(const char *key, const char *def);
/* After T48_EV_PAUSE: tell the shell we are ready to be frozen (optional). */
void t48_paused(void);
/* The socket itself, for apps with their own poll loop. */
int t48_fd(void);
/* Milliseconds from a clock that never jumps. */
long long t48_now_ms(void);

#endif
