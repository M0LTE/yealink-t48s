// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: the lamps. The phone part of the shell says what each lamp
 * should be doing; leds_apply() tells the kernel's LED driver
 * (kernel/drivers/t48s_leds.c) through /sys/class/leds. The phone has four
 * lamps: power (top right of the case), and one each in the Headset, Mute
 * and Speakerphone keys. The Message key has none, so that lamp is only
 * kept here, for the day voicemail (issue #27) decides how to show it.
 * Where there is no such LED (the simulator on a PC) nothing happens; the
 * wanted state is still kept, logged, and can be read by the simulator's
 * checks ("led.speaker" and so on). */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "shell.h"

#define BLINK_MS "300"    /* on for this long, off for this long: the rate Yealink rings at */

static const char *const names[LAMP_COUNT] = {"power", "speaker", "headset", "mute", "message"};
static const char *const leds[LAMP_COUNT] = {"t48s:red:power", "t48s:green:speaker", "t48s:green:headset", "t48s:red:mute", NULL};
static const char *const modes[] = {"off", "on", "blink"};
static enum led_mode want[LAMP_COUNT];
static char applied[LAMP_COUNT];    /* told the hardware at least once */

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

/* Make the hardware match. */
static void leds_apply(enum led which, enum led_mode mode)
{
    const char *led = leds[which];
    if (!led) return;
    if (mode == LAMP_BLINK) {
        /* the kernel's "timer" trigger does the blinking */
        if (led_write(led, "trigger", "timer")) return;
        led_write(led, "delay_on", BLINK_MS);
        led_write(led, "delay_off", BLINK_MS);
    } else {
        if (led_write(led, "trigger", "none")) return;
        led_write(led, "brightness", mode == LAMP_ON ? "1" : "0");
    }
}

void leds_set(enum led which, enum led_mode mode)
{
    if (which >= LAMP_COUNT) return;
    /* the first time, tell the hardware whatever it is: a lamp may have been left on */
    if (want[which] == mode && applied[which]) return;
    if (want[which] != mode) logmsg("lamp %s: %s", names[which], modes[mode]);
    want[which] = mode;
    applied[which] = 1;
    leds_apply(which, mode);
}

const char *leds_state(const char *name)
{
    for (int i = 0; i < LAMP_COUNT; i++)
        if (!strcmp(name, names[i])) return modes[want[i]];
    return NULL;
}
