// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: the lamps. The phone part of the shell says what each lamp
 * should be doing; leds_apply() tells the kernel's LED driver
 * (kernel/drivers/t48s_leds.c) through /sys/class/leds. The phone has four
 * lamps: the message lamp (top right of the case; it blinks for a ringing
 * call and for voicemail, as Yealink has it), and one each in the Headset,
 * Mute and Speakerphone keys. A kernel built before the lamp was renamed
 * still calls it "power", so that name is tried too.
 * Where there is no such LED (the simulator on a PC) nothing happens; the
 * wanted state is still kept, logged, and can be read by the simulator's
 * checks ("led.speaker" and so on). */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "shell.h"

#define BLINK_MS "300"    /* on for this long, off for this long: the rate Yealink rings at */
#define SLOW_MS "1000"    /* and the rate it shows voicemail at */

static const char *const names[LAMP_COUNT] = {"message", "speaker", "headset", "mute"};
static const char *const leds[LAMP_COUNT] = {"t48s:red:message", "t48s:green:speaker", "t48s:green:headset", "t48s:red:mute"};
static const char *const old_leds[LAMP_COUNT] = {"t48s:red:power", NULL, NULL, NULL};
static const char *const modes[] = {"off", "on", "blink", "slow"};
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
    char path[96];
    snprintf(path, sizeof path, "/sys/class/leds/%s", led);
    if (access(path, F_OK) && old_leds[which]) led = old_leds[which];
    if (mode == LAMP_BLINK || mode == LAMP_SLOW) {
        /* the kernel's "timer" trigger does the blinking */
        if (led_write(led, "trigger", "timer")) return;
        led_write(led, "delay_on", mode == LAMP_SLOW ? SLOW_MS : BLINK_MS);
        led_write(led, "delay_off", mode == LAMP_SLOW ? SLOW_MS : BLINK_MS);
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
