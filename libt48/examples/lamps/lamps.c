// SPDX-License-Identifier: AGPL-3.0-or-later
/* lamps: a check of the lamps from an app (API level 5), and the smallest
 * program that uses them.
 *
 *   1       the Headset lamp to a steady level, a step brighter each press
 *   2       the Mute lamp flashes and dies away to a glow (a pattern, run by the shell)
 *   3       the three key lamps run one after another, round and round
 *   4       the message lamp flashes
 *   5       all three key lamps breathe together
 *   6       the app does the fading itself: Speaker up and down, a level every 20 ms
 *   0       every lamp back to the phone
 *   X       close
 */
#include <stdio.h>
#include "t48.h"

#define KEYS (1u << T48_LAMP_HEADSET | 1u << T48_LAMP_MUTE | 1u << T48_LAMP_SPEAKER)

static t48_surface *s;
static const char *doing = "Nothing set: the lamps are the phone's";
static int steady, sweep = -1;

static void draw(void)
{
    static const char *const help[] = {
        "1  Headset: steady, a step brighter", "2  Mute: flash, then a glow", "3  A run down the three keys",
        "4  Message lamp: flash", "5  All three breathe", "6  Speaker: faded by this app", "0  Back to the phone",
    };
    const t48_theme *t = t48_theme_now();
    t48_fill(s, 0, 0, s->w, s->h, t->bg);
    t48_text(s, 24, 20, &t48_font_head, t->text, "Lamps");
    for (int i = 0; i < 7; i++) t48_text(s, 40, 80 + i * 36, &t48_font_normal, t->text, help[i]);
    t48_text(s, 40, 360, &t48_font_normal, t48_lamps_ok() ? t->good : t->bad,
             t48_lamps_ok() ? doing : "This phone's software does not lend lamps to apps");
    t48_text_centre(s, s->w / 2, 440, &t48_font_small, t->dim, "X: close     hold X: home");
    t48_damage_all();
}

/* one lamp of the run: lit for a third of the round, each a third later than the one above */
static void run_lamp(int lamp, int place)
{
    struct t48_lamp_key k[5];
    int n = 0;
    if (place) k[n++] = (struct t48_lamp_key){(uint16_t)(place * 150), 0, 0};
    k[n++] = (struct t48_lamp_key){0, 255, 0};
    k[n++] = (struct t48_lamp_key){150, 0, 0};
    if (place < 2) k[n++] = (struct t48_lamp_key){(uint16_t)((2 - place) * 150), 0, 0};
    t48_lamp(lamp, 0);
    t48_lamp_pattern(1u << lamp, k, n, 1);
}

static void key(int code)
{
    static const struct t48_lamp_key flash[] = {{0, 255, 0}, {300, 40, 0}};
    static const struct t48_lamp_key breathe[] = {{900, 255, 0}, {900, 8, 0}};
    sweep = -1;
    switch (code) {
    case '1':
        steady = steady >= 250 ? 50 : steady + 50;
        t48_lamp(T48_LAMP_HEADSET, steady);
        doing = "Headset: steady";
        fprintf(stderr, "lamps: headset %d\n", steady);
        break;
    case '2':
        t48_lamp_pattern(1u << T48_LAMP_MUTE, flash, 2, 0);
        doing = "Mute: a flash, then a glow";
        break;
    case '3':
        run_lamp(T48_LAMP_HEADSET, 0);
        run_lamp(T48_LAMP_MUTE, 1);
        run_lamp(T48_LAMP_SPEAKER, 2);
        doing = "A run down the three keys";
        break;
    case '4':
        t48_lamp_pulse(1u << T48_LAMP_MESSAGE, 255, 250);
        doing = "Message lamp: a flash";
        break;
    case '5':
        t48_lamp_pattern(KEYS, breathe, 2, 1);
        doing = "All three breathe";
        break;
    case '6':
        sweep = 0;
        doing = "Speaker: faded by this app";
        break;
    case '0':
        t48_lamps_release(KEYS | 1u << T48_LAMP_MESSAGE);
        steady = 0;
        doing = "Nothing set: the lamps are the phone's";
        break;
    default:
        return;
    }
    draw();
}

int main(void)
{
    t48_event ev;

    if (t48_app_init() < 0) return 1;
    s = t48_screen();
    draw();
    t48_timer(0, 20, 1);
    while (t48_poll(&ev, -1) >= 0) {
        switch (ev.type) {
        case T48_EV_TIMER:
            if (sweep < 0) break;
            sweep = (sweep + 5) % 510;   /* up to 255 and down again in two seconds */
            t48_lamp(T48_LAMP_SPEAKER, sweep < 255 ? sweep : 510 - sweep);
            break;
        case T48_EV_KEY:
            if (!ev.state) break;
            if (ev.key == T48_KEY_BACK) return 0;
            key(ev.key);
            break;
        case T48_EV_PAUSE:
            /* the shell has taken the lamps back; they stay the phone's until a key sets one again */
            sweep = -1;
            steady = 0;
            doing = "Nothing set: the lamps are the phone's";
            break;
        case T48_EV_RESUME:
        case T48_EV_THEME:
            draw();
            break;
        case T48_EV_QUIT:
            return 0;
        }
    }
    return 0;
}
