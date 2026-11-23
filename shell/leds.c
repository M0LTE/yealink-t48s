// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: the lamps. The phone part of the shell says what each lamp
 * should be doing; nothing drives the hardware yet, because the LED driver
 * is not written (issue #3). When it is, leds_apply() is the one place to
 * fill in. Until then the wanted state is kept, logged, and can be read by
 * the simulator's checks ("led.speaker" and so on). */
#include <string.h>
#include "shell.h"

static const char *const names[LAMP_COUNT] = {"power", "speaker", "headset", "mute", "message"};
static const char *const modes[] = {"off", "on", "blink"};
static enum led_mode want[LAMP_COUNT];

/* Make the hardware match. Nothing to do yet: see issue #3. */
static void leds_apply(enum led which, enum led_mode mode)
{
    (void)which;
    (void)mode;
}

void leds_set(enum led which, enum led_mode mode)
{
    if (which >= LAMP_COUNT || want[which] == mode) return;
    want[which] = mode;
    logmsg("lamp %s: %s", names[which], modes[mode]);
    leds_apply(which, mode);
}

const char *leds_state(const char *name)
{
    for (int i = 0; i < LAMP_COUNT; i++)
        if (!strcmp(name, names[i])) return modes[want[i]];
    return NULL;
}
