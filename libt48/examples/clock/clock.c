// SPDX-License-Identifier: AGPL-3.0-or-later
/* clock: the smallest useful app for the T48 firmware, and the proof that the
 * app protocol works. A clock with seconds, the last key or touch it was
 * given, and a dot under the finger. A short press on X leaves. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "t48.h"

#define C_BG T48_RGB(10, 14, 26)
#define C_TEXT T48_RGB(232, 238, 246)
#define C_DIM T48_RGB(130, 146, 170)
#define C_ACCENT T48_RGB(120, 196, 255)

static t48_surface *s;
static char last_input[64] = "Press a key or touch the screen";
static char shown[16];

static void draw_time(int force)
{
    char hms[16], date[64];
    time_t t = time(NULL);
    struct tm tm;
    int seconds = strcmp(t48_setting("seconds", "true"), "false") != 0;

    localtime_r(&t, &tm);
    strftime(hms, sizeof hms, seconds ? "%H:%M:%S" : "%H:%M", &tm);
    if (!force && !strcmp(hms, shown)) return;
    strcpy(shown, hms);
    strftime(date, sizeof date, "%A %e %B %Y", &tm);
    t48_fill(s, 0, 110, s->w, 220, C_BG);
    t48_text_centre(s, s->w / 2, 120, &t48_font_huge, C_TEXT, hms);
    t48_text_centre(s, s->w / 2, 270, &t48_font_head, C_DIM, date);
    t48_damage(0, 110, s->w, 220);
}

static void draw_input(void)
{
    t48_fill(s, 0, 350, s->w, 40, C_BG);
    t48_text_centre(s, s->w / 2, 356, &t48_font_normal, C_ACCENT, last_input);
    t48_damage(0, 350, s->w, 40);
}

static void draw_all(void)
{
    t48_fill(s, 0, 0, s->w, s->h, C_BG);
    t48_text(s, 24, 20, &t48_font_head, C_ACCENT, t48_setting("title", "Clock"));
    t48_text_centre(s, s->w / 2, 440, &t48_font_small, C_DIM, "X: close     hold X: home");
    draw_time(1);
    draw_input();
    t48_damage_all();
}

static const char *key_name(int key)
{
    static char b[8];
    switch (key) {
    case T48_KEY_UP: return "up";
    case T48_KEY_DOWN: return "down";
    case T48_KEY_LEFT: return "left";
    case T48_KEY_RIGHT: return "right";
    case T48_KEY_OK: return "OK";
    }
    snprintf(b, sizeof b, "%c", key);
    return b;
}

int main(void)
{
    t48_event ev;
    int paused = 0;

    if (t48_app_init() < 0) return 1;
    s = t48_screen();
    draw_all();
    t48_timer(0, 100, 1);
    while (t48_poll(&ev, -1) >= 0) {
        switch (ev.type) {
        case T48_EV_TIMER:
            if (!paused) draw_time(0);
            break;
        case T48_EV_KEY:
            if (!ev.state) break;
            if (ev.key == T48_KEY_BACK) return 0;
            snprintf(last_input, sizeof last_input, "Key: %s", key_name(ev.key));
            draw_input();
            break;
        case T48_EV_TOUCH:
            if (ev.state == T48_TOUCH_UP) break;
            t48_round_fill(s, ev.x - 6, ev.y - 6, 12, 12, 6, C_ACCENT);
            t48_damage(ev.x - 6, ev.y - 6, 12, 12);
            snprintf(last_input, sizeof last_input, "Touch at %d, %d", ev.x, ev.y);
            draw_input();
            break;
        case T48_EV_PAUSE:
            paused = 1;
            t48_paused();  /* nothing to tidy up: freeze us now */
            break;
        case T48_EV_RESUME:
            paused = 0;
            draw_all();
            break;
        case T48_EV_QUIT:
            return 0;
        }
    }
    return 0;
}
