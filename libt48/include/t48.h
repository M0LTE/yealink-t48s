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
 *
 * A game draws whole pictures and flips them instead: see t48_flip() below.
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
/* the same with every pixel drawn as n x n (n 1 to 4): a small picture made to fill the screen */
void t48_blit_scaled(t48_surface *s, int x, int y, const uint32_t *src, int sw, int sh, int sstride, int n);
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

/* The faces in use. They follow the theme (below), so take a face's height
 * when drawing, not once at start. */
extern t48_font t48_font_tiny;    /* 12 px fixed width */
extern t48_font t48_font_small;   /* 15 px */
extern t48_font t48_font_normal;  /* 19 px */
extern t48_font t48_font_bold;    /* 19 px bold */
extern t48_font t48_font_mono;    /* 15 px fixed width */
extern t48_font t48_font_head;    /* 26 px bold */
extern t48_font t48_font_big;     /* 40 px bold */
extern t48_font t48_font_huge;    /* 104 px bold; digits, colon, dot, minus and space only */

/* A family: the eight faces, in the order of T48_FACE_*. B612 is in every
 * program. The shell has the others and hands them to apps in the font file
 * (docs/app-protocol.md), so an app does not carry them. */
typedef struct t48_font_family { char name[16]; t48_font face[T48_FACES]; } t48_font_family;
extern const t48_font_family t48_fonts_b612, t48_fonts_plex, t48_fonts_figtree;
void t48_fonts_use(const t48_font_family *f);

/* Text with its top at y. Returns the x after the last character. ASCII only;
 * anything else is drawn as '?'. */
int t48_text(t48_surface *s, int x, int y, const t48_font *f, uint32_t c, const char *str);
int t48_text_width(const t48_font *f, const char *str);
/* cut to maxw pixels, ending in ".." if cut */
void t48_text_fit(t48_surface *s, int x, int y, int maxw, const t48_font *f, uint32_t c, const char *str);
void t48_text_centre(t48_surface *s, int cx, int y, const t48_font *f, uint32_t c, const char *str);
void t48_text_right(t48_surface *s, int rx, int y, const t48_font *f, uint32_t c, const char *str);

/* ---------- the theme ---------- */

/* The look the owner chose (Settings, Screen, Theme): colours by what they are
 * for, shapes, and the font family. An app started by the shell is given it,
 * and again when it changes (T48_EV_THEME: draw everything again). The
 * keyboard, the parts below and the fonts follow it by themselves; an app that
 * takes its own colours from t48_theme_now() matches too. Until the shell
 * says otherwise it is Switchboard. */
typedef struct t48_msg_theme t48_theme;
const t48_theme *t48_theme_now(void);
extern const t48_theme t48_themes[];          /* every theme there is */
extern const int t48_theme_count;
const t48_theme *t48_theme_named(const char *name);   /* NULL: no such theme */
/* Make this the theme: the colours, the keyboard and, if fonts is not NULL, the faces. */
void t48_theme_use(const t48_theme *t, const t48_font_family *fonts);

/* ---------- parts in the theme's look ---------- */

/* What the shell's own screens are made of, for an app that wants to look like
 * them. Each draws on s in the current theme. */
#define T48_UI_HEAD_H 48                      /* the header */
#define T48_UI_TAG_H 34
enum { T48_UI_PLAIN, T48_UI_FIRST, T48_UI_GO, T48_UI_STOP, T48_UI_LIT, T48_UI_OFF };   /* what a button is for */
typedef struct t48_ui_ink { uint32_t text, dim, faint; } t48_ui_ink;

/* fill the surface; later parts mix with this ground where the theme asks */
void t48_ui_clear(t48_surface *s, uint32_t ground);
uint32_t t48_ui_ground(void);
void t48_ui_ground_is(uint32_t ground);       /* the same without filling: what is under the parts drawn next */
/* the header: the screen's name, the time, a way back (a touch left of x 130)
 * or a lamp (T48_UI_GO, T48_UI_LIT, T48_UI_STOP; 0: none) */
