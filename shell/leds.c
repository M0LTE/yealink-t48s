// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: the lamps. The phone part of the shell says what each lamp
 * should be doing; leds_apply() tells the kernel's LED devices through
 * /sys/class/leds. The phone has four lamps: the message lamp (top right of
 * the case; it blinks for a ringing call and for voicemail, as Yealink has
 * it), and one each in the Headset, Mute and Speakerphone keys. A kernel
 * built before the lamp was renamed still calls it "power", so that name is
 * tried too.
 *
 * The app in front may borrow lamps (API level 5, docs/app-protocol.md): a
 * level for each, or a pattern of fades that is stepped here fifty times a
 * second. What the phone wants is kept all the while and goes back on the
 * lamp the moment the app gives it up, is paused or dies. The key lamps have
 * 255 levels (kernel6/drivers/t48s_lamps.c); the message lamp, and any lamp
 * of an older kernel, is on or off, on from level 128, and the message lamp is
 * changed at most twenty times a second because each change keeps the kernel
 * busy for 7 ms.
 *
 * Where there is no such LED (the simulator on a PC) nothing happens; the
 * state is still kept, logged, and can be read by the simulator's checks
 * ("led.speaker" for what the phone wants, "lamp.speaker" for what is lit). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "shell.h"

#define BLINK_MS "300"    /* on for this long, off for this long: the rate Yealink rings at */
#define SLOW_MS "1000"    /* and the rate it shows voicemail at */
#define STEP_MS 20        /* a pattern's fades are stepped this often */
#define SWITCH_MS 50      /* an on-or-off lamp lent to an app is changed no faster than this */

static const char *const names[LAMP_COUNT] = {"message", "speaker", "headset", "mute"};
static const char *const leds[LAMP_COUNT] = {"t48s:red:message", "t48s:green:speaker", "t48s:green:headset", "t48s:red:mute"};
static const char *const old_leds[LAMP_COUNT] = {"t48s:red:power", NULL, NULL, NULL};
static const char *const modes[] = {"off", "on", "blink", "slow"};
static const enum led of_app[T48_LAMPS] = {LAMP_MESSAGE, LAMP_HEADSET, LAMP_MUTE, LAMP_SPEAKER};   /* by T48_LAMP_* */

static struct lamp {
    enum led_mode want;   /* the phone's */
    char applied;         /* told the hardware at least once */
    char app;             /* lent to the app */
    int level;            /* the app's, 0 to 255 */
    int out;              /* what the device was last told for the app, -1: nothing yet */
    int fd;               /* its brightness, open while the app has the lamp; -1: no such device */
    int max;              /* the device's highest brightness */
    long long quiet;      /* an on-or-off lamp: not to be changed again before this */
    struct t48_lamp_key key[T48_LAMP_KEYS];   /* the pattern it is running */
    int nkeys, loop, from;
    long long t0, total;
} lamp[LAMP_COUNT];
static long long stepped;   /* when the patterns were last stepped */

/* the LED device of a lamp: its name now, or the one an older kernel gave it */
static const char *device(enum led which)
{
    char path[96];
    snprintf(path, sizeof path, "/sys/class/leds/%s", leds[which]);
    return access(path, F_OK) && old_leds[which] ? old_leds[which] : leds[which];
}

static int led_write(const char *led, const char *file, const char *value)
{
    char path[96];
    snprintf(path, sizeof path, "/sys/class/leds/%s/%s", led, file);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, value, strlen(value));
    close(fd);
    return n < 0 ? -1 : 0;
}

/* Make the hardware match what the phone wants. */
static void leds_apply(enum led which, enum led_mode mode)
{
    const char *led = device(which);
    if (mode == LAMP_BLINK || mode == LAMP_SLOW) {
        /* the kernel's "timer" trigger does the blinking */
        if (led_write(led, "trigger", "timer")) return;
        led_write(led, "delay_on", mode == LAMP_SLOW ? SLOW_MS : BLINK_MS);
        led_write(led, "delay_off", mode == LAMP_SLOW ? SLOW_MS : BLINK_MS);
    } else {
        if (led_write(led, "trigger", "none")) return;
        /* as bright as it goes: 255 on a lamp with levels, and a lamp without takes it as 1 */
        led_write(led, "brightness", mode == LAMP_ON ? "255" : "0");
    }
}

void leds_set(enum led which, enum led_mode mode)
{
    if (which >= LAMP_COUNT) return;
    struct lamp *l = &lamp[which];
    /* the first time, tell the hardware whatever it is: a lamp may have been left on */
    if (l->want == mode && l->applied) return;
    if (l->want != mode) logmsg("lamp %s: %s", names[which], modes[mode]);
    l->want = mode;
    l->applied = 1;
    if (!l->app) leds_apply(which, mode);   /* an app has it: the phone's turn comes when the app lets go */
}

const char *leds_state(const char *name)
{
    for (int i = 0; i < LAMP_COUNT; i++)
        if (!strcmp(name, names[i])) return modes[lamp[i].want];
    return NULL;
}

