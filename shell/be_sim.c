// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell as the PC simulator: the screen is a picture in memory, input
 * comes from a script, and the script can save the picture and check which
 * screen is showing. The script language is described in docs/shell.md. */
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
static enum { W_NONE, W_FRAME, W_SCREEN, W_STATE } waiting;
static unsigned frame_mark;  /* the app's picture count at the last input or waitframe */
static char wait_screen[16];
static char wait_key[32], wait_value[100];
static long long wait_until;

static const struct { const char *name; int code; } keys[] = {
    {"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'}, {"5", '5'}, {"6", '6'}, {"7", '7'},
    {"8", '8'}, {"9", '9'}, {"star", T48_KEY_STAR}, {"*", T48_KEY_STAR}, {"hash", T48_KEY_HASH}, {"#", T48_KEY_HASH},
    {"up", T48_KEY_UP}, {"down", T48_KEY_DOWN}, {"left", T48_KEY_LEFT}, {"right", T48_KEY_RIGHT},
    {"ok", T48_KEY_OK}, {"x", T48_KEY_BACK},
    {"voldown", SK_VOLDOWN}, {"volup", SK_VOLUP}, {"headset", SK_HEADSET}, {"mute", SK_MUTE},
    {"message", SK_MESSAGE}, {"hold", SK_HOLD}, {"redial", SK_REDIAL}, {"transfer", SK_TRANSFER},
    {"speaker", SK_SPEAKER},
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

/* run one line of the script */
static void step(long long now)
{
    char line[300], cmd[32] = "", a[200] = "", b[100] = "", c[32] = "";
    size_t n = strcspn(at, "\n");

    if (!*at) { shell_stop(failures ? 1 : 0); return; }
    snprintf(line, sizeof line, "%.*s", (int)n, at);
    at += n + (at[n] == '\n');
    line_no++;
    if (strncmp(line, "run ", 4)) line[strcspn(line, "#")] = 0;
    if (sscanf(line, "%31s %199s %99s %31s", cmd, a, b, c) < 1 || cmd[0] == '#') return;
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
    if (waiting) {
        const char *v = waiting == W_STATE ? shell_state(wait_key) : NULL;
        int done = waiting == W_FRAME ? shell_app_frames() != frame_mark :
                   waiting == W_STATE ? v && !strcmp(v, wait_value) : !strcmp(wait_screen, shell_screen_name());
        if (!done && now < wait_until) return;
        if (!done) {
            char m[240];
            if (waiting == W_FRAME) snprintf(m, sizeof m, "the app drew nothing in time");
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

const struct backend be_sim = {"simulator", sim_init, sim_flush, sim_fds, sim_handle, sim_timeout, sim_tick, sim_fini};