void t48_ui_header(t48_surface *s, const char *title, int back, int lamp);
void t48_ui_header_ground(t48_surface *s);
/* a button. The labels "<x", "+", "-", "^" and "v" are drawn as marks */
void t48_ui_btn(t48_surface *s, int x, int y, int w, int h, const char *label, int role);
void t48_ui_key(t48_surface *s, int x, int y, int w, int h, const char *label);   /* a key of a number pad */
void t48_ui_panel(t48_surface *s, int x, int y, int w, int h);                    /* a quiet box that holds something */
/* the ground of one row of a list, and the inks to write on it */
void t48_ui_row(t48_surface *s, int x, int y, int w, int h, int selected, t48_ui_ink *ink);
int t48_ui_tag(t48_surface *s, int x, int y, const char *text, int role);         /* a short word in a box; the x after it */
int t48_ui_tag_width(const char *text);
void t48_ui_switch(t48_surface *s, int rx, int y, int h, int on, int selected);   /* On or Off, its right edge at rx */
void t48_ui_level(t48_surface *s, int x, int y, int w, int h, int level, int max, int disabled, int selected);
void t48_ui_lamp(t48_surface *s, int x, int y, uint32_t colour);
/* dir: -1 points left, 1 right, -2 up, 2 down; size is half its length, t its weight */
void t48_ui_chevron(t48_surface *s, int cx, int cy, int size, int dir, int t, uint32_t c);
/* a box over the dimmed screen (tone: T48_UI_PLAIN, T48_UI_LIT, T48_UI_STOP), the
 * same with the screen left showing, and the inks and buttons to use inside */
void t48_ui_dialog(t48_surface *s, int x, int y, int w, int h, int tone);
void t48_ui_popup(t48_surface *s, int x, int y, int w, int h, int tone);
void t48_ui_dialog_ink(t48_ui_ink *ink);
void t48_ui_dialog_btn(t48_surface *s, int x, int y, int w, int h, const char *label, int role);
int t48_ui_notice(t48_surface *s, int x, int y, const char *text, int role);      /* a wide tag; the x after it */
int t48_ui_notice_width(const char *text);
void t48_ui_tile(t48_surface *s, int x, int y, int w, int h, int selected, t48_ui_ink *ink);
void t48_ui_icon(t48_surface *s, int x, int y, int size, const char *letter);     /* a square with a letter, or "+" */

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

/* ---------- a WebSocket client (plain ws:// for now) ---------- */

typedef struct t48_ws t48_ws;
enum { T48_WS_TEXT = 1, T48_WS_BINARY = 2 };
/* A whole message. Text is NUL terminated (data[len] is 0); data is valid
 * only during the call, and the callback may change it in place. */
typedef void (*t48_ws_cb)(void *ctx, int type, unsigned char *data, size_t len);

/* Connect to ws://host[:port][/path] and upgrade. Waits up to timeout_ms
 * (a name lookup may take longer; addresses do not). Messages bigger than
 * max_msg are dropped. NULL on failure, with a short reason in err. Call
 * t48_ws_read once straight away: frames may have come with the answer. */
t48_ws *t48_ws_open(const char *url, int timeout_ms, size_t max_msg, char *err, size_t errn);
/* for t48_watch_fd */
int t48_ws_fd(const t48_ws *ws);
/* Take what has arrived and call cb for each whole message; pings are
 * answered here. 0, or -1 when the connection has ended (then t48_ws_close). */
int t48_ws_read(t48_ws *ws, t48_ws_cb cb, void *ctx);
int t48_ws_send_text(t48_ws *ws, const char *text, size_t len);
void t48_ws_close(t48_ws *ws);
/* split a ws:// address: 1 if it is one */
int t48_ws_parse_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn);

/* ---------- an on-screen keyboard ---------- */

/* A full-screen text editor: touch keys, and multi-tap on the keypad (2 is
 * a b c 2; # steps abc, ABC, 123; X rubs out; OK is Done). The caller keeps
 * its own event loop: open it, draw it, pass keys and touches in, and draw
 * again whenever one of the calls returns T48_KBD_REDRAW. */
enum { T48_KBD_PASSWORD = 1,   /* never drawn in full: stars, but for the character just typed */
       T48_KBD_DIGITS = 2 };   /* start on the numbers, and the keypad types digits */
enum { T48_KBD_NOTHING = 0, T48_KBD_REDRAW, T48_KBD_DONE, T48_KBD_CANCEL };

