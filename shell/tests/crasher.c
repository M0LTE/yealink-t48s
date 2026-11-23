// SPDX-License-Identifier: AGPL-3.0-or-later
/* crasher: a badly behaved app for testing the shell. 1 crashes, 2 exits with
 * an error, 3 hangs without reading its events and ignores QUIT. */
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include "t48.h"

int main(void)
{
    t48_event ev;
    if (t48_app_init() < 0) return 1;
    t48_surface *s = t48_screen();
    t48_fill(s, 0, 0, s->w, s->h, T48_RGB(40, 12, 12));
    t48_text(s, 40, 40, &t48_font_big, T48_RGB(255, 255, 255), "Crasher");
    t48_text(s, 40, 120, &t48_font_normal, T48_RGB(220, 200, 200), "1 crash    2 exit with an error    3 hang");
    t48_damage_all();
    while (t48_poll(&ev, -1) >= 0) {
        if (ev.type != T48_EV_KEY || !ev.state) continue;
        if (ev.key == '1') raise(SIGSEGV);
        if (ev.key == '2') return 3;
        if (ev.key == '3') for (;;) pause();
    }
    return 0;
}
