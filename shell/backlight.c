// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-shell: the backlight (issue #18). The shell decides when the screen
 * is bright, dimmed or off; the kernel's backlight driver
 * (kernel/drivers/t48s_backlight.c, /sys/class/backlight/t48s) does it.
 *
 *   bright   the brightness set in Settings, Screen
 *   dimmed   after dim_seconds with nothing touched: the lowest level
 *   off      after off_seconds with nothing touched: the lamp is off
 *
 * A key, a touch, the handset or a call brings it back. While the screen is
 * off nobody can see what a key or a touch would do, so the one that wakes
 * the screen does nothing else: the press is swallowed, down and up. Lifting
 * the handset and the Speakerphone and Headset keys are different: they ask
 * for a line whatever the screen shows, so they wake it and work as well. A
 * dimmed screen can still be read, so there nothing is swallowed.
 *
 * The screen stays on during a call, and it dims but never goes off while an
 * app is showing (an app is often something to watch, not to touch).
 *
 * Where there is no backlight device (the simulator on a PC) nothing is
 * written; the state is still kept, logged, and can be read by the
 * simulator's checks ("backlight", "backlight.level").
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell.h"

#define DIM_SECONDS 120       /* settings file "screen": dim_seconds, off_seconds; 0 is never */
#define OFF_SECONDS 1800
#define DIM_LEVEL 1
#define RELOAD_MS 5000        /* how often the settings file is looked at again */

enum bl { BL_ON, BL_DIM, BL_OFF };
static const char *const bl_names[] = {"on", "dim", "off"};

static enum bl state = BL_ON;
static int level = -1;            /* the brightness when bright, 1 to BL_LEVELS */
static int shown = -1;            /* what the hardware was last told: 0 off, else the level */
static long dim_ms, off_ms;
static long long last_touched, next_reload;
static int eaten_keys[8], neaten; /* keys whose press woke the screen: their release is swallowed too */
static int eating_touch;

static int bl_path(char *out, size_t n, const char *leaf)
{
    /* T48_BACKLIGHT: another folder to look in, for the simulator's tests */
    const char *base = getenv("T48_BACKLIGHT") ? getenv("T48_BACKLIGHT") : "/sys/class/backlight";
    DIR *d = opendir(base);
    struct dirent *de;
    int found = 0;
    if (!d) return 0;
    while (!found && (de = readdir(d)))
        if (de->d_name[0] != '.') { snprintf(out, n, "%s/%s/%s", base, de->d_name, leaf); found = 1; }
    closedir(d);
    return found;
}