typedef struct t48_kbd {
    char title[96];
    char text[200];
    int len, cur, max, flags;
    int page, shift, mode;           /* touch page; shift 0, once, locked; keypad abc, ABC, 123 */
    int tap_key, tap_n;              /* multi-tap in progress */
    long long tap_until, reveal_until, flash_until;
    int flash;
    int w, h;                        /* the surface last drawn on */
} t48_kbd;

/* The keyboard's colours and shapes. They follow the theme; an app may set its own after. */
typedef struct t48_kbd_look {
    uint32_t bg, field, field_text, label;   /* the ground, the box typed into and its ink, the title */
    uint32_t key, side, text, dim;           /* a letter key, a key that is not a letter, their ink, "Space" */
    uint32_t accent, on_accent, cursor;      /* Done and Caps; the cursor */
    uint32_t edge;                           /* the colour of the outline or the lip */
    int edge_kind;                           /* 0 none, 1 a one-pixel outline, 2 a lip under the key */
    int radius;
} t48_kbd_look;
extern t48_kbd_look t48_kbd_style;

/* max is the longest text allowed (up to 199) */
void t48_kbd_open(t48_kbd *k, const char *title, const char *text, int max, int flags);
void t48_kbd_draw(t48_kbd *k, t48_surface *s);          /* fills the whole surface */
int t48_kbd_key(t48_kbd *k, int code, int down);        /* T48_KEY_* */
int t48_kbd_touch(t48_kbd *k, int state, int x, int y);
int t48_kbd_timeout(const t48_kbd *k);                  /* ms until t48_kbd_tick is wanted, -1: never */
int t48_kbd_tick(t48_kbd *k);
const char *t48_kbd_text(const t48_kbd *k);

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
    T48_EV_THEME,      /* the owner changed the look (t48_theme_now): draw everything again */
    T48_EV_FLIPPED,    /* a t48_flip() is on the screen. frame: the screen's refreshes, counted; time_us: when */
};

