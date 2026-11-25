// SPDX-License-Identifier: AGPL-3.0-or-later
/* fliptest: a check of flipping (API level 3), and the smallest program that
 * uses it. A bar crosses the screen once a second. If the screen tears, the
 * bar is broken into pieces that do not line up; flipped, it never is. The top
 * line counts frames a second and refreshes missed; the second line shows each
 * key and touch with how long it took to get here.
 *
 *   1  flip, redrawing only the bar
 *   2  flip, redrawing every pixel
 *   3  flip, a 400 x 240 picture drawn at twice its size
 *   4  no flip: the shared buffer and DAMAGE, which can tear
 *   X  close
 *
 * Once a second the same figures go to standard error, which on the phone is
 * the shell's log. */
#include <stdio.h>
#include <string.h>
#include "t48.h"

#define HEAD_H 64       /* the two lines of text */
#define BAR_W 24
#define BAR_STEP 12
#define SMALL_W 400
#define SMALL_H 240

enum { FLIP_BAR = 1, FLIP_FULL, FLIP_SMALL, COPY };
static const char *const mode_name[] = {"", "Flip", "Flip, every pixel", "Flip, 400 x 240 doubled", "No flip"};

static int mode = FLIP_BAR, can_flip, paused;
static t48_surface *s;                 /* where the next picture is drawn */
static long tick;                      /* pictures drawn */
static char stats[96] = "", input[96] = "1 2 3 4: change the test     X: close";
static uint32_t small[SMALL_W * SMALL_H];

/* which bar is in each buffer, so that only it need be rubbed out */
static struct { const uint32_t *px; int bar_x, whole; } seen[9];

/* a second's worth of figures */
static long long sec_start, draw_us;
static int flips, missed;
static unsigned last_frame;
static int have_frame;

static int *slot(const t48_surface *b, int **whole)
{
    int i = 0;
    while (i < 8 && seen[i].px && seen[i].px != b->px) i++;
    if (!seen[i].px) { seen[i].px = b->px; seen[i].whole = 1; }
    *whole = &seen[i].whole;
    return &seen[i].bar_x;
}

static void redraw_everything(void)
{
    for (int i = 0; i < 9; i++) seen[i].whole = 1;
}

static void draw_head(t48_surface *b)
{
    char line[160];
    t48_fill(b, 0, 0, b->w, HEAD_H, T48_RGB(0, 0, 0));
    snprintf(line, sizeof line, "%s   %s", mode_name[mode], stats);
    t48_text(b, 12, 6, &t48_font_bold, T48_RGB(255, 255, 255), line);
    t48_text(b, 12, 34, &t48_font_small, T48_RGB(160, 160, 160), input);
}

static void draw(t48_surface *b)
{
    int *whole, *bar_x = slot(b, &whole), x = (int)(tick * BAR_STEP % b->w);

    if (mode == FLIP_FULL) {
        int v = (int)(tick % 120);
        t48_fill(b, 0, HEAD_H, b->w, b->h - HEAD_H, T48_RGB(0, v < 60 ? v : 120 - v, 40));
        t48_fill(b, x, HEAD_H, BAR_W, b->h - HEAD_H, T48_RGB(255, 255, 255));
        *whole = 1;
    } else if (mode == FLIP_SMALL) {
        int t = (int)tick, bx = (int)(tick * (BAR_STEP / 2) % SMALL_W);
        for (int y = 0; y < SMALL_H; y++) {
            uint32_t *p = small + y * SMALL_W;
            for (int i = 0; i < SMALL_W; i++) {
                unsigned v = (unsigned)((i + t) ^ (y + (t >> 1))) & 255;
                p[i] = v << 16 | (v >> 1) << 8 | (255 - v);
            }
            for (int i = bx; i < bx + BAR_W / 2 && i < SMALL_W; i++) p[i] = 0xffffff;
        }
        t48_blit_scaled(b, 0, 0, small, SMALL_W, SMALL_H, SMALL_W, 2);
        *whole = 1;
    } else {
        if (*whole) t48_fill(b, 0, HEAD_H, b->w, b->h - HEAD_H, T48_RGB(0, 0, 0));
        else t48_fill(b, *bar_x, HEAD_H, BAR_W, b->h - HEAD_H, T48_RGB(0, 0, 0));
        t48_fill(b, x, HEAD_H, BAR_W, b->h - HEAD_H, T48_RGB(255, 255, 255));
        if (mode == COPY) {
            if (*whole) t48_damage_all();
            else { t48_damage(*bar_x, HEAD_H, BAR_W, b->h - HEAD_H); t48_damage(x, HEAD_H, BAR_W, b->h - HEAD_H); }
        }
        *whole = 0;
        *bar_x = x;
    }
    draw_head(b);
    if (mode == COPY) t48_damage(0, 0, b->w, HEAD_H);
    tick++;
}