const char *leds_shown(const char *name)
{
    static char b[8];
    for (int i = 0; i < LAMP_COUNT; i++) {
        if (strcmp(name, names[i])) continue;
        if (!lamp[i].app) return modes[lamp[i].want];
        snprintf(b, sizeof b, "%d", lamp[i].level);
        return b;
    }
    return NULL;
}

/* ---------- lent to the app ---------- */

static void take(enum led which)
{
    struct lamp *l = &lamp[which];
    char path[96], num[16] = "";
    const char *led = device(which);

    if (l->app) return;
    logmsg("lamp %s: the app's", names[which]);
    l->app = 1;
    l->level = 0;
    l->out = -1;
    l->nkeys = 0;
    l->quiet = 0;
    l->max = 0;
    led_write(led, "trigger", "none");
    snprintf(path, sizeof path, "/sys/class/leds/%s/max_brightness", led);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, num, sizeof num - 1) > 0) l->max = atoi(num);
        close(fd);
    }
    snprintf(path, sizeof path, "/sys/class/leds/%s/brightness", led);
    l->fd = l->max > 0 ? open(path, O_WRONLY | O_CLOEXEC) : -1;
}

/* the app's level to the device, if it is not there already and may go now */
static void show(struct lamp *l, long long now)
{
    char num[8];
    int v = l->max >= 255 ? l->level : l->level >= 128 ? l->max : 0;
    if (l->fd < 0 || v == l->out) return;
    if (l->max < 255) {
        if (now < l->quiet) return;   /* leds_tick() comes back for it */
        l->quiet = now + SWITCH_MS;
    }
    int n = snprintf(num, sizeof num, "%d", v);
    if (pwrite(l->fd, num, (size_t)n, 0) < 0) return;
    l->out = v;
}

/* where a pattern has its lamp, t ms after it began; *done when it has ended */
static int pattern_level(const struct lamp *l, long long t, int *done)
{
    int from = l->from;
    if (l->loop && l->total > 0 && t >= l->total) {
        t %= l->total;
        from = l->key[l->nkeys - 1].level;   /* every round after the first starts where the last ended */
    }
    for (int i = 0; i < l->nkeys; i++) {
        if (t < l->key[i].ms) return from + (l->key[i].level - from) * (int)t / l->key[i].ms;
        t -= l->key[i].ms;
        from = l->key[i].level;
    }
    *done = 1;
    return from;
}

void leds_app_set(unsigned mask, const uint8_t *level)
{
    long long now = t48_now_ms();
    for (int n = 0; n < T48_LAMPS; n++) {
        if (!(mask >> n & 1)) continue;
        struct lamp *l = &lamp[of_app[n]];
        take(of_app[n]);
        l->nkeys = 0;
        l->level = level[n];
        show(l, now);
    }
}

void leds_app_pattern(unsigned mask, int loop, const struct t48_lamp_key *key, int n)
{
    long long now = t48_now_ms();
    if (n < 1 || n > T48_LAMP_KEYS) return;
    for (int k = 0; k < T48_LAMPS; k++) {
        if (!(mask >> k & 1)) continue;
        struct lamp *l = &lamp[of_app[k]];
        int done = 0;
        take(of_app[k]);
        memcpy(l->key, key, (size_t)n * sizeof *key);
        l->nkeys = n;
        l->loop = loop;
        l->from = l->level;
        l->t0 = now;
        l->total = 0;
        for (int i = 0; i < n; i++) l->total += key[i].ms;
        l->level = pattern_level(l, 0, &done);
        if (done) l->nkeys = 0;   /* nothing but jumps: it is there already */
        show(l, now);
    }
    stepped = now;
}

void leds_app_release(unsigned mask)
{
    for (int n = 0; n < T48_LAMPS; n++) {
        struct lamp *l = &lamp[of_app[n]];
        if (!(mask >> n & 1) || !l->app) continue;
        logmsg("lamp %s: the phone's again, %s", names[of_app[n]], modes[l->want]);
        if (l->fd >= 0) close(l->fd);
        l->fd = -1;
        l->app = 0;
        l->nkeys = 0;
        leds_apply(of_app[n], l->want);
    }
}

int leds_timeout(long long now)
{
    long long at = -1;
    for (int i = 0; i < LAMP_COUNT; i++) {
        const struct lamp *l = &lamp[i];
        if (!l->app) continue;
        if (l->nkeys && (at < 0 || stepped + STEP_MS < at)) at = stepped + STEP_MS;
        int v = l->max >= 255 ? l->level : l->level >= 128 ? l->max : 0;
        if (l->fd >= 0 && v != l->out && (at < 0 || l->quiet < at)) at = l->quiet;   /* a change that had to wait */
    }
    return at < 0 ? -1 : at > now ? (int)(at - now) : 0;
}

void leds_tick(long long now)
{
    int step = now >= stepped + STEP_MS;
    for (int i = 0; i < LAMP_COUNT; i++) {
        struct lamp *l = &lamp[i];
        if (!l->app) continue;
        if (l->nkeys && step) {
            int done = 0;
            l->level = pattern_level(l, now - l->t0, &done);
            if (done) l->nkeys = 0;
        }
        show(l, now);
    }
    if (step) stepped = now;
}