typedef struct t48_event {
    int type;
    int key, state;
    int x, y;
    int id;
    long long time_us;   /* KEY, TOUCH, FLIPPED: when, on the clock of t48_now_us(); 0 from a shell older than level 3 */
    unsigned frame;      /* FLIPPED */
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
long long t48_now_us(void);   /* the same clock in microseconds: the one events are timed on */

/* ---------- flipping: whole pictures, shown without tearing ---------- */

/* For games and anything else that draws the whole screen many times a
 * second. The shell lends pictures of the screen ("flip buffers"); draw all
 * of one, call t48_flip(), and it goes on the screen whole at the next
 * refresh. On the phone the screen is pointed at the buffer: no copy.
 *
 *     t48_surface *s = t48_flip_start();
 *     if (!s) ...                          the shell is older than level 3: use t48_screen() and t48_damage()
 *     for (;;) {
 *         while (t48_poll(&ev, 0) > 0) ... keys, touches, PAUSE, QUIT
 *         draw all of s
 *         s = t48_flip();                  waits if the last one is not on the screen yet
 *     }
 *
 * t48_flip() keeps time with the screen, so the loop above runs at the
 * refresh rate or a whole fraction of it. The surface it returns holds an
 * older picture, not the last: draw everything, or keep track. Each flip also
 * comes back as T48_EV_FLIPPED. After T48_EV_PAUSE stop flipping and wait in
 * t48_poll() for T48_EV_RESUME; the shell puts the last picture back itself. */
t48_surface *t48_flip_start(void);    /* the first buffer to draw into; NULL: this shell cannot flip */
t48_surface *t48_flip(void);          /* show what was drawn; the buffer to draw next */
void t48_flip_wait(void);             /* until the last t48_flip() is on the screen */
/* Back to t48_screen() and t48_damage(): the next t48_damage() takes the screen,
 * and the next t48_flip() takes it back. */
void t48_flip_stop(void);
int t48_flip_period_us(void);         /* one refresh of the screen, in microseconds (14746 on the T48S); 0: cannot flip */

/* ---------- sound (level 4) ----------
 *
 * For an app whose manifest has the "audio-out" permission. One format: 16000
 * samples a second, one channel, signed 16-bit (t48_resample turns other
 * sound into it). Samples go straight into memory shared with the audio
 * service; nothing is sent.
 *
 *     if (t48_sound_open() == 0) t48_sound_write(samples, n);
 *
 * It comes from the loudspeaker at the owner's Apps volume (the volume keys,
 * while the app is in front). A call silences it and it comes back after.
 * Off the screen the app is silent. In both cases, and in the simulator,
 * writing goes on working at the same pace, into nothing: an app need not
 * know. An app that stops writing is silence. */
int t48_sound_open(void);                         /* 0, or -1: no sound for this app (no permission, or an older shell) */
void t48_sound_close(void);
/* Write n samples, waiting for room. Returns n, or -1 if sound is not open. */
int t48_sound_write(const int16_t *samples, int n);
/* The same without waiting: as many as there is room for now. Returns how many. */
int t48_sound_put(const int16_t *samples, int n);
int t48_sound_free(void);                         /* how many t48_sound_put would take now */
int t48_sound_queued(void);                       /* samples written and not yet taken */
/* How many samples may wait (default 1024, 64 ms; 320 to 4096). Fewer is less
 * delay; too few and a late app leaves a gap. The service takes 320 at a time. */
void t48_sound_buffer(int samples);
/* From a sample written now to the loudspeaker, in microseconds, to within 10 ms. */
int t48_sound_latency_us(void);
int t48_sound_playing(void);                      /* 1: heard now. 0: held by a call, off the screen, or no audio service */
unsigned t48_sound_dry(void);                     /* times the queue ran empty while playing: a count of gaps, and of sounds that ended */
int t48_sound_rate(void);                         /* 16000 */

/* ---------- lamps (level 5) ----------
 *
 * For an app whose manifest has the "lamps" permission, while it is in front:
 * T48_LAMP_MESSAGE (red, top right of the case), and the lamps in three keys,
 * top to bottom T48_LAMP_HEADSET (green), T48_LAMP_MUTE (red) and
 * T48_LAMP_SPEAKER (green). Levels are 0 to 255 and look evenly spaced. The
 * message lamp is on or off (on from 128) and slow: use it for the odd flash.
 * A mask is 1 << lamp, or several of them.
 *
 *     t48_lamp_pulse(1 << T48_LAMP_MUTE, 255, 200);       a flash that dies away in 200 ms
 *
 * Two ways to move them. Set levels yourself, as often as you draw; or hand
 * the shell a pattern of fades and it runs it without another message. A lamp
 * you have set is yours until you release it. T48_EV_PAUSE takes them all
 * back (a call, Home): set them again after T48_EV_RESUME. On an older shell,
 * or without the permission, these do nothing. */
int t48_lamps_ok(void);                           /* 1: this shell lends lamps to apps */
void t48_lamp(int lamp, int level);               /* one lamp, now */
void t48_lamps_set(unsigned mask, const uint8_t *level);   /* several at once: level[T48_LAMPS], those in the mask */
/* From where each lamp is now, a straight fade to each key's level over its
 * ms, in turn (ms 0 jumps); then it stays, or with loop goes round again. Up
 * to T48_LAMP_KEYS keys. A new pattern or level replaces the one running. */
void t48_lamp_pattern(unsigned mask, const struct t48_lamp_key *key, int n, int loop);
void t48_lamp_fade(unsigned mask, int level, int ms);    /* to that level over ms */
void t48_lamp_pulse(unsigned mask, int level, int ms);   /* to that level at once, then down to dark over ms */
void t48_lamps_release(unsigned mask);            /* back to the phone: mute, message waiting and so on */

/* Other rates and stereo, in whole numbers and cheaply: 44100 or 48000 stereo
 * to the phone's format costs under 1% of its processor. Down, each output
 * sample is the mean of the input it covers; up, a straight line. Good for
 * games and speech, not for hi-fi. Feed it in pieces of any size. */
typedef struct t48_resampler {
    int rate, channels;
    int32_t step, need, inv, acc, last, p1, p2;
} t48_resampler;
void t48_resample_init(t48_resampler *r, int rate, int channels);   /* 4000 to 96000; 1 or 2 channels, interleaved */
/* in_frames of input (a frame is one sample for each channel) to at most cap samples of output. Returns how many. */
int t48_resample(t48_resampler *r, const int16_t *in, int in_frames, int16_t *out, int cap);
int t48_resample_max(const t48_resampler *r, int in_frames);   /* the most that many frames can give */

#endif