/* count a picture; once a second, say how it went */
static void counted(long long now, unsigned frame, int framed)
{
    if (framed && have_frame && frame - last_frame > 1) missed += (int)(frame - last_frame - 1);
    if (framed) { last_frame = frame; have_frame = 1; }
    flips++;
    if (!sec_start) sec_start = now;
    if (now - sec_start < 1000000) return;
    double fps = flips * 1e6 / (double)(now - sec_start);
    snprintf(stats, sizeof stats, "%.1f a second   %d missed   %.1f ms to draw", fps, missed, (double)draw_us / flips / 1000);
    fprintf(stderr, "fliptest: %s: %s\n", mode_name[mode], stats);
    sec_start = now;
    flips = missed = 0;
    draw_us = 0;
}

static void set_mode(int m)
{
    if (m == mode || (m != COPY && !can_flip)) return;
    mode = m;
    if (mode == COPY) { t48_flip_stop(); s = t48_screen(); }
    else s = t48_flip_start();
    redraw_everything();
    sec_start = 0;
    flips = missed = have_frame = 0;
    draw_us = 0;
    stats[0] = 0;
}

static const char *key_name(int key)
{
    static char b[8];
    switch (key) {
    case T48_KEY_UP: return "Up";
    case T48_KEY_DOWN: return "Down";
    case T48_KEY_LEFT: return "Left";
    case T48_KEY_RIGHT: return "Right";
    case T48_KEY_OK: return "OK";
    case T48_KEY_SOFT1: return "Soft key 1";
    case T48_KEY_SOFT2: return "Soft key 2";
    case T48_KEY_SOFT3: return "Soft key 3";
    case T48_KEY_SOFT4: return "Soft key 4";
    }
    snprintf(b, sizeof b, "%c", key);
    return b;
}

/* 0: go on, 1: leave */
static int handle(const t48_event *ev)
{
    double late = ev->time_us ? (double)(t48_now_us() - ev->time_us) / 1000 : 0;
    switch (ev->type) {
    case T48_EV_FLIPPED:
        if (mode != COPY) counted(ev->time_us, ev->frame, 1);
        break;
    case T48_EV_KEY:
        if (ev->key == T48_KEY_BACK) return 1;
        snprintf(input, sizeof input, "%s %s   %.1f ms ago", key_name(ev->key), ev->state ? "down" : "up", late);
        fprintf(stderr, "fliptest: %s\n", input);
        if (ev->state && ev->key >= '1' && ev->key <= '4') set_mode(ev->key - '0');
        break;
    case T48_EV_TOUCH:
        snprintf(input, sizeof input, "Touch %s at %d, %d   %.1f ms ago",
                 ev->state == T48_TOUCH_DOWN ? "down" : ev->state == T48_TOUCH_UP ? "up" : "moved", ev->x, ev->y, late);
        break;
    case T48_EV_PAUSE:
        paused = 1;
        t48_paused();
        break;
    case T48_EV_RESUME:
        paused = 0;
        redraw_everything();
        sec_start = 0;
        flips = missed = have_frame = 0;
        break;
    case T48_EV_QUIT:
        return 1;
    }
    return 0;
}

int main(void)
{
    t48_event ev;
    long long next = 0;
    int r;

    if (t48_app_init() < 0) return 1;
    s = t48_flip_start();
    can_flip = s != NULL;
    if (!can_flip) {
        mode = COPY;
        s = t48_screen();
        snprintf(input, sizeof input, "This firmware cannot flip     X: close");
    }
    for (;;) {
        /* everything that has arrived; then, off the screen, wait for more */
        while ((r = t48_poll(&ev, paused ? -1 : 0)) > 0)
            if (handle(&ev)) return 0;
        if (r < 0) return 0;
        if (paused) continue;

        long long t0 = t48_now_us();
        draw(s);
        draw_us += t48_now_us() - t0;
        if (mode != COPY) {
            s = t48_flip();   /* keeps time with the screen */
        } else {
            /* nothing to keep time with: sixty a second by the clock */
            long long now = t48_now_us();
            counted(now, 0, 0);
            if (next < now) next = now;
            next += 16667;
            while ((now = t48_now_us()) < next) {
                r = t48_poll(&ev, (int)((next - now) / 1000));
                if (r < 0 || (r > 0 && handle(&ev))) return 0;
                if (mode != COPY || paused) break;
            }
        }
    }
}
