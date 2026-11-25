// SPDX-License-Identifier: AGPL-3.0-or-later
/* tones: a check of sound from apps (API level 4), and the smallest program
 * that makes any. Four rising notes when it starts, and again on OK.
 *
 *   OK      the four notes
 *   1       one short beep
 *   2       a steady tone, until 2 is pressed again
 *   X       close
 *
 * The volume keys set the level while it is in front. Once a second while
 * anything plays, when it has been played out and when the app closes, the
 * figures go to standard error, which on the phone is the shell's log. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "t48.h"

#define RATE 16000
#define CHUNK 160           /* 10 ms at a time, whenever there is room */

/* One note: a sine read out of a table by a counter that wraps once a cycle,
 * with 8 ms fades so it does not click. No floating point once the table is
 * made, and the pitch is exact however long it plays. */
#define FADE (RATE * 8 / 1000)
#define PEAK 24000          /* 3 dB under full scale */
static int16_t sine[1024];
static struct {
    int pos, total;         /* samples played, and its whole length */
    uint32_t phase, step;   /* a cycle is 2^32 */
} note;

static const int tune[] = {523, 659, 784, 1047};   /* C E G C */
static int tune_at = -1;    /* the note of the tune now playing, -1: none */
static int steady;          /* key 2: a tone that goes on */
static int have_sound;
static long written;
static long long last_log;
static int busy;            /* something was started and has not been heard out */

/* How long what is written waits before the audio service takes it: the end
 * of each piece written and when, checked off as the queue goes down. */
#define NMARK 64
static struct { long end; long long at; } mark[NMARK];
static int mark_n;
static long wait_n, wait_sum_us, wait_max_us;

static void marks_check(void)
{
    long taken = written - t48_sound_queued();
    long long now = t48_now_us();
    int k = 0;
    while (k < mark_n && mark[k].end <= taken) {
        long us = (long)(now - mark[k].at);
        wait_n++;
        wait_sum_us += us;
        if (us > wait_max_us) wait_max_us = us;
        k++;
    }
    if (k) memmove(mark, mark + k, (size_t)(mark_n - k) * sizeof mark[0]);
    mark_n -= k;
}

static int sounding(void) { return note.pos < note.total; }

static void start_note(int hz, int ms)
{
    note.total = RATE * ms / 1000;
    note.pos = 0;
    note.phase = 0;
    note.step = (uint32_t)(((uint64_t)hz << 32) / RATE);
}

static void stop_note(void)
{
    if (note.total - note.pos > FADE) note.pos = note.total - FADE;
}

static int make(int16_t *out, int n)
{
    int i = 0;
    for (; i < n; i++, note.pos++) {
        if (note.pos >= note.total) {
            if (!steady) break;
            note.pos = FADE;   /* round again, past the fade in */
        }
        int left = note.total - note.pos;
        int g = note.pos < FADE ? note.pos : !steady && left < FADE ? left : FADE;
        out[i] = (int16_t)(sine[note.phase >> 22] * g / FADE);
        note.phase += note.step;
    }
    return i;
}

static void feed(void)
{
    int16_t buf[CHUNK];
    while (have_sound && t48_sound_free() >= CHUNK) {
        if (!sounding() && !steady) {
            if (tune_at < 0 || ++tune_at >= (int)(sizeof tune / sizeof tune[0])) { tune_at = -1; break; }
            start_note(tune[tune_at], 180);
        }
        int n = make(buf, CHUNK);
        written += t48_sound_put(buf, n);
        if (mark_n < NMARK) { mark[mark_n].end = written; mark[mark_n++].at = t48_now_us(); }
    }
}

static void say(const char *what)
{
    fprintf(stderr, "tones: %s: %ld samples written, %d waiting, latency %d ms, %s, dry %u; waited %ld ms on average, %ld at most\n",
            what, written, t48_sound_queued(), t48_sound_latency_us() / 1000, t48_sound_playing() ? "playing" : "not playing",
            t48_sound_dry(), wait_n ? wait_sum_us / wait_n / 1000 : 0, wait_max_us / 1000);
}

static const char *state_text(void)
{
    return steady ? "Steady tone" : sounding() || tune_at >= 0 ? "Playing" : "Silent";
}

static const char *shown;   /* the state on the screen */

static void draw(void)
{
    t48_surface *s = t48_screen();
    const t48_theme *t = t48_theme_now();
    t48_fill(s, 0, 0, s->w, s->h, t->bg);
    t48_text(s, 40, 40, &t48_font_head, t->text, "Tones");
    if (!have_sound) {
        t48_text(s, 40, 110, &t48_font_normal, t->text, "No sound");
    } else {
        t48_text(s, 40, 110, &t48_font_normal, t->text, "OK: four notes     1: beep     2: steady tone     X: close");
        t48_text(s, 40, 160, &t48_font_normal, t->dim, shown = state_text());
    }
    t48_damage_all();
}

int main(void)
{
    t48_event ev;
    int paused = 0;

    if (t48_app_init() < 0) return 1;
    for (int i = 0; i < 1024; i++) sine[i] = (int16_t)lrint(PEAK * sin(2 * M_PI * i / 1024));
    have_sound = t48_sound_open() == 0;
    if (!have_sound) fprintf(stderr, "tones: no sound from this shell\n");
    t48_sound_buffer(640);   /* 40 ms waiting at most */
    tune_at = 0;
    start_note(tune[0], 180);
    feed();
    draw();
    t48_timer(0, 10, 1);
    t48_timer(1, 250, 1);
    for (;;) {
        if (t48_poll(&ev, -1) < 0) break;
        if (ev.type == T48_EV_QUIT) break;
        if (ev.type == T48_EV_PAUSE) { paused = 1; steady = 0; note.pos = note.total; tune_at = -1; t48_paused(); }
        if (ev.type == T48_EV_RESUME || ev.type == T48_EV_THEME) { paused = 0; draw(); }
        if (ev.type == T48_EV_KEY && ev.state) {
            if (ev.key == T48_KEY_BACK) break;
            if (ev.key == T48_KEY_OK) { steady = 0; tune_at = 0; start_note(tune[0], 180); }
            if (ev.key == '1') { steady = 0; tune_at = -1; start_note(880, 80); }
            if (ev.key == '2') { steady = !steady; tune_at = -1; if (steady) start_note(600, 1000); else stop_note(); }
            feed();
            draw();
        }
        if (ev.type == T48_EV_TIMER && ev.id == 0 && !paused) {
            marks_check();
            feed();
            if (sounding() || tune_at >= 0) busy = 1;
            else if (busy && !t48_sound_queued()) { busy = 0; say("played"); }
        }
        if (ev.type == T48_EV_TIMER && ev.id == 1 && !paused) {
            if (have_sound && shown != state_text()) draw();   /* not every time: drawing the screen is the costly part */
            if ((sounding() || tune_at >= 0) && t48_now_ms() - last_log >= 1000) { last_log = t48_now_ms(); say("playing"); }
        }
    }
    say("closing");
    return 0;
}