static int bl_read(const char *leaf)
{
    char p[300];
    int v = -1;
    FILE *f;
    if (!bl_path(p, sizeof p, leaf) || !(f = fopen(p, "r"))) return -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void bl_write(const char *leaf, int v)
{
    char p[300];
    FILE *f;
    if (!bl_path(p, sizeof p, leaf) || !(f = fopen(p, "w"))) return;
    fprintf(f, "%d\n", v);
    fclose(f);
}

int backlight_present(void)
{
    char p[300];
    return bl_path(p, sizeof p, "brightness");
}

/* make the hardware show this: 0 is off */
static void show(int v)
{
    if (v == shown) return;
    if (v > 0) {
        int max = bl_read("max_brightness");
        if (max > 0) bl_write("brightness", v * max / BL_LEVELS > 0 ? v * max / BL_LEVELS : 1);
        if (shown <= 0) bl_write("bl_power", 0);
    } else {
        bl_write("bl_power", 4);   /* FB_BLANK_POWERDOWN: off, and the level is kept */
    }
    shown = v;
}

static void enter(enum bl s, const char *why)
{
    if (s != state) logmsg("backlight: %s (%s)", bl_names[s], why);
    state = s;
    show(s == BL_OFF ? 0 : s == BL_DIM && level > DIM_LEVEL ? DIM_LEVEL : level);
}

static void reload(void)
{
    int b = atoi(cfg_str("screen", "brightness", "-1"));
    dim_ms = atol(cfg_str("screen", "dim_seconds", "-1")) * 1000;
    off_ms = atol(cfg_str("screen", "off_seconds", "-1")) * 1000;
    if (dim_ms < 0) dim_ms = DIM_SECONDS * 1000L;
    if (off_ms < 0) off_ms = OFF_SECONDS * 1000L;
    if (b >= 0) {
        b = b < 1 ? 1 : b > BL_LEVELS ? BL_LEVELS : b;   /* never fully dark: the screen is how you get back */
        if (b != level) { level = b; if (state == BL_ON) show(level); }
    }
}

void backlight_init(void)
{
    last_touched = t48_now_ms();
    reload();
    next_reload = last_touched + RELOAD_MS;
    if (level < 0) {
        /* nothing set: bright is whatever the hardware was left at (U-Boot's 8 of 10) */
        int max = bl_read("max_brightness"), now = bl_read("brightness");
        level = max > 0 && now > 0 ? (now * BL_LEVELS + max - 1) / max : BL_LEVELS;
    }
    enter(BL_ON, "start");
}

int backlight_level(void) { return level; }

void backlight_set_level(int v)
{
    level = v < 1 ? 1 : v > BL_LEVELS ? BL_LEVELS : v;
    last_touched = t48_now_ms();
    state = BL_ON;
    show(level);
}

/* something happened that the owner should see, such as a call arriving */
void backlight_wake(const char *why)
{
    last_touched = t48_now_ms();
    if (state != BL_ON) enter(BL_ON, why);
}

/* A key went down or up. 1: it only woke the screen, and goes no further. */
int backlight_key(int code, int down)
{
    if (!down) {
        for (int i = 0; i < neaten; i++)
            if (eaten_keys[i] == code) {
                eaten_keys[i] = eaten_keys[--neaten];
                return 1;
            }
        return 0;
    }
    int was_off = state == BL_OFF;
    backlight_wake("a key");
    if (!was_off || code == SK_SPEAKER || code == SK_HEADSET) return 0;
    if (neaten < (int)(sizeof eaten_keys / sizeof eaten_keys[0])) eaten_keys[neaten++] = code;
    return 1;
}

/* A touch began, moved or ended. 1: it only woke the screen. */
int backlight_touch(int touch_state)
{
    if (touch_state == T48_TOUCH_DOWN) {
        eating_touch = state == BL_OFF;
        backlight_wake("a touch");
        return eating_touch;
    }
    if (!eating_touch) { last_touched = t48_now_ms(); return 0; }
    if (touch_state == T48_TOUCH_UP) eating_touch = 0;
    return 1;
}

static int held_on(void)
{
    /* a call, ringing or dialling: the phone is in use */
    const char *ph = phone_state("phone");
    return ph && strcmp(ph, "idle");
}

int backlight_timeout(long long now)
{
    long long next = next_reload;
    if (state == BL_ON && dim_ms > 0 && last_touched + dim_ms < next) next = last_touched + dim_ms;
    /* an app showing is dimmed at most, so there the second deadline only matters while still bright */
    if (state != BL_OFF && off_ms > 0 && last_touched + off_ms < next &&
        (state == BL_ON || strcmp(shell_screen_name(), "app")))
        next = last_touched + off_ms;
    return next > now ? (int)(next - now) : 0;
}

void backlight_tick(long long now)
{
    if (now >= next_reload) { next_reload = now + RELOAD_MS; reload(); }
    if (held_on()) { backlight_wake("the phone"); return; }
    long long idle = now - last_touched;
    int app_showing = !strcmp(shell_screen_name(), "app");
    if (off_ms > 0 && idle >= off_ms && !app_showing) {
        if (state != BL_OFF) enter(BL_OFF, "nothing touched");
    } else if ((dim_ms > 0 && idle >= dim_ms) || (off_ms > 0 && idle >= off_ms)) {
        if (state == BL_ON) enter(BL_DIM, "nothing touched");
    }
}

const char *backlight_state(const char *key)
{
    static char b[12];
    if (!strcmp(key, "backlight")) return bl_names[state];
    if (!strcmp(key, "backlight.level")) { snprintf(b, sizeof b, "%d", shown < 0 ? level : shown); return b; }
    return NULL;
}
