// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell as the PC simulator: the screen is a picture in memory, input
 * comes from a script, and the script can save the picture and check which
 * screen is showing. The script language is described in docs/shell.md. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell.h"

static uint32_t screen_px[SW * SH];
static const t48_surface screen = {screen_px, SW, SH, SW};

static char *script;        /* the whole script */
static char *at;            /* next line */
static int line_no, failures;
static long long resume_at; /* nothing happens before this */
static int release_key;     /* key to let go at resume_at, 0: none */
static enum { W_NONE, W_FRAME, W_SCREEN, W_STATE, W_PIXEL } waiting;
static unsigned frame_mark;  /* the app's picture count at the last input or waitframe */
static char wait_screen[16];
static char wait_key[32], wait_value[100];
static int wait_x, wait_y;
static uint32_t wait_colour;
static long long wait_until;

static const struct { const char *name; int code; } keys[] = {
    {"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'}, {"5", '5'}, {"6", '6'}, {"7", '7'},
    {"8", '8'}, {"9", '9'}, {"star", T48_KEY_STAR}, {"*", T48_KEY_STAR}, {"hash", T48_KEY_HASH}, {"#", T48_KEY_HASH},
    {"up", T48_KEY_UP}, {"down", T48_KEY_DOWN}, {"left", T48_KEY_LEFT}, {"right", T48_KEY_RIGHT},
    {"ok", T48_KEY_OK}, {"x", T48_KEY_BACK},
    {"voldown", SK_VOLDOWN}, {"volup", SK_VOLUP}, {"headset", SK_HEADSET}, {"mute", SK_MUTE},
    {"message", SK_MESSAGE}, {"hold", SK_HOLD}, {"redial", SK_REDIAL}, {"transfer", SK_TRANSFER},
    {"speaker", SK_SPEAKER},
    {"soft1", SK_SOFT1}, {"soft2", SK_SOFT2}, {"soft3", SK_SOFT3}, {"soft4", SK_SOFT4},
};

static int key_named(const char *name)
{
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (!strcmp(keys[i].name, name)) return keys[i].code;
    return 0;
}

static int sim_init(void)
{
    static char one_shot[400];
    if (opt.shot) {
        /* an app's first picture if one was asked for, otherwise the idle screen */
        snprintf(one_shot, sizeof one_shot, "%swait 200\nshot %s\nquit\n", opt.start ? "waitframe 5000\n" : "", opt.shot);
        script = one_shot;
    } else if (opt.script) {
        FILE *f = strcmp(opt.script, "-") ? fopen(opt.script, "r") : stdin;
        if (!f) { perror(opt.script); return -1; }
        script = calloc(1, 65536);
        if (!script || fread(script, 1, 65535, f) == 0) { fprintf(stderr, "%s: empty\n", opt.script); return -1; }
        if (f != stdin) fclose(f);
    } else {
        fprintf(stderr, "t48-sim: nothing to do without --script or --shot (there is no window yet)\n");
        return -1;
    }
    at = script;
    return 0;
}

static void sim_flush(const uint32_t *src, int stride, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    for (int j = 0; j < h; j++)
        memcpy(screen_px + (y + j) * SW + x, src + (y + j) * stride + x, (size_t)w * 4);
}

static int sim_fds(struct pollfd *p, int max) { (void)p; (void)max; return 0; }
static void sim_handle(const struct pollfd *p, int n) { (void)p; (void)n; }

static int sim_timeout(long long now)
{
    if (waiting) return 10;
    return resume_at > now ? (int)(resume_at - now) : 0;
}

static void fail(const char *what)
{
    logmsg("script line %d: FAILED: %s", line_no, what);
    failures++;
}

static uint32_t pixel(int x, int y) { return screen_px[y * SW + x] & 0xFFFFFF; }

/* RRGGBB, or the name of one of the theme's colours */
static int colour_named(const char *name, uint32_t *out)
{
    static const struct { const char *name; size_t at; } roles[] = {
        {"bg", offsetof(t48_theme, bg)}, {"panel", offsetof(t48_theme, panel)}, {"text", offsetof(t48_theme, text)},
        {"key", offsetof(t48_theme, key)}, {"sel", offsetof(t48_theme, sel)}, {"head", offsetof(t48_theme, head)},
        {"accent", offsetof(t48_theme, accent)}, {"good", offsetof(t48_theme, good)}, {"bad", offsetof(t48_theme, bad)},
        {"ring", offsetof(t48_theme, ground_ring)}, {"call", offsetof(t48_theme, ground_call)}, {"hold", offsetof(t48_theme, ground_hold)},
    };
    char *end;
    for (unsigned i = 0; i < sizeof roles / sizeof roles[0]; i++)
        if (!strcmp(roles[i].name, name)) { memcpy(out, (const char *)T + roles[i].at, sizeof *out); return 1; }
    *out = (uint32_t)strtoul(name, &end, 16);
    return strlen(name) == 6 && !*end;
}

/* run one line of the script */
static void step(long long now)
{
    char line[300], cmd[32] = "", a[200] = "", b[100] = "", c[32] = "", d[32] = "";
    size_t n = strcspn(at, "\n");

    if (!*at) { shell_stop(failures ? 1 : 0); return; }
    snprintf(line, sizeof line, "%.*s", (int)n, at);
    at += n + (at[n] == '\n');
    line_no++;
    if (strncmp(line, "run ", 4)) line[strcspn(line, "#")] = 0;
    if (sscanf(line, "%31s %199s %99s %31s %31s", cmd, a, b, c, d) < 1 || cmd[0] == '#') return;
    if (!strncmp(cmd, "key", 3) || !strncmp(cmd, "touch", 5) || !strcmp(cmd, "hook")) frame_mark = shell_app_frames();

    if (!strcmp(cmd, "wait")) {
        resume_at = now + atoi(a);
    } else if (!strcmp(cmd, "key") || !strcmp(cmd, "keydown") || !strcmp(cmd, "keyup")) {
        int code = key_named(a);
        if (!code) { fail("no such key"); return; }
        if (strcmp(cmd, "keyup")) shell_key(code, 1);
        if (!strcmp(cmd, "keyup")) shell_key(code, 0);
        if (!strcmp(cmd, "key")) {  /* press, hold (60 ms unless told), let go */
            release_key = code;
            resume_at = now + (b[0] ? atoi(b) : 60);
        }
    } else if (!strcmp(cmd, "scandown") || !strcmp(cmd, "scanup")) {
        /* a key as the phone's keypad sends it: its place in the matrix, the Linux key code its table
         * gives (none: a place with no key event), and the name of the key that is to the shell, if any */
        int code = c[0] ? key_named(c) : 0;
        if (c[0] && !code) { fail("no such key"); return; }
        shell_scan(atoi(a), atoi(b), code, !b[0] ? -1 : cmd[4] == 'd');
    } else if (!strcmp(cmd, "touch")) {  /* a tap */
        shell_touch(T48_TOUCH_DOWN, atoi(a), atoi(b));
        shell_touch(T48_TOUCH_UP, atoi(a), atoi(b));
    } else if (!strcmp(cmd, "touchdown")) {
        shell_touch(T48_TOUCH_DOWN, atoi(a), atoi(b));
    } else if (!strcmp(cmd, "touchmove")) {
        shell_touch(T48_TOUCH_MOVE, atoi(a), atoi(b));
    } else if (!strcmp(cmd, "touchup")) {
        shell_touch(T48_TOUCH_UP, atoi(a), atoi(b));
    } else if (!strcmp(cmd, "hook")) {
        shell_hook(!strcmp(a, "off"));
    } else if (!strcmp(cmd, "shot")) {
        if (t48_save(&screen, a) < 0) fail("cannot write the picture");
        else logmsg("wrote %s", a);
    } else if (!strcmp(cmd, "expect")) {
        if (strcmp(a, shell_screen_name())) {
            char m[80];
            snprintf(m, sizeof m, "expected the %s screen, showing %s", a, shell_screen_name());
            fail(m);
        }
    } else if (!strcmp(cmd, "waitscreen")) {
        waiting = W_SCREEN;
        snprintf(wait_screen, sizeof wait_screen, "%s", a);
        wait_until = now + (b[0] ? atoi(b) : 3000);
    } else if (!strcmp(cmd, "expectstate")) {
        const char *v = shell_state(a);
        if (!v || strcmp(v, b)) {
            char m[200];
            snprintf(m, sizeof m, "expected %s to be %s, it is %s", a, b, v ? v : "(no such thing)");
            fail(m);
        }
    } else if (!strcmp(cmd, "waitstate")) {
        waiting = W_STATE;
        snprintf(wait_key, sizeof wait_key, "%s", a);
        snprintf(wait_value, sizeof wait_value, "%s", b);
        wait_until = now + (c[0] ? atoi(c) : 3000);
    } else if (!strcmp(cmd, "expectpixel") || !strcmp(cmd, "waitpixel")) {
        /* the colour of one pixel of the screen: RRGGBB, or a role of the theme in use (bg, text, sel, ...) */
        wait_x = atoi(a);
        wait_y = atoi(b);
        if (wait_x < 0 || wait_x >= SW || wait_y < 0 || wait_y >= SH || !colour_named(c, &wait_colour)) { fail("no such pixel or colour"); return; }
        if (cmd[0] == 'w') {
            waiting = W_PIXEL;
            wait_until = now + (d[0] ? atoi(d) : 3000);
        } else if (pixel(wait_x, wait_y) != wait_colour) {
            char m[120];
            snprintf(m, sizeof m, "expected the pixel at %d, %d to be %06X, it is %06X", wait_x, wait_y, (unsigned)wait_colour, (unsigned)pixel(wait_x, wait_y));
            fail(m);
        }
    } else if (!strcmp(cmd, "run")) {
        /* a command for the PC's shell, such as making the far end of a test call do something */
        if (system(line + 4) != 0) fail("the command failed");
    } else if (!strcmp(cmd, "waitframe")) {
        waiting = W_FRAME;  /* until the app has drawn since the last input */
        wait_until = now + (a[0] ? atoi(a) : 3000);
    } else if (!strcmp(cmd, "quit")) {
        shell_stop(failures ? 1 : 0);
    } else {
        fail("no such command");
    }
}

static void sim_tick(long long now)
{
    shell_input_us = t48_now_us();   /* a script's keys and touches happen as their line is run */
    if (waiting) {
        const char *v = waiting == W_STATE ? shell_state(wait_key) : NULL;
        int done = waiting == W_FRAME ? shell_app_frames() != frame_mark :
                   waiting == W_PIXEL ? pixel(wait_x, wait_y) == wait_colour :
                   waiting == W_STATE ? v && !strcmp(v, wait_value) : !strcmp(wait_screen, shell_screen_name());
        if (!done && now < wait_until) return;
        if (!done) {
            char m[240];
            if (waiting == W_FRAME) snprintf(m, sizeof m, "the app drew nothing in time");
            else if (waiting == W_PIXEL) snprintf(m, sizeof m, "waited for the pixel at %d, %d to be %06X, it is still %06X", wait_x, wait_y, (unsigned)wait_colour, (unsigned)pixel(wait_x, wait_y));
            else if (waiting == W_STATE) snprintf(m, sizeof m, "waited for %s to be %s, it is still %s", wait_key, wait_value, v ? v : "(no such thing)");
            else snprintf(m, sizeof m, "waited for the %s screen, still showing %s", wait_screen, shell_screen_name());
            fail(m);
        }
        waiting = W_NONE;
        frame_mark = shell_app_frames();
    }
    if (now < resume_at) return;
    if (release_key) {
        shell_key(release_key, 0);
        release_key = 0;
    }
    step(now);
}

static void sim_fini(void) {}

const struct backend be_sim = {"simulator", sim_init, sim_flush, sim_fds, sim_handle, sim_timeout, sim_tick, sim_fini, NULL, NULL, NULL};
